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
        function library() {
            if (!active()) return Promise.reject(new Error("signed-out"));
            return Promise.resolve(session.installedGames()).then(function (installed) {
                if (!active()) throw new Error("signed-out");
                return session.games.map(function (game) {
                    var local = installed.has(game.id);
                    return { id: game.id, name: game.name, installed: local, canPlay: local };
                });
            });
        }
        function action(appId, install, handoff) {
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
                if (install) return installed ? "already-installed" : session.install(game);
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
        return {
            session: function () { return Promise.resolve().then(identity); },
            library: library,
            play: function (appId, handoff) { return action(appId, false, handoff); },
            install: function (appId) { return action(appId, true); }
        };
    }
    return { create: create };
}());
