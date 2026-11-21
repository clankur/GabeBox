(function () {
    "use strict";
    var storage = Windows.Storage;
    var running = false;

    function installedRoot() {
        return storage.ApplicationData.current.localFolder.getFolderAsync("SteamLibrary").then(function (library) {
            return library.getFolderAsync("steamapps");
        }).then(function (apps) {
            return apps.getFolderAsync("common");
        }).then(function (common) {
            return common.getFolderAsync("XCom UFO Defense");
        });
    }

    function copyGuestFiles(folder, fs) {
        var files = [];
        function walk(dir, prefix) {
            return dir.getItemsAsync().then(function (items) {
                var subfolders = Promise.resolve();
                for (var i = 0; i < items.size; ++i) {
                    var item = items.getAt(i);
                    var path = prefix + "/" + item.name;
                    if (item.isOfType(storage.StorageItemTypes.folder)) {
                        fs.fs.mkdir(path);
                        if (prefix === "/XCOM" && /^game_(?:[1-9]|10)$/i.test(item.name)) continue;
                        (function (child, childPath) {
                            subfolders = subfolders.then(function () { return walk(child, childPath); });
                        }(item, path));
                    } else if (item.isOfType(storage.StorageItemTypes.file)) {
                        if (!/\.dll$|^dosbox\.exe$|^ufo defense.*\.exe$/i.test(item.name)) {
                            files.push({path: path, item: item});
                        }
                    }
                }
                return subfolders;
            });
        }
        var index = 0;
        function copyNext() {
            if (index === files.length) return Promise.resolve();
            var source = files[index++];
            return storage.FileIO.readBufferAsync(source.item).then(function (buffer) {
                var bytes = new Uint8Array(buffer.length);
                if (bytes.length) {
                    var reader = storage.Streams.DataReader.fromBuffer(buffer);
                    try { reader.readBytes(bytes); } finally { reader.close(); }
                }
                fs.createFile(source.path, bytes);
            }).then(copyNext);
        }
        // Preserve the publisher's mount source, including empty game/save directories.
        fs.fs.mkdir("/XCOM");
        return walk(folder, "/XCOM").then(function () {
            return Promise.all([copyNext(), copyNext(), copyNext(), copyNext()]);
        }).then(function () { return files.length; });
    }

    function trialFrames(canvas, runId) {
        var start = Date.now();
        var lines = [];
        function capture(seconds) {
            return new Promise(function (resolve) {
                setTimeout(resolve, Math.max(0, start + seconds * 1000 - Date.now()));
            }).then(function () {
                var name = "xcom-trial-" + runId + "-" + seconds + "s.png";
                if (!document.documentElement.contains(canvas) || !running) throw new Error("closed");
                var encoded = canvas.toDataURL("image/png");
                if (encoded.indexOf("data:image/png;base64,") !== 0) throw new Error("capture");
                var png = Windows.Security.Cryptography.CryptographicBuffer.decodeFromBase64String(encoded.split(",")[1]);
                return storage.ApplicationData.current.localFolder.createFileAsync(name,
                    storage.CreationCollisionOption.failIfExists).then(function (file) {
                    return storage.FileIO.writeBufferAsync(file, png);
                }).then(function () { lines.push("steam_dos_frame_" + seconds + "s=" + name); });
            }).catch(function () { lines.push("steam_dos_frame_" + seconds + "s=unavailable"); });
        }
        return capture(8).then(function () { return capture(18); }).then(function () {
            return lines.join("\r\n") + "\r\n";
        });
    }

    function start(checkpoint, automaticTrial) {
        if (running) return Promise.resolve("steam_dos=already-running\r\n");
        running = true;
        var screen = document.createElement("section");
        screen.id = "xcom-game";
        var frame = document.createElement("div");
        frame.className = "dos-screen";
        var canvas = document.createElement("canvas");
        canvas.tabIndex = 0;
        canvas.setAttribute("aria-label", "X-COM: UFO Defense");
        frame.appendChild(canvas);
        screen.appendChild(frame);
        var back = document.createElement("button");
        back.textContent = "Return to library";
        back.title = "This trial keeps saves in memory until you leave the game.";
        screen.appendChild(back);
        var library = document.querySelector("main");
        var game;
        var closed = false;
        var complete;
        var termination = new Promise(function (resolve) { complete = resolve; });
        function close(stop, result) {
            if (closed) return;
            closed = true;
            if (screen.parentNode) screen.parentNode.removeChild(screen);
            library.hidden = false;
            running = false;
            complete(result);
            if (stop && game) { try { game.exit(); } catch (ignore) {} }
            var play = document.getElementById("play");
            if (play) play.focus();
        }
        back.addEventListener("click", function () { close(true, "steam_dos=returned-to-library\r\n"); });
        var root;
        var config;
        var files;
        var phase = "reading-config";
        var launch = Promise.resolve(checkpoint(phase)).then(installedRoot).then(function (folder) {
            root = folder;
            return folder.getFileAsync("dosbox.conf");
        }).then(function (file) {
            return storage.FileIO.readTextAsync(file);
        }).then(function (text) {
            // Only an independently verified publisher configuration is supported.
            config = verifiedConfig(text);
            if (!window.exports || typeof window.exports.WDOSBOX !== "function") {
                throw new Error("packaged-runtime-unavailable");
            }
            document.body.appendChild(screen);
            library.hidden = true;
            phase = "starting-emulator";
            return checkpoint(phase);
        }).then(function () {
            if (closed) throw new Error("closed");
            return Dos(canvas, {
                wdosboxUrl: "xcom-runtime/dosbox-emterp.js",
                onAbort: function () { close(false, "steam_dos=runtime-aborted\r\n"); },
                onExit: function () { close(false, "steam_dos=guest-exited\r\n"); }
            });
        }).then(function (runtime) {
            phase = "copying-installed-files";
            return checkpoint(phase).then(function () {
                return root.getFolderAsync("XCOM");
            }).then(function (guest) {
                return copyGuestFiles(guest, runtime.fs);
            }).then(function (count) {
                files = count;
                runtime.fs.createFile("/XCOMDOS.CONF", config.settings);
                phase = "starting-guest";
                return checkpoint(phase);
            }).then(function () {
                if (closed) throw new Error("closed");
                return runtime.main(["-conf", "XCOMDOS.CONF"].concat(config.args));
            });
        }).then(function (ci) {
            game = ci;
            if (closed) {
                try { game.exit(); } catch (ignore) {}
                return "steam_dos=returned-to-library\r\n";
            }
            canvas.focus();
            var result = "steam_dos_files=" + files + "\r\nsteam_dos=guest-started\r\n";
            if (!automaticTrial) return result;
            return checkpoint("waiting-for-trial-frames").then(function () {
                return trialFrames(canvas, Date.now());
            }).then(function (frames) { return result + frames; });
        }).catch(function () {
            var result = "steam_dos=failed\r\nsteam_dos_phase=" + phase + "\r\n";
            close(true, result);
            return result;
        });
        return Promise.race([launch, termination]);
    }

    function verifiedConfig(text) {
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
            } else if (["cpu", "mixer", "sblaster", "dos", "dosbox"].indexOf(section) !== -1) {
                // The 6.22.60 asm.js heap cannot initialize a 32 MiB guest. Its VGA-only
                // renderer uses the newer DOSBox spelling for the vendor's legacy VGA mode.
                if (section === "dosbox" && /^memsize\s*=\s*32$/i.test(value)) line = "memsize=16";
                if (section === "dosbox" && /^machine\s*=\s*vga$/i.test(value)) line = "machine=vgaonly";
                settings.push(line);
            }
        });
        var published = ["@echo off", "mount c xcom -freesize 1024", "c:", "cd xcom", "call ufocd.bat", "exit"];
        if (commands.join("\n") !== published.join("\n")) throw new Error("Unexpected publisher autoexec.");
        // Replace js-dos's initial MEMFS root mount with the publisher's XCOM mount.
        // The install has UFOCD.BAT in its root and no nested XCOM directory.
        return {settings: settings.join("\r\n") + "\r\n", args: [
            "-c", "z:", "-c", "mount -u c", "-c", "mount c /XCOM -freesize 1024",
            "-c", "c:", "-c", "call UFOCD.BAT", "-c", "exit"
        ]};
    }

    window.xcomDosRuntime = {start: start};
}());
