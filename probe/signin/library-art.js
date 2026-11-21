// Small, public Steam store capsules. Keep artwork independent of the sign-in client.
var libraryArt = (function () {
    "use strict";
    var list = document.getElementById("library-list");
    var database, client, frame, reportTimer;
    var reporting = Promise.resolve();
    var pending = Object.create(null);
    var active = 0;
    var day = 86400000;
    var maxBytes = 32768;
    var stats = { run_id: Date.now(), active: 0, displayed: 0, disk_hits: 0, network_requests: 0, downloaded: 0, failed: 0, peak_active: 0 };

    function db() {
        if (!database) database = new Promise(function (resolve, reject) {
            var request = indexedDB.open("steam-library-art-v1", 1);
            request.onupgradeneeded = function () {
                request.result.createObjectStore("capsules", { keyPath: "id" }).createIndex("saved", "saved");
            };
            request.onsuccess = function () { resolve(request.result); };
            request.onerror = request.onblocked = reject;
        });
        return database;
    }

    function cached(id) {
        return db().then(function (database) {
            return new Promise(function (resolve, reject) {
                var request = database.transaction("capsules").objectStore("capsules").get(id);
                request.onsuccess = function () { resolve(request.result); };
                request.onerror = reject;
            });
        }).catch(function () { return null; });
    }

    function save(id, bytes) {
        // Serialized write transactions keep at most 256 records / 8 MiB of JPEG payload.
        return db().then(function (database) {
            return new Promise(function (resolve, reject) {
                var transaction = database.transaction("capsules", "readwrite");
                transaction.oncomplete = resolve;
                transaction.onabort = transaction.onerror = reject;
                var store = transaction.objectStore("capsules");
                store.put({ id: id, saved: Date.now(), bytes: bytes });
                var count = store.count();
                count.onsuccess = function () {
                    if (count.result > 256) {
                        var oldest = store.index("saved").openKeyCursor();
                        oldest.onsuccess = function () { store.delete(oldest.result.primaryKey); };
                    }
                };
            });
        }).catch(function () {});
    }

    function timed(operation) {
        var timer = setTimeout(function () { operation.cancel(); }, 10000);
        return Promise.resolve(operation).then(function (result) {
            clearTimeout(timer);
            return result;
        }, function (error) {
            clearTimeout(timer);
            throw error;
        });
    }

    function fetchCapsule(id) {
        var http = Windows.Web.Http;
        if (!client) {
            var filter = new http.Filters.HttpBaseProtocolFilter();
            filter.cookieUsageBehavior = http.Filters.HttpCookieUsageBehavior.noCookies;
            filter.allowAutoRedirect = filter.allowUI = false;
            filter.cacheControl.writeBehavior = http.Filters.HttpCacheWriteBehavior.noCache;
            client = new http.HttpClient(filter);
        }
        var uri = new Windows.Foundation.Uri("https://shared.fastly.steamstatic.com/store_item_assets/steam/apps/" +
            id + "/capsule_184x69.jpg");
        stats.network_requests++;
        return timed(client.getAsync(uri, http.HttpCompletionOption.responseHeadersRead)).then(function (response) {
            var status = response.statusCode;
            var length = response.content.headers.contentLength;
            if (status === 404 || status === 410) {
                response.close();
                return null;
            }
            if (status !== 200 || !(length > 0 && length <= maxBytes)) {
                response.close();
                throw new Error("Unavailable capsule");
            }
            return timed(response.content.readAsBufferAsync()).then(function (buffer) {
                response.close();
                if (buffer.length !== length) throw new Error("Unexpected capsule size");
                var crypto = Windows.Security.Cryptography.CryptographicBuffer;
                var raw = atob(crypto.encodeToBase64String(buffer));
                var bytes = new Uint8Array(raw.length);
                for (var i = 0; i < raw.length; i++) bytes[i] = raw.charCodeAt(i);
                stats.downloaded++;
                return bytes.buffer;
            }, function (error) { response.close(); throw error; });
        });
    }

    function decode(bytes) {
        return new Promise(function (resolve, reject) {
            var image = new Image();
            var url = URL.createObjectURL(new Blob([bytes], { type: "image/jpeg" }));
            image.alt = "";
            image.onload = image.onerror = function (event) {
                URL.revokeObjectURL(url);
                image.onload = image.onerror = null;
                if (event.type === "load") resolve(image);
                else reject(new Error("Unreadable capsule"));
            };
            image.src = url;
        });
    }

    function load(id) {
        function download() {
            return fetchCapsule(id).then(function (bytes) {
                if (!bytes) return save(id, null).then(function () { return null; });
                return decode(bytes).then(function (image) {
                    return save(id, bytes).then(function () { return image; });
                });
            });
        }
        return cached(id).then(function (record) {
            if (!record || Date.now() - record.saved >= (record.bytes ? 30 : 1) * day) return download();
            if (!record.bytes) return null;
            return decode(record.bytes).then(function (image) {
                stats.disk_hits++;
                return image;
            }, download);
        });
    }

    function report() {
        clearTimeout(reportTimer);
        reportTimer = setTimeout(function () {
            stats.active = active;
            var text = Object.keys(stats).map(function (key) { return "art_" + key + "=" + stats[key]; }).join("\r\n");
            reporting = reporting.then(function () {
                var storage = Windows.Storage;
                return storage.ApplicationData.current.localFolder.createFileAsync("steam-library-art.txt",
                    storage.CreationCollisionOption.openIfExists).then(function (file) {
                    return storage.FileIO.writeTextAsync(file, text + "\r\n");
                });
            }).catch(function () {});
        }, 250);
    }

    function start(button) {
        var entry = button.entry;
        var id = String(entry.id);
        pending[id] = true;
        active++;
        stats.peak_active = Math.max(stats.peak_active, active);
        load(id).then(function (image) {
            if (!image) {
                entry.artRetry = Date.now() + day;
                stats.failed++;
                return;
            }
            // Search may have rebuilt the button during a fetch.
            var destination = list.contains(button) ? button : list.querySelector('[data-art-id="' + id + '"]');
            if (destination) {
                destination.firstElementChild.appendChild(image);
                stats.displayed++;
            }
        }).catch(function () {
            entry.artRetry = Date.now() + 60000;
            stats.failed++;
        }).then(function () {
            delete pending[id];
            active--;
            schedule();
            report();
        });
    }

    function scan() {
        frame = null;
        if (!list.clientHeight) return;
        var bounds = list.getBoundingClientRect();
        var near = [];
        for (var i = 0; i < list.children.length; i++) {
            var button = list.children[i];
            var rect = button.getBoundingClientRect();
            var distance = Math.max(bounds.top - rect.bottom, rect.top - bounds.bottom, 0);
            var slot = button.firstElementChild;
            if (slot.firstChild && distance > list.clientHeight) slot.removeChild(slot.firstChild);
            if (slot.firstChild || pending[button.entry.id] || button.entry.artRetry > Date.now() ||
                !button.hasAttribute("data-art-id")) continue;
            if (distance <= rect.height) near.push({ button: button, distance: distance });
        }
        near.sort(function (a, b) { return a.distance - b.distance; });
        for (var n = 0; n < near.length && active < 3; n++) {
            if (!pending[near[n].button.entry.id]) start(near[n].button);
        }
    }

    function schedule() { if (frame == null) frame = requestAnimationFrame(scan); }

    function decorate(button, entry) {
        var art = document.createElement("span");
        art.className = "game-art";
        art.setAttribute("aria-hidden", "true");
        art.setAttribute("data-initial", entry.name.charAt(0).toUpperCase());
        button.insertBefore(art, button.firstChild);
        if (/^[1-9][0-9]{0,9}$/.test(String(entry.id))) button.setAttribute("data-art-id", entry.id);
    }

    list.addEventListener("scroll", schedule);
    window.addEventListener("resize", schedule);
    return { decorate: decorate, schedule: schedule };
})();
