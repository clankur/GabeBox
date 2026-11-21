(function () {
    "use strict";
    var storage = Windows.Storage;
    var running = false;

    // Each profile describes one verified Steam depot. Never take launch commands or
    // mount paths from user input or execute the installed autoexec text directly.
    var xcom = {
        appId: 7760,
        title: "X-COM: UFO Defense",
        pointerControls: true,
        installDirectory: "XCom UFO Defense",
        configFile: "dosbox.conf",
        mountDirectory: "XCOM",
        freeSpaceMiB: 1024,
        batchFile: "UFOCD.BAT",
        guestConfigFile: "XCOMDOS.CONF",
        trialFilePrefix: "xcom-trial-",
        emptyRootDirectories: /^game_(?:[1-9]|10)$/i,
        excludedFiles: /\.dll$|^dosbox\.exe$|^ufo defense.*\.exe$/i,
        expectedAutoexec: ["@echo off", "mount c xcom -freesize 1024", "c:", "cd xcom", "call ufocd.bat", "exit"],
        settingsSections: ["cpu", "mixer", "sblaster", "dos", "dosbox"],
        // The pinned interpreter cannot initialize a 32 MiB guest and calls VGA-only mode "vgaonly".
        settingsOverrides: [
            {section: "dosbox", match: /^memsize\s*=\s*32$/i, replacement: "memsize=16"},
            {section: "dosbox", match: /^machine\s*=\s*vga$/i, replacement: "machine=vgaonly"}
        ]
    };

    function installedRoot(profile) {
        return storage.ApplicationData.current.localFolder.getFolderAsync("SteamLibrary").then(function (library) {
            return library.getFolderAsync("steamapps");
        }).then(function (apps) {
            return apps.getFolderAsync("common");
        }).then(function (common) {
            return common.getFolderAsync(profile.installDirectory);
        });
    }

    function copyGuestFiles(profile, folder, fs, requireOpen) {
        var mountPoint = "/" + profile.mountDirectory;
        var files = [];
        function walk(dir, prefix) {
            requireOpen();
            return dir.getItemsAsync().then(function (items) {
                requireOpen();
                var subfolders = Promise.resolve();
                for (var i = 0; i < items.size; ++i) {
                    var item = items.getAt(i);
                    var path = prefix + "/" + item.name;
                    if (item.isOfType(storage.StorageItemTypes.folder)) {
                        fs.fs.mkdir(path);
                        if (prefix === mountPoint && profile.emptyRootDirectories.test(item.name)) continue;
                        (function (child, childPath) {
                            subfolders = subfolders.then(function () { return walk(child, childPath); });
                        }(item, path));
                    } else if (item.isOfType(storage.StorageItemTypes.file)) {
                        if (!profile.excludedFiles.test(item.name)) {
                            files.push({path: path, item: item});
                        }
                    }
                }
                return subfolders;
            });
        }
        var index = 0;
        function copyNext() {
            requireOpen();
            if (index === files.length) return Promise.resolve();
            var source = files[index++];
            return storage.FileIO.readBufferAsync(source.item).then(function (buffer) {
                requireOpen();
                var bytes = new Uint8Array(buffer.length);
                if (bytes.length) {
                    var reader = storage.Streams.DataReader.fromBuffer(buffer);
                    try { reader.readBytes(bytes); } finally { reader.close(); }
                }
                fs.createFile(source.path, bytes);
            }).then(copyNext);
        }
        // Preserve the publisher's mount source, including empty game/save directories.
        fs.fs.mkdir(mountPoint);
        return walk(folder, mountPoint).then(function () {
            return Promise.all([copyNext(), copyNext(), copyNext(), copyNext()]);
        }).then(function () { return files.length; });
    }

    function trialFrames(profile, canvas, runId, requireOpen) {
        var start = Date.now();
        var lines = [];
        function capture(seconds) {
            requireOpen();
            return new Promise(function (resolve) {
                setTimeout(resolve, Math.max(0, start + seconds * 1000 - Date.now()));
            }).then(function () {
                var name = profile.trialFilePrefix + runId + "-" + seconds + "s.png";
                requireOpen();
                if (!document.documentElement.contains(canvas)) throw new Error("closed");
                var encoded = canvas.toDataURL("image/png");
                if (encoded.indexOf("data:image/png;base64,") !== 0) throw new Error("capture");
                var png = Windows.Security.Cryptography.CryptographicBuffer.decodeFromBase64String(encoded.split(",")[1]);
                return storage.ApplicationData.current.localFolder.createFileAsync(name,
                    storage.CreationCollisionOption.failIfExists).then(function (file) {
                    requireOpen();
                    return storage.FileIO.writeBufferAsync(file, png);
                }).then(function () { lines.push("steam_dos_frame_" + seconds + "s=" + name); });
            }).catch(function () { lines.push("steam_dos_frame_" + seconds + "s=unavailable"); });
        }
        return capture(8).then(function () { return capture(18); }).then(function () {
            return lines.join("\r\n") + "\r\n";
        });
    }

    function start(profile, checkpoint, automaticTrial) {
        if (running) return Promise.resolve("steam_dos=already-running\r\n");
        running = true;
        var screen = document.createElement("section");
        screen.id = "xcom-game";
        var frame = document.createElement("div");
        frame.className = "dos-screen";
        var canvas = document.createElement("canvas");
        canvas.tabIndex = 0;
        canvas.setAttribute("aria-label", profile.title);
        frame.appendChild(canvas);
        screen.appendChild(frame);
        var back = document.createElement("button");
        back.textContent = "Return to library";
        back.title = "This trial keeps saves in memory until you leave the game.";
        screen.appendChild(back);
        var library = document.querySelector("main");
        var previousInputMode;
        var game;
        var closed = false;
        var complete;
        var termination = new Promise(function (resolve) { complete = resolve; });
        var pendingReport = Promise.resolve();
        function requireOpen() {
            if (closed) throw new Error("closed");
        }
        function record(phase) {
            requireOpen();
            pendingReport = Promise.resolve().then(function () {
                requireOpen();
                return checkpoint(phase);
            });
            return pendingReport;
        }
        function close(stop, result) {
            if (closed) return;
            closed = true;
            if (screen.parentNode) screen.parentNode.removeChild(screen);
            if (previousInputMode !== undefined) navigator.gamepadInputEmulation = previousInputMode;
            library.hidden = false;
            // Let an already-submitted report write finish before another Play can start.
            function release() {
                running = false;
                complete(result);
            }
            pendingReport.then(release, release);
            if (stop && game) { try { game.exit(); } catch (ignore) {} }
            var play = document.getElementById("play");
            if (play) play.focus();
        }
        back.addEventListener("click", function () { close(true, "steam_dos=returned-to-library\r\n"); });
        var root;
        var config;
        var files;
        var phase = "reading-config";
        var launch = record(phase).then(function () {
            return installedRoot(profile);
        }).then(function (folder) {
            root = folder;
            return folder.getFileAsync(profile.configFile);
        }).then(function (file) {
            return storage.FileIO.readTextAsync(file);
        }).then(function (text) {
            // Only an independently verified publisher configuration is supported.
            config = verifiedConfig(profile, text);
            if (!window.exports || typeof window.exports.WDOSBOX !== "function") {
                throw new Error("packaged-runtime-unavailable");
            }
            document.body.appendChild(screen);
            library.hidden = true;
            if (profile.pointerControls && "gamepadInputEmulation" in navigator) {
                previousInputMode = navigator.gamepadInputEmulation;
                navigator.gamepadInputEmulation = "mouse";
            }
            phase = "starting-emulator";
            return record(phase);
        }).then(function () {
            if (closed) throw new Error("closed");
            return Dos(canvas, {
                wdosboxUrl: "xcom-runtime/dosbox-emterp.js",
                onAbort: function () { close(false, "steam_dos=runtime-aborted\r\n"); },
                onExit: function () { close(false, "steam_dos=guest-exited\r\n"); }
            });
        }).then(function (runtime) {
            phase = "copying-installed-files";
            return record(phase).then(function () {
                requireOpen();
                return root.getFolderAsync(profile.mountDirectory);
            }).then(function (guest) {
                requireOpen();
                return copyGuestFiles(profile, guest, runtime.fs, requireOpen);
            }).then(function (count) {
                requireOpen();
                files = count;
                runtime.fs.createFile("/" + profile.guestConfigFile, config);
                phase = "starting-guest";
                return record(phase);
            }).then(function () {
                if (closed) throw new Error("closed");
                // js-dos pre-mounts MEMFS at C:. Mount the publisher's data directory in its place.
                return runtime.main([
                    "-conf", profile.guestConfigFile, "-c", "z:", "-c", "mount -u c",
                    "-c", "mount c /" + profile.mountDirectory + " -freesize " + profile.freeSpaceMiB,
                    "-c", "c:", "-c", "call " + profile.batchFile, "-c", "exit"
                ]);
            });
        }).then(function (ci) {
            game = ci;
            if (closed) {
                try { game.exit(); } catch (ignore) {}
                return termination;
            }
            canvas.focus();
            var result = "steam_dos_files=" + files + "\r\nsteam_dos=guest-started\r\n";
            if (!automaticTrial) return result;
            return record("waiting-for-trial-frames").then(function () {
                return trialFrames(profile, canvas, Date.now(), requireOpen);
            }).then(function (frames) { return result + frames; });
        }).catch(function () {
            var result = "steam_dos=failed\r\nsteam_dos_phase=" + phase + "\r\n";
            close(true, result);
            return termination;
        });
        return Promise.race([launch, termination]);
    }

    function verifiedConfig(profile, text) {
        var lines = text.split(/\r\n?|\n/);
        var section = "";
        var commands = [];
        var settings = [];
        lines.forEach(function (line) {
            var value = line.trim();
            var header = /^\[([^\]]+)\]$/.exec(value);
            if (header) section = header[1].toLowerCase();
            if (section === "autoexec") {
                if (!header && value && value.charAt(0) !== "#") {
                    commands.push(value.toLowerCase().replace(/\s+/g, " "));
                }
            } else if (profile.settingsSections.indexOf(section) !== -1) {
                profile.settingsOverrides.forEach(function (option) {
                    if (section === option.section && option.match.test(value)) line = option.replacement;
                });
                settings.push(line);
            }
        });
        if (commands.join("\n") !== profile.expectedAutoexec.join("\n")) {
            throw new Error("Unexpected publisher autoexec.");
        }
        return settings.join("\r\n") + "\r\n";
    }

    // Bind the public adapter to a trusted profile so its caller can supply only trial controls.
    window.xcomDosRuntime = {
        appId: xcom.appId,
        start: function (checkpoint, automaticTrial) { return start(xcom, checkpoint, automaticTrial); }
    };
}());
