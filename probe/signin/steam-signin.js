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
    var signOut = document.getElementById("sign-out");
    var play = document.getElementById("play");
    var download = document.getElementById("download");
    var cloudSaves = document.getElementById("cloud-saves");
    var cloudPanel = document.getElementById("cloud-saves-panel");
    var cloudRestore = document.getElementById("cloud-saves-open");
    var cloudStatus = document.getElementById("cloud-saves-status");
    var downloadStatus = document.getElementById("download-status");
    var bigPictureMode = document.documentElement.getAttribute("data-steam-big-picture") === "true";
    var bigPictureStatus;
    function showBigPictureStatus(message) {
        if (!bigPictureStatus) {
            bigPictureStatus = document.createElement("p");
            bigPictureStatus.id = "big-picture-status";
            bigPictureStatus.setAttribute("role", "status");
            document.body.appendChild(bigPictureStatus);
        }
        bigPictureStatus.textContent = message;
        bigPictureStatus.hidden = !message;
    }
    var current;
    var entries = [];
    var selected;
    var transfer;
    var launching = false;
    var signingOut = false;
    var restoring = false;
    var manifestFolder;
    // Only packaged adapters can be selected. Steam library metadata supplies the AppID.
    var hostedGames = window.xcomDosRuntime ? [{
        appId: window.xcomDosRuntime.appId,
        backend: "dosbox",
        phaseField: "steam_dos_phase",
        authorizedLine: "steam_dos_install=verified",
        authorize: function (runner, appId, attempt) {
            return runner.authorizeDosGame(appId, attempt.file, attempt.report);
        },
        start: window.xcomDosRuntime.start
    }] : [];
    if (window.steamSdlRuntimes) hostedGames = hostedGames.concat(window.steamSdlRuntimes);

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
        drawCode(canvas, url);
        canvas.hidden = false;
    }

    function drawCode(target, url) {
        var qr = qrcode(0, "M");
        qr.addData(url);
        qr.make();
        var size = qr.getModuleCount();
        var scale = 12;
        target.width = target.height = (size + 8) * scale;
        var context = target.getContext("2d");
        context.fillStyle = "#fff";
        context.fillRect(0, 0, target.width, target.height);
        context.fillStyle = "#000";
        for (var y = 0; y < size; y++) {
            for (var x = 0; x < size; x++) {
                if (qr.isDark(y, x)) context.fillRect((x + 4) * scale, (y + 4) * scale, scale, scale);
            }
        }
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
        if (selected !== button) closeCloudSaves();
        selected = button;
        if (button) button.setAttribute("aria-pressed", "true");
        entryName.textContent = button ? button.entry.name : "";
        updateActions();
        if (button && button.entry.game) checkLocalInstall(button.entry);
    }

    function closeCloudSaves() {
        cloudPanel.hidden = true;
        cloudSaves.setAttribute("aria-expanded", "false");
        cloudStatus.textContent = "";
    }

    function updateActions() {
        var focused = document.activeElement;
        var entry = selected && selected.entry;
        play.hidden = !entry || !entry.game || !entry.installed;
        download.hidden = !entry || !entry.game || !!entry.installed;
        cloudSaves.hidden = !entry || !entry.game || !entry.installed || !steamCloud.supported(entry.id);
        play.disabled = download.disabled = cloudSaves.disabled = launching || restoring || !!transfer;
        cloudRestore.disabled = cloudSaves.disabled;
        if (cloudSaves.hidden || launching || transfer) closeCloudSaves();
        if (((focused === play || focused === download || focused === cloudSaves) &&
             (focused.hidden || focused.disabled)) || (focused === cloudRestore && cloudPanel.hidden)) {
            (selected || search).focus();
        }
    }

    function primaryAction() {
        return play.hidden ? download : play;
    }

    function readLocalInstall(entry) {
        var storage = Windows.Storage;
        return Promise.resolve(manifestFolder).then(function (folder) {
            return folder ? folder.tryGetItemAsync("appmanifest_" + entry.id + ".acf") : null;
        }).then(function (file) {
            return file ? storage.FileIO.readTextAsync(file) : "";
        }).then(function (text) {
            var match = /^\s*"StateFlags"\s*"([0-9]+)"\s*$/mi.exec(text);
            return !!match && (+match[1] & 0x7000a6) === 4;
        });
    }

    function checkLocalInstall(entry) {
        if (entry.installed || !manifestFolder) return Promise.resolve(!!entry.installed);
        if (entry.installCheck) return entry.installCheck;
        entry.installCheck = readLocalInstall(entry).then(function (installed) {
            if (installed) {
                entry.installed = true;
                if (selected && selected.entry === entry) updateActions();
            }
            return !!entry.installed;
        }, function () {
            entry.installCheck = null;
            return !!entry.installed;
        });
        return entry.installCheck;
    }

    function renderList() {
        var filter = search.value.trim().toLowerCase();
        var fragment = document.createDocumentFragment();
        entries.forEach(function (entry) {
            if (filter && entry.name.toLowerCase().indexOf(filter) < 0) return;
            var button = document.createElement("button");
            var name = document.createElement("span");
            button.type = "button";
            name.className = "tile-name";
            name.textContent = entry.name;
            button.appendChild(name);
            button.entry = entry;
            libraryArt.decorate(button, entry);
            button.setAttribute("aria-pressed", "false");
            fragment.appendChild(button);
        });
        list.textContent = "";
        list.appendChild(fragment);
        empty.hidden = !!list.firstElementChild;
        empty.textContent = entries.length ? "No matching games." : "No games in this library.";
        select(list.firstElementChild);
        libraryArt.schedule();
    }

    function runGameLaunch(attempt, entry, automaticTrial, onExit) {
        if (transfer || launching || restoring) return;
        var fileIO = Windows.Storage.FileIO;
        var complete = "status=steam-core-complete\r\n";
        var launch = steamGameLaunch.forGame(XboxProbe.Native.ProbeRunner, entry.id, hostedGames);
        var report = attempt.libraryReport + "steam_launch_run_id=" + Date.now() + "\r\n" +
            "steam_launch_app_id=" + entry.id + "\r\nsteam_launch_backend=" + launch.backend + "\r\n";
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
            if (result.indexOf("steam_dos=guest-started\r\n") >= 0) message = " started in DOSBox.";
            if (result.indexOf("steam_sdl_host_frame=presented\r\n") >= 0) message = " started.";
            if (result.indexOf("steam_launch=not-installed\r\n") >= 0) {
                message = " is on disk, but Steam could not reopen its install.";
            }
            if (result.indexOf("steam_launch=client-rejected\r\n") >= 0) message = " could not start in Steam.";
            if (result.indexOf("steam_launch=save-recovery-error\r\n") >= 0) message = " needs save recovery before it can start.";
            if (/steam_launch=(?:dependency-install-rejected|dependency-install-incomplete)\r\n/.test(result) ||
                    result.indexOf("steam_launch_client_reason=dependency-not-ready\r\n") >= 0) {
                message = " is waiting for a required Steam component.";
            }
            if (result.indexOf("steam_launch_client_reason=invalid-platform\r\n") >= 0) {
                message = " has a Windows build that Steam refused on this Xbox.";
            }
            downloadStatus.textContent = entry.name + message;
            return fileIO.writeTextAsync(attempt.file, report + result + complete).then(function () { return result; });
        }

        return fileIO.writeTextAsync(attempt.file, report + "status=steam-game-launch-starting\r\n").then(function () {
            return launch.start({
                file: attempt.file,
                report: report,
                automaticTrial: automaticTrial === true,
                bigPicture: bigPictureMode,
                onExit: onExit,
                checkpoint: function (detail) {
                    return fileIO.writeTextAsync(attempt.file, report + detail + "status=steam-game-launch-starting\r\n");
                }
            });
        }).then(finish, function () { return finish("steam_launch=failed\r\n"); });
    }

    function openBigPicture(attempt) {
        var host = window.steamBigPictureHost;
        if (!host || !window.steamBigPictureBroker) {
            showBigPictureStatus("Steam Big Picture is missing from this package. Reinstall the Big Picture build.");
            return;
        }
        var games = entries;
        function currentSession() {
            return current === attempt && games === entries && !signingOut;
        }
        function installedGames() {
            return Promise.resolve(manifestFolder).then(function (folder) {
                return folder ? folder.getFilesAsync() : null;
            }).then(function (files) {
                var manifests = new Set();
                for (var i = 0; files && i < files.size; i++) {
                    var match = /^appmanifest_([1-9][0-9]*)\.acf$/i.exec(files.getAt(i).name);
                    if (match) manifests.add(+match[1]);
                }
                var installed = new Set();
                return Promise.all(games.filter(function (game) { return manifests.has(game.id); }).map(function (game) {
                    return readLocalInstall(game).then(function (local) { if (local) installed.add(game.id); });
                })).then(function () { return installed; });
            });
        }
        function resume(appId) {
            if (currentSession()) return Promise.resolve().then(function () { return host.resume(appId); }).catch(function () {
                showBigPictureStatus("Steam Big Picture could not resume. Reopen the app to retry.");
            });
        }
        function playGame(game) {
            var resumed = false;
            function restore() {
                if (resumed) return;
                resumed = true;
                return resume(game.id);
            }
            return Promise.resolve(runGameLaunch(attempt, game, false, restore)).then(function (result) {
                if (/^steam_sdl_host_frame=presented\r?$|^steam_dos=guest-started\r?$/m.test(result)) return "presented";
                if (/^steam_launch=window-ready\r?$/m.test(result)) {
                    // Desktop Play owns its own window. Reveal Big Picture when the
                    // operating system gives the shell foreground focus again.
                    function returned() {
                        if (document.hidden) return;
                        window.removeEventListener("focus", returned);
                        document.removeEventListener("visibilitychange", returned);
                        restore();
                    }
                    window.addEventListener("focus", returned);
                    document.addEventListener("visibilitychange", returned);
                    return "window-ready";
                }
                restore();
                if (/^steam_launch=no-license\r?$/m.test(result)) return "not-owned";
                if (/^steam_launch=not-installed\r?$/m.test(result)) return "not-installed";
                if (/^steam_launch=busy\r?$/m.test(result)) return "busy";
                if (/^steam_launch=session-lost\r?$/m.test(result)) return "signed-out";
                return "failed";
            }, function () { restore(); return "failed"; });
        }
        var broker = steamBigPictureBroker.create({
            current: currentSession, games: games, installedGames: installedGames,
            steamId: function () { return XboxProbe.Native.ProbeRunner.steamSessionSteamId(); },
            isInstalled: function (game) {
                return readLocalInstall(game).then(function (local) {
                    if (currentSession()) game.installed = local;
                    return local;
                });
            },
            busy: function () {
                return !!transfer || launching || restoring || !document.getElementById("sdl-game").hidden ||
                    !!document.getElementById("xcom-game");
            },
            install: startDownload, play: playGame, signOut: signOutCurrent
        });
        function failed() {
            if (currentSession()) showBigPictureStatus("Steam Big Picture could not open. Reopen the app to retry.");
        }
        return Promise.resolve().then(function () { return host.open(broker, failed); }).then(function () {
            if (currentSession()) showBigPictureStatus("");
        }, failed);
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

        function runConfiguredTrial(file) {
            if (!file) return save(complete);
            return storage.FileIO.readTextAsync(file).then(function (text) {
                var config = steamGameLaunch.parseTrial(text);
                if (!config) return save("steam_launch=invalid-game\r\n" + complete);
                if (!config.repeat) {
                    if (values.hasKey(versionKey) && values.lookup(versionKey) === packageInfo.id.fullName) return save(complete);
                    values.insert(versionKey, packageInfo.id.fullName);
                }
                var game = entries.filter(function (entry) { return entry.id === config.appId; })[0];
                if (!game) return save("steam_launch=game-unavailable\r\n" + complete);
                if (!config.install) return runGameLaunch(attempt, game, true);
                return save("status=steam-game-install-checking\r\n").then(function () {
                    return checkLocalInstall(game);
                }).then(function (installed) {
                    if (installed) return "already-installed";
                    return save("status=steam-game-installing\r\n").then(function () {
                        return startDownload(game);
                    });
                }).then(function (stage) {
                    attempt.libraryReport += "steam_launch_install=" + stage + "\r\n";
                    if (stage !== "installed" && stage !== "already-installed") return save(complete);
                    return runGameLaunch(attempt, game, true);
                });
            });
        }

        return packageInfo.installedLocation.tryGetItemAsync("steam-cloud-preview.txt").then(function (preview) {
            if (preview) {
                return runCloudRestore(646570, true).then(function (result) {
                    return save("steam_cloud_preview=" + result + "\r\n" + complete);
                });
            }
            return packageInfo.installedLocation.tryGetItemAsync("steam-launch-app.txt").then(runConfiguredTrial);
        }).then(null, function () { return save("steam_launch_config=unavailable\r\n" + complete); });
    }

    function runCloudRestore(appId, preview) {
        var storage = Windows.Storage;
        var prefix = "steam_cloud_run_id=" + Date.now() + "\r\nsteam_cloud_app_id=" + appId + "\r\n";
        var report;
        function write(checkpoint) {
            if (!report) return Promise.resolve();
            return Promise.resolve(storage.FileIO.writeTextAsync(report,
                prefix + "steam_cloud_status=" + checkpoint + "\r\n")).then(null, function () {});
        }
        return Promise.resolve(storage.ApplicationData.current.localFolder.createFileAsync(
            "steam-cloud-report.txt", storage.CreationCollisionOption.replaceExisting
        )).then(function (file) {
            report = file;
            return write("checking");
        }, function () {}).then(function () {
            return XboxProbe.Native.ProbeRunner.restoreSteamCloud(appId, preview);
        }).then(function (result) {
            return write(result).then(function () { return result; });
        }, function () {
            return write("failed").then(function () { return "failed"; });
        });
    }

    function saveLibraryInventory() {
        var storage = Windows.Storage;
        return Windows.ApplicationModel.Package.current.installedLocation
            .tryGetItemAsync("steam-library-inventory.txt").then(function (marker) {
                if (!marker) return "";
                var games = entries.map(function (entry) { return { id: entry.id, name: entry.name }; });
                return storage.ApplicationData.current.localFolder.createFileAsync("steam-library-inventory.json",
                    storage.CreationCollisionOption.replaceExisting).then(function (file) {
                    return storage.FileIO.writeTextAsync(file, JSON.stringify(games) + "\r\n");
                }).then(function () { return "steam_library_inventory=saved\r\n"; });
            }).then(null, function () { return "steam_library_inventory=unavailable\r\n"; });
    }

    function finishNative(attempt, payload) {
        var reportRead = Windows.Storage.FileIO.readTextAsync(attempt.file);
        if (bigPictureMode && payload) {
            reportRead = Promise.resolve(reportRead).catch(function () { return attempt.prefix; });
        }
        return reportRead.then(function (report) {
            if (payload) {
                entries = JSON.parse(payload);
                entries.sort(function (a, b) { return a.name.localeCompare(b.name); });
                manifestFolder = Windows.Storage.ApplicationData.current.localFolder
                    .tryGetItemAsync("SteamLibrary").then(function (folder) {
                        return folder ? folder.tryGetItemAsync("steamapps") : null;
                    });
                signin.hidden = true;
                if (bigPictureMode) {
                    showBigPictureStatus("Opening Steam Big Picture…");
                } else {
                    libraryCount.textContent = entries.length + (entries.length === 1 ? " game" : " games");
                    search.value = "";
                    renderList();
                    library.hidden = false;
                    if ("gamepadInputEmulation" in navigator) navigator.gamepadInputEmulation = "keyboard";
                    (list.firstElementChild || search).focus();
                }
                attempt.done = true;
                attempt.client.close();
                if (report.indexOf("steam_session_cache=unavailable\r\n") >= 0) {
                    downloadStatus.textContent = "This Xbox could not save the Steam sign-in.";
                }
                if (bigPictureMode) {
                    attempt.libraryReport = report.replace("status=steam-library-rendering\r\n",
                        "steam_library_provider=big-picture\r\n");
                    function openLibrary() { return openBigPicture(attempt); }
                    return Windows.Storage.FileIO.writeTextAsync(attempt.file,
                        attempt.libraryReport + "status=steam-core-complete\r\n").then(openLibrary, openLibrary);
                }
                attempt.libraryReport = report.replace("status=steam-library-rendering\r\n", "steam_library_rendered=yes\r\n");
                return saveLibraryInventory().then(function (checkpoint) {
                    attempt.libraryReport += checkpoint;
                    return completeLibrary(attempt);
                });
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
        var firstLineEnd = checkpoint.indexOf("\r\n");
        var firstLine = firstLineEnd < 0 ? checkpoint : checkpoint.slice(0, firstLineEnd);
        var parts = /^(\w+(?:-\w+)*)(?::(-?\d+))?$/.exec(firstLine);
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
                if (firstLineEnd >= 0) report += checkpoint.slice(firstLineEnd + 2);
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
        if (job.write) {
            job.write.then(function () { job.complete(stage); }, function () { job.complete(stage); });
        } else {
            job.complete(stage);
        }
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

    function startDownload(entry) {
        if (transfer || launching || restoring) return Promise.resolve("busy");
        if (!entry || !entry.game) return Promise.resolve("game-unavailable");
        if (entry.installed) return Promise.resolve("already-installed");
        var job = { entry: entry, id: Date.now() };
        var completion = new Promise(function (resolve) { job.complete = resolve; });
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
        return completion;
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
        var button = event.target.entry ? event.target : event.target.parentElement;
        if (!button || !button.entry) return;
        select(button);
        var action = primaryAction();
        if (!action.hidden && !action.disabled) action.focus();
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
            if (!cloudPanel.hidden && (target === cloudRestore || target === cloudSaves) && !restoring) {
                closeCloudSaves();
                next = cloudSaves;
            } else {
                next = target.entry ? search : (selected || search);
            }
        } else if (!direction) {
            return;
        } else if (target === search) {
            if (direction === "up") next = signOut;
            else if (direction === "down") next = list.firstElementChild;
            else return; // Keep horizontal caret movement and native search input.
        } else if (target === signOut) {
            if (direction === "down" || direction === "left") next = search;
        } else if (target.entry) {
            next = libraryNavigation.adjacent(list.children, target, direction);
            if (!next && direction === "up") next = search;
            if (!next && direction === "right") next = primaryAction();
        } else if (target === play || target === download) {
            if (direction === "left") next = selected;
            if (direction === "up") next = signOut;
            if (direction === "down") next = cloudSaves;
        } else if (target === cloudSaves) {
            if (direction === "left") next = selected;
            if (direction === "up") next = primaryAction();
            if (direction === "down" && !cloudPanel.hidden) next = cloudRestore;
        } else if (target === cloudRestore) {
            if (direction === "left" || direction === "up") next = cloudSaves;
        } else {
            return;
        }
        event.preventDefault();
        if (next && !next.hidden && !next.disabled) next.focus();
    });
    download.addEventListener("click", function () {
        if (selected) startDownload(selected.entry);
    });
    play.addEventListener("click", function () {
        if (selected && selected.entry.installed) runGameLaunch(current, selected.entry);
    });
    cloudSaves.addEventListener("click", function () {
        if (cloudSaves.disabled) return;
        if (!cloudPanel.hidden) {
            closeCloudSaves();
            return;
        }
        cloudPanel.hidden = false;
        cloudSaves.setAttribute("aria-expanded", "true");
        cloudRestore.focus();
    });
    cloudRestore.addEventListener("click", function () {
        var entry = selected && selected.entry;
        if (!entry || cloudPanel.hidden || cloudRestore.disabled) return;
        restoring = true;
        signOut.disabled = true;
        cloudStatus.textContent = "Downloading Steam Cloud saves…";
        updateActions();
        function done(result) {
            restoring = false;
            signOut.disabled = false;
            updateActions();
            if (selected && selected.entry === entry && !cloudPanel.hidden) {
                cloudStatus.textContent = steamCloud.message(result);
                play.focus();
            }
        }
        runCloudRestore(entry.id, false).then(done, function () { done("failed"); });
    });
    function signOutCurrent() {
        if (signingOut || restoring) return Promise.resolve("busy");
        signingOut = true;
        if (transfer) updateDownload(transfer, "interrupted");
        launching = true;
        signOut.disabled = true;
        downloadStatus.textContent = "Signing out of Steam…";
        updateActions();
        function endSignOut(cleared) {
            signingOut = false;
            launching = false;
            signOut.disabled = false;
            entries = [];
            manifestFolder = null;
            renderList();
            if ("gamepadInputEmulation" in navigator) navigator.gamepadInputEmulation = originalInputMode;
            library.hidden = true;
            signin.hidden = false;
            if (bigPictureMode) showBigPictureStatus("");
            downloadStatus.textContent = "";
            status.textContent = cleared ? "Signed out of Steam." : "Could not confirm removal of the saved Steam session.";
            retry.hidden = false;
            retry.focus();
            return "signed-out";
        }
        return Promise.resolve().then(function () { return XboxProbe.Native.ProbeRunner.signOutSteam(); })
            .then(endSignOut, function () { return endSignOut(false); });
    }
    signOut.addEventListener("click", signOutCurrent);
    start();
})();
