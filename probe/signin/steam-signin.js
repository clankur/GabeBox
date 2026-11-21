(function () {
    "use strict";

    var status = document.getElementById("status");
    var canvas = document.getElementById("code");
    var retry = document.getElementById("retry");
    var cancel = document.getElementById("cancel");
    var rememberRow = document.getElementById("remember-session");
    var remember = document.getElementById("remember");
    var signin = document.getElementById("signin");
    var library = document.getElementById("library");
    var libraryCount = document.getElementById("library-count");
    var list = document.getElementById("library-list");
    var search = document.getElementById("library-search");
    var empty = document.getElementById("library-empty");
    var entryName = document.getElementById("entry-name");
    var entryType = document.getElementById("entry-type");
    var signOut = document.getElementById("sign-out");
    var play = document.getElementById("play");
    var download = document.getElementById("download");
    var downloadStatus = document.getElementById("download-status");
    var current;
    var entries = [];
    var selected;
    var transfer;
    var launching = false;
    var manifestFolder;

    function httpClient() {
        var filter = new Windows.Web.Http.Filters.HttpBaseProtocolFilter();
        filter.allowAutoRedirect = false;
        return new Windows.Web.Http.HttpClient(filter);
    }

    function bounded(operation) {
        var timer = setTimeout(function () { operation.cancel(); }, 10000);
        return operation.then(function (value) {
            clearTimeout(timer);
            return value;
        }, function (error) {
            clearTimeout(timer);
            throw error;
        });
    }

    function byteString(tag, value) {
        return String.fromCharCode(tag) + unsignedVarint(String(value.length)) + value;
    }

    function unsignedVarint(decimal) {
        // Use decimal division to preserve Valve's entire uint64 client ID in the Xbox web host.
        if (!/^[0-9]{1,20}$/.test(decimal)) throw new Error("Invalid identifier");
        var output = "";
        do {
            var remainder = 0;
            var quotient = "";
            for (var i = 0; i < decimal.length; i++) {
                var part = remainder * 10 + Number(decimal.charAt(i));
                var digit = Math.floor(part / 128);
                if (quotient || digit) quotient += digit;
                remainder = part % 128;
            }
            output += String.fromCharCode(remainder + (quotient ? 128 : 0));
            decimal = quotient;
        } while (decimal);
        return output;
    }

    function steamRequest(client, method, message) {
        var form = new Windows.Web.Http.HttpStringContent(
            "input_protobuf_encoded=" + encodeURIComponent(btoa(message)),
            Windows.Storage.Streams.UnicodeEncoding.utf8, "application/x-www-form-urlencoded"
        );
        var target = new Windows.Foundation.Uri(
            "https://api.steampowered.com/IAuthenticationService/" + method + "/v1/?format=json"
        );
        return bounded(client.postAsync(target, form)).then(function (response) {
            form.close();
            var code = response.headers.hasKey("X-EResult") ? response.headers.lookup("X-EResult") : "";
            if (!response.isSuccessStatusCode || (code && code !== "1")) {
                response.close();
                throw new Error("Valve rejected the request");
            }
            return bounded(response.content.readAsStringAsync()).then(function (body) {
                response.close();
                return JSON.parse(body).response;
            }, function () {
                response.close();
                throw new Error("Valve response could not be read");
            });
        }, function () {
            form.close();
            throw new Error("Valve is unreachable");
        });
    }

    function showCode(url) {
        if (!/^https:\/\/s\.team\/q\/[a-zA-Z0-9/_-]+$/.test(url)) throw new Error("Invalid Valve code");
        var qr = qrcode(0, "M");
        qr.addData(url);
        qr.make();
        var size = qr.getModuleCount();
        var scale = 12;
        canvas.width = canvas.height = (size + 8) * scale;
        var context = canvas.getContext("2d");
        context.fillStyle = "#fff";
        context.fillRect(0, 0, canvas.width, canvas.height);
        context.fillStyle = "#000";
        for (var y = 0; y < size; y++) {
            for (var x = 0; x < size; x++) {
                if (qr.isDark(y, x)) context.fillRect((x + 4) * scale, (y + 4) * scale, scale, scale);
            }
        }
        canvas.hidden = false;
    }

    function clearCode() {
        canvas.hidden = true;
        canvas.width = canvas.height = 0;
    }

    function steamTokenClaims(refresh) {
        var part = refresh.split(".")[1];
        if (!part) throw new Error("Valve did not return an account identity");
        part = part.replace(/-/g, "+").replace(/_/g, "/");
        while (part.length % 4) part += "=";
        var claims = JSON.parse(atob(part));
        var id = claims.sub;
        if (typeof id !== "string" || !/^[0-9]{17,20}$/.test(id)) throw new Error("Invalid Valve account identity");
        var expiry = claims.exp;
        if (typeof expiry !== "number" || Math.floor(expiry) !== expiry || expiry <= 0 || expiry > 4294967295) {
            expiry = 0;
        }
        return { id: id, expiry: expiry };
    }

    function finish(attempt, display, marker) {
        if (current !== attempt || attempt.done) return;
        attempt.done = true;
        clearTimeout(attempt.timer);
        attempt.client.close();
        clearCode();
        cancel.hidden = true;
        rememberRow.hidden = true;
        retry.hidden = false;
        retry.focus();
        status.textContent = display;
        if (marker && attempt.file) {
            Windows.Storage.FileIO.writeTextAsync(attempt.file, attempt.prefix + marker +
                "\r\nstatus=steam-core-complete\r\n").done(null, function () {});
        }
    }

    function select(button) {
        if (selected) selected.setAttribute("aria-pressed", "false");
        selected = button;
        if (button) button.setAttribute("aria-pressed", "true");
        entryName.textContent = button ? button.entry.name : "";
        entryType.textContent = button ? button.entry.type : "";
        updateActions();
        if (button && button.entry.game) checkLocalInstall(button.entry);
    }

    function updateActions() {
        var focused = document.activeElement;
        var entry = selected && selected.entry;
        play.hidden = !entry || !entry.game || !entry.installed;
        download.hidden = !entry || !entry.game || !!entry.installed;
        play.disabled = download.disabled = launching || !!transfer;
        if ((focused === play || focused === download) && (focused.hidden || focused.disabled)) {
            (selected || search).focus();
        }
    }

    function checkLocalInstall(entry) {
        if (entry.installed || entry.checked || !manifestFolder) return;
        entry.checked = true;
        var storage = Windows.Storage;
        manifestFolder.then(function (folder) {
            if (!folder) return null;
            return folder.tryGetItemAsync("appmanifest_" + entry.id + ".acf");
        }).then(function (file) {
            return file ? storage.FileIO.readTextAsync(file) : "";
        }).done(function (text) {
            var match = /^\s*"StateFlags"\s*"([0-9]+)"\s*$/mi.exec(text);
            if (match && (+match[1] & 0x7000a6) === 4) {
                entry.installed = true;
                if (selected && selected.entry === entry) updateActions();
            }
        }, function () { entry.checked = false; });
    }

    function renderList() {
        var filter = search.value.trim().toLowerCase();
        var fragment = document.createDocumentFragment();
        entries.forEach(function (entry) {
            if (filter && entry.name.toLowerCase().indexOf(filter) < 0) return;
            var button = document.createElement("button");
            button.type = "button";
            button.textContent = entry.name;
            button.entry = entry;
            button.setAttribute("aria-pressed", "false");
            fragment.appendChild(button);
        });
        list.textContent = "";
        list.appendChild(fragment);
        empty.hidden = !!list.firstElementChild;
        select(list.firstElementChild);
    }

    function runGameLaunch(attempt, entry) {
        if (transfer || launching) return;
        var fileIO = Windows.Storage.FileIO;
        var complete = "status=steam-core-complete\r\n";
        var report = attempt.libraryReport + "steam_launch_run_id=" + Date.now() + "\r\n";
        launching = true;
        signOut.disabled = true;
        updateActions();
        downloadStatus.textContent = "Starting " + entry.name + "…";

        function finish(result) {
            launching = false;
            signOut.disabled = false;
            updateActions();
            var message = " launch could not be confirmed.";
            if (result.indexOf("steam_launch=window-ready\r\n") >= 0) message = " started.";
            if (result.indexOf("steam_launch=not-installed\r\n") >= 0) {
                message = " is on disk, but Steam could not reopen its install.";
            }
            if (result.indexOf("steam_launch=client-rejected\r\n") >= 0) message = " could not start in Steam.";
            if (/steam_launch=(?:dependency-install-rejected|dependency-install-incomplete)\r\n/.test(result) ||
                    result.indexOf("steam_launch_client_reason=dependency-not-ready\r\n") >= 0) {
                message = " is waiting for a required Steam component.";
            }
            if (result.indexOf("steam_launch_client_reason=invalid-platform\r\n") >= 0) {
                message = " has a Windows build that Steam refused on this Xbox.";
            }
            downloadStatus.textContent = entry.name + message;
            return fileIO.writeTextAsync(attempt.file, report + result + complete);
        }

        return fileIO.writeTextAsync(attempt.file, report + "status=steam-game-launch-starting\r\n").then(function () {
            return XboxProbe.Native.ProbeRunner.launchSteamGame(entry.id);
        }).then(finish, function () { return finish("steam_launch=failed\r\n"); });
    }

    function completeLibrary(attempt) {
        var storage = Windows.Storage;
        var packageInfo = Windows.ApplicationModel.Package.current;
        var values = storage.ApplicationData.current.localSettings.values;
        var versionKey = "steamLaunchProbePackage";
        var complete = "status=steam-core-complete\r\n";
        function save(checkpoint) {
            return storage.FileIO.writeTextAsync(attempt.file, attempt.libraryReport + checkpoint);
        }

        if (values.hasKey(versionKey) && values.lookup(versionKey) === packageInfo.id.fullName) return save(complete);
        return packageInfo.installedLocation.tryGetItemAsync("steam-launch-app.txt").then(function (file) {
            if (!file) return save(complete);
            return storage.FileIO.readTextAsync(file).then(function (text) {
                var requested = text.trim();
                if (!/^\d{1,8}$/.test(requested) || +requested < 1 || +requested > 16777215) {
                    return save("steam_launch=invalid-game\r\n" + complete);
                }
                values.insert(versionKey, packageInfo.id.fullName);
                var game = entries.filter(function (entry) { return entry.id === +requested; })[0];
                if (!game) return save("steam_launch=game-unavailable\r\n" + complete);
                return runGameLaunch(attempt, game);
            });
        }, function () { return save("steam_launch_config=unavailable\r\n" + complete); });
    }

    function finishNative(attempt, payload) {
        return Windows.Storage.FileIO.readTextAsync(attempt.file).then(function (report) {
            if (payload) {
                entries = JSON.parse(payload);
                entries.sort(function (a, b) { return a.name.localeCompare(b.name); });
                manifestFolder = Windows.Storage.ApplicationData.current.localFolder
                    .tryGetItemAsync("SteamLibrary").then(function (folder) {
                        return folder ? folder.tryGetItemAsync("steamapps") : null;
                    });
                libraryCount.textContent = entries.length + (entries.length === 1 ? " title" : " titles");
                search.value = "";
                renderList();
                signin.hidden = true;
                library.hidden = false;
                if ("gamepadInputEmulation" in navigator) navigator.gamepadInputEmulation = "keyboard";
                (list.firstElementChild || search).focus();
                attempt.done = true;
                attempt.client.close();
                if (report.indexOf("steam_session_cache=unavailable\r\n") >= 0) {
                    downloadStatus.textContent = "This Xbox could not save the Steam sign-in.";
                }
                attempt.libraryReport = report.replace("status=steam-library-rendering\r\n", "steam_library_rendered=yes\r\n");
                return completeLibrary(attempt);
            } else if (report.indexOf("steam_signin=authenticated\r\n") >= 0) {
                finish(attempt, "Steam approved sign-in. The library did not finish loading.");
            } else {
                finish(attempt, "Steam Guard approved the request. The native client did not complete sign-in.");
            }
        });
    }

    function submitApproval(attempt, reply) {
        var claims = steamTokenClaims(reply.refresh_token);
        var optedIn = remember.checked;
        clearCode();
        cancel.hidden = true;
        rememberRow.hidden = true;
        status.textContent = "Connecting to Steam and reading your library…";
        var approved = attempt.prefix + "steam_approval=received\r\n";
        var operation = XboxProbe.Native.ProbeRunner.runSteamSignIn(
            attempt.file, approved, reply.account_name, reply.refresh_token, claims.id, optedIn, claims.expiry
        );
        return operation.then(function (payload) { return finishNative(attempt, payload); });
    }

    var downloadMessages = {
        "starting": "Preparing the download",
        "queued": "Queued in Steam",
        "resuming": "Steam is retrying the paused download",
        "paused": "Steam reports the download is paused",
        "downloading": "Downloading from Steam",
        "staging": "Writing game files",
        "installed": "Download complete",
        "stalled": "Steam stopped reporting progress",
        "session-lost": "Steam disconnected. Sign in again to retry",
        "no-license": "Steam could not confirm access to this game",
        "library-folder-error": "Steam could not open this Xbox storage folder",
        "install-rejected": "Steam declined the download",
        "install-path-error": "Steam reported a different install location",
        "interrupted": "Download monitoring stopped on sign-out"
    };

    function storageCheckpoint(stage) {
        try {
            return XboxProbe.Native.ProbeRunner.steamStorageInfo().replace(/^(?=\w)/gm, "steam_storage_" + stage + "_");
        } catch (error) {
            return "steam_storage_" + stage + "_error=unavailable\r\n";
        }
    }

    function updateDownload(job, checkpoint) {
        if (transfer !== job) return;
        var parts = /^(\w+(?:-\w+)*)(?::(-?\d+))?$/.exec(checkpoint);
        var stage = parts ? parts[1] : "failed";
        var detail = parts ? parts[2] : "";
        var active = stage === "starting" || stage === "queued" || stage === "resuming" ||
            stage === "paused" || stage === "downloading" || stage === "staging";
        if (checkpoint !== job.last) {
            job.last = checkpoint;
            var message = downloadMessages[stage] || "Steam could not start or track the download";
            downloadStatus.textContent = job.entry.name + " · " + message;
            if (job.file) {
                var report = job.prefix + "steam_download=" + stage + "\r\n";
                if (detail) {
                    var metric = stage === "install-rejected" || stage === "library-folder-error" ? "result" : "state";
                    report += "steam_download_" + metric + "=" + detail + "\r\n";
                }
                if (!active) report += storageCheckpoint("after");
                report += "status=steam-download-" + (active ? "in-progress" : "complete") + "\r\n";
                var write = function () { return Windows.Storage.FileIO.writeTextAsync(job.file, report); };
                job.write = job.write ? job.write.then(write, write) : write();
                job.write.done(null, function () {});
            }
        }
        if (active) return;
        clearTimeout(job.timer);
        if (stage === "installed") job.entry.installed = true;
        transfer = null;
        select(selected);
    }

    function pollDownload(job) {
        if (transfer !== job) return;
        try {
            updateDownload(job, XboxProbe.Native.ProbeRunner.steamDownloadStatus());
        } catch (error) {
            updateDownload(job, "failed");
        }
        if (transfer === job) job.timer = setTimeout(function () { pollDownload(job); }, 2000);
    }

    function startDownload() {
        if (transfer || launching || !selected || !selected.entry.game || selected.entry.installed) return;
        var job = { entry: selected.entry, id: Date.now() };
        transfer = job;
        updateActions();
        signOut.disabled = true;
        updateDownload(job, "starting");
        Windows.Storage.ApplicationData.current.localFolder.createFileAsync(
            "steam-download-report.txt", Windows.Storage.CreationCollisionOption.replaceExisting
        ).then(function (file) {
            job.file = file;
            job.prefix = "probe=steam-download\r\nrun_id=" + job.id + "\r\n" + storageCheckpoint("before");
            return Windows.Storage.FileIO.writeTextAsync(file, job.prefix +
                "steam_download=starting\r\nstatus=steam-download-in-progress\r\n");
        }).then(function () {
            return XboxProbe.Native.ProbeRunner.queueSteamDownload(job.entry.id);
        }).done(function (checkpoint) {
            signOut.disabled = false;
            updateDownload(job, checkpoint);
            if (transfer === job) job.timer = setTimeout(function () { pollDownload(job); }, 1000);
        }, function () {
            signOut.disabled = false;
            updateDownload(job, "failed");
        });
    }

    function schedulePoll(attempt, challenge) {
        if (current !== attempt || attempt.done) return;
        if (Date.now() >= attempt.deadline) {
            finish(attempt, "The code expired. Get a new code to try again.", "steam_approval=expired");
            return;
        }

        var interval = Math.max(2, Number(challenge.interval) || 5) * 1000;
        attempt.timer = setTimeout(function () {
            var message = String.fromCharCode(8) + unsignedVarint(String(challenge.client_id)) +
                byteString(18, atob(challenge.request_id));
            steamRequest(attempt.client, "PollAuthSessionStatus", message).then(function (reply) {
                if (current !== attempt || attempt.done) return;
                if (reply.refresh_token && reply.account_name) return submitApproval(attempt, reply);
                if (reply.new_challenge_url) showCode(reply.new_challenge_url);
                if (reply.new_client_id) challenge.client_id = reply.new_client_id;
                schedulePoll(attempt, challenge);
            }).done(null, function () {
                finish(attempt, "Steam could not finish this sign-in. Get a new code to retry.", "steam_signin=failed");
            });
        }, interval);
    }

    function prepareQr(attempt) {
        attempt.deadline = Date.now() + 150000;
        status.textContent = "Preparing Steam Guard sign-in…";
        return Windows.Storage.FileIO.writeTextAsync(attempt.file,
            attempt.prefix + "status=steam-approval-starting\r\n").then(function () {
            var name = "Xbox Dev Mode Steam client";
            var device = byteString(10, name) + String.fromCharCode(16, 1);
            return steamRequest(attempt.client, "BeginAuthSessionViaQR", byteString(26, device));
        }).then(function (challenge) {
            if (!challenge || !challenge.client_id || typeof challenge.request_id !== "string") {
                throw new Error("Incomplete Steam Guard challenge");
            }
            showCode(challenge.challenge_url);
            status.textContent = "Scan this code with Steam Guard in the Steam Mobile app, then approve this Xbox.";
            cancel.hidden = false;
            rememberRow.hidden = false;
            remember.focus();
            return Windows.Storage.FileIO.writeTextAsync(attempt.file, attempt.prefix +
                "steam_qr_challenge=displayed\r\nstatus=steam-approval-ready\r\n").then(function () {
                schedulePoll(attempt, challenge);
            });
        });
    }

    function start() {
        if (current && !current.done) return;
        retry.hidden = true;
        remember.checked = false;
        status.textContent = "Checking for a saved Steam session…";
        var attempt = {
            client: httpClient(),
            prefix: "probe=steam-signin\r\nrun_id=" + Date.now() + "\r\n"
        };
        current = attempt;
        Windows.Storage.ApplicationData.current.localFolder.createFileAsync(
            "probe-report.txt", Windows.Storage.CreationCollisionOption.replaceExisting
        ).then(function (file) {
            attempt.file = file;
            return Windows.Storage.FileIO.writeTextAsync(file, attempt.prefix + "status=steam-session-checking\r\n");
        }).then(function () {
            return XboxProbe.Native.ProbeRunner.resumeSteamSignIn(attempt.file, attempt.prefix).then(null, function () {
                return "fallback";
            });
        }).then(function (payload) {
            if (payload && payload !== "fallback") return finishNative(attempt, payload);
            if (payload === "fallback") attempt.prefix += "steam_session_restore=fallback\r\n";
            return prepareQr(attempt);
        }).done(null, function () {
            finish(attempt, "Steam could not prepare an approval code. Try again.", "steam_qr_challenge=failed");
        });
    }

    retry.addEventListener("click", start);
    cancel.addEventListener("click", function () {
        finish(current, "Sign-in cancelled.", "steam_approval=cancelled");
    });
    search.addEventListener("input", renderList);
    list.addEventListener("focusin", function (event) {
        if (event.target.entry) select(event.target);
    });
    list.addEventListener("click", function (event) {
        if (event.target.entry) select(event.target);
    });
    var originalInputMode = navigator.gamepadInputEmulation;
    // EdgeHTML can emit arrow keys, Navigation keys, or gamepad virtual keys.
    var directions = {
        37: "left", 140: "left", 205: "left", 214: "left",
        38: "up", 138: "up", 203: "up", 211: "up",
        39: "right", 141: "right", 206: "right", 213: "right",
        40: "down", 139: "down", 204: "down", 212: "down"
    };
    library.addEventListener("keydown", function (event) {
        var target = event.target;
        var key = event.keyCode;
        if (event.defaultPrevented || event.altKey || event.ctrlKey || event.metaKey || event.shiftKey) return;
        if ((key === 142 || key === 195) && target.tagName === "BUTTON") {
            event.preventDefault();
            if (!event.repeat && !target.disabled) target.click();
            return;
        }
        var direction = directions[key];
        var next;
        if (key === 27 || key === 143 || key === 196) {
            if (target === search) return;
            next = target.entry ? search : (selected || search);
        } else if (!direction) {
            return;
        } else if (target === search) {
            if (direction === "up") next = signOut;
            else if (direction === "down") next = list.firstElementChild;
            else return; // Keep horizontal caret movement and native search input.
        } else if (target === signOut) {
            if (direction === "down" || direction === "left") next = search;
        } else if (target.entry) {
            if (direction === "up") next = target.previousElementSibling || search;
            if (direction === "down") next = target.nextElementSibling;
            if (direction === "right") next = play.hidden ? download : play;
        } else if (target === play || target === download) {
            if (direction === "left") next = selected;
            if (direction === "up") next = signOut;
        } else {
            return;
        }
        event.preventDefault();
        if (next && !next.hidden && !next.disabled) next.focus();
    });
    download.addEventListener("click", startDownload);
    play.addEventListener("click", function () {
        if (selected && selected.entry.installed) runGameLaunch(current, selected.entry);
    });
    signOut.addEventListener("click", function () {
        if (transfer) updateDownload(transfer, "interrupted");
        launching = true;
        signOut.disabled = true;
        downloadStatus.textContent = "Signing out of Steam…";
        updateActions();
        function endSignOut(cleared) {
            launching = false;
            signOut.disabled = false;
            entries = [];
            manifestFolder = null;
            renderList();
            if ("gamepadInputEmulation" in navigator) navigator.gamepadInputEmulation = originalInputMode;
            library.hidden = true;
            signin.hidden = false;
            downloadStatus.textContent = "";
            status.textContent = cleared ? "Signed out of Steam." : "Could not confirm removal of the saved Steam session.";
            retry.hidden = false;
            retry.focus();
        }
        XboxProbe.Native.ProbeRunner.signOutSteam().done(endSignOut, function () { endSignOut(false); });
    });
    start();
})();
