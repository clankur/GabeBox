// Created only inside the trusted package page after Steam returns an owned library.
var steamBigPictureBroker = (function () {
    "use strict";

    function create(session) {
        var acting = false;
        function nativeId() {
            if (!session.current()) return null;
            var id = session.steamId();
            if (id === "") return null;
            if (typeof id !== "string" || !/^[1-9][0-9]{16,19}$/.test(id) ||
                (id.length === 20 && id > "18446744073709551615")) throw new Error("session-unavailable");
            return id;
        }
        // The owned games belong to the account active when this page acquired
        // its catalog. A later account must acquire its own catalog and broker.
        var owner;
        try { owner = nativeId(); } catch (_) { owner = null; }
        function identity() {
            var id = nativeId();
            return { steamId: owner && owner === id ? id : null };
        }
        function active() {
            try { return !!identity().steamId; } catch (_) { return false; }
        }
        function supportsCloudBackup(game) {
            return typeof session.cloudBackup === "function" && typeof session.supportsCloudBackup === "function" &&
                session.supportsCloudBackup(game.id);
        }
        function supportsCloudRestore(game) {
            return typeof session.cloudRestore === "function" && typeof session.supportsCloudRestore === "function" &&
                session.supportsCloudRestore(game.id);
        }
        function library() {
            if (!active()) return Promise.reject(new Error("signed-out"));
            return Promise.resolve(session.installedGames()).then(function (installed) {
                if (!active()) throw new Error("signed-out");
                return session.games.map(function (game) {
                    var local = installed.has(game.id);
                    var row = { id: game.id, name: game.name, installed: local, canPlay: local };
                    if (local && supportsCloudBackup(game)) {
                        row.cloudBackup = true;
                        if (supportsCloudRestore(game)) row.cloudRestore = true;
                    }
                    return row;
                });
            });
        }
        function cloudAction(appId, action) {
            if (!active()) return Promise.resolve("signed-out");
            if (typeof appId !== "number" || appId !== Math.floor(appId) || appId < 1 || appId > 0xffffff)
                return Promise.resolve("invalid-app");
            var game = session.games.find(function (entry) { return entry.id === appId; });
            if (!game) return Promise.resolve("not-owned");
            function supported() {
                return supportsCloudBackup(game) && (action !== "restore" || supportsCloudRestore(game));
            }
            if (!supported()) return Promise.resolve("unsupported");
            var writing = action !== "status";
            if (writing) {
                if (acting || session.busy()) return Promise.resolve("busy");
                acting = true;
            }
            return Promise.resolve().then(function () {
                if (!active()) return false;
                return session.isInstalled(game);
            }).then(function (installed) {
                if (!active()) return "signed-out";
                if (!installed) return "not-installed";
                if (writing && session.busy()) return "busy";
                if (!supported()) return "unsupported";
                return action === "restore" ? session.cloudRestore(appId) : session.cloudBackup(appId, writing);
            }).then(function (result) {
                if (writing) acting = false;
                return active() ? result : "signed-out";
            }, function () {
                if (writing) acting = false;
                return "failed";
            });
        }
        function action(appId, install, handoff, progress) {
            if (!active()) return Promise.resolve("signed-out");
            if (typeof appId !== "number" || appId !== Math.floor(appId) || appId < 1 || appId > 0xffffff) {
                return Promise.resolve("invalid-app");
            }
            if (acting || session.busy()) return Promise.resolve("busy");
            var game = session.games.filter(function (entry) { return entry.id === appId; })[0];
            if (!game) return Promise.resolve("not-owned");
            acting = true;
            return Promise.resolve().then(function () {
                return session.isInstalled(game);
            }).then(function (installed) {
                if (!active()) return "signed-out";
                if (session.busy()) return "busy";
                if (install) return installed ? "already-installed" : session.install(game, function (stage, counts) {
                    if (typeof progress === "function" && active()) progress(stage, counts);
                });
                if (!installed) return "not-installed";
                return Promise.resolve(handoff && handoff()).then(function () {
                    if (!active()) return "signed-out";
                    if (session.busy()) return "busy";
                    return session.play(game);
                });
            }).then(function (result) {
                acting = false;
                return result;
            }, function () {
                acting = false;
                return "failed";
            });
        }
        function storage(operation, argument) {
            if (!active()) return Promise.reject(new Error("signed-out"));
            if (typeof session.storage !== "function") return Promise.reject(new Error("storage-unavailable"));
            if (operation === "list") {
                return Promise.resolve(session.storage(operation, argument)).then(function (result) {
                    if (!active()) throw new Error("signed-out");
                    var owned = new Set(session.games.map(function (game) { return game.id; }));
                    result.folders.forEach(function (folder) {
                        folder.apps = folder.apps.filter(function (app) { return owned.has(app.appId); });
                    });
                    return result;
                });
            }
            if (operation !== "select" && operation !== "move" && operation !== "migrate")
                return Promise.reject(new Error("invalid-storage-operation"));
            if (acting || session.busy()) return Promise.resolve({ok: false, error: "Steam is busy."});
            acting = true;
            return Promise.resolve().then(function () {
                if (!active()) throw new Error("signed-out");
                return session.storage(operation, argument);
            }).then(function (result) {
                acting = false;
                return result;
            }, function (error) {
                acting = false;
                throw error;
            });
        }
        return {
            storage: storage,
            cloudBackup: function (appId, download) {
                if (typeof download !== "boolean") return Promise.resolve("invalid-app");
                return cloudAction(appId, download ? "download" : "status");
            },
            cloudRestore: function (appId) { return cloudAction(appId, "restore"); },
            session: function () { return Promise.resolve().then(identity); },
            library: library,
            signOut: function () {
                if (!active()) return Promise.resolve("session-changed");
                if (acting || session.busy()) return Promise.resolve("busy");
                acting = true;
                return Promise.resolve().then(function () {
                    if (!active()) return "session-changed";
                    return session.signOut();
                }).then(function (result) { acting = false; return result; },
                    function () { acting = false; return "failed"; });
            },
            play: function (appId, handoff) { return action(appId, false, handoff); },
            install: function (appId, progress) { return action(appId, true, null, progress); }
        };
    }
    return { create: create };
}());
