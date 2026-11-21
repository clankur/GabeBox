// Created only inside the trusted package page after Steam returns an owned library.
var steamBigPictureBroker = (function () {
    "use strict";

    function create(session) {
        var acting = false;
        function library() {
            if (!session.current()) return Promise.reject(new Error("signed-out"));
            return Promise.resolve(session.installedGames()).then(function (installed) {
                if (!session.current()) throw new Error("signed-out");
                return session.games.map(function (game) {
                    var local = installed.has(game.id);
                    return { id: game.id, name: game.name, installed: local, canPlay: local };
                });
            });
        }
        function action(appId, install) {
            if (!session.current()) return Promise.resolve("signed-out");
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
                if (!session.current()) return "signed-out";
                if (session.busy()) return "busy";
                if (install) return installed ? "already-installed" : session.install(game);
                return installed ? session.play(game) : "not-installed";
            }).then(function (result) {
                acting = false;
                return result;
            }, function () {
                acting = false;
                return "failed";
            });
        }
        return {
            library: library,
            play: function (appId) { return action(appId, false); },
            install: function (appId) { return action(appId, true); }
        };
    }
    return { create: create };
}());
