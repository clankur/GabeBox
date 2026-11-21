var steamGameLaunch = (function () {
    "use strict";

    function forGame(runner, appId, hostedGames) {
        var hosted = hostedGames.filter(function (game) { return game.appId === appId; })[0];
        if (!hosted) {
            return {
                backend: "steam",
                start: function (attempt) {
                    return runner.launchSteamGame(appId, attempt.file, attempt.report);
                }
            };
        }
        return {
            backend: hosted.backend,
            start: function (attempt) {
                return Promise.resolve(hosted.authorize(runner, appId, attempt)).then(function (authorization) {
                    if (authorization.split(/\r?\n/).indexOf(hosted.authorizedLine) === -1) return authorization;
                    return Promise.resolve(hosted.start(function (phase) {
                        return attempt.checkpoint(authorization + hosted.phaseField + "=" + phase + "\r\n");
                    }, attempt.automaticTrial)).then(function (result) { return authorization + result; });
                });
            }
        };
    }

    function parseTrial(text) {
        var config = /^(\d{1,8})(?:\r?\n(repeat))?(?:\r?\n(install))?$/.exec(text.trim());
        if (!config || +config[1] < 1 || +config[1] > 16777215) return null;
        return { appId: +config[1], repeat: !!config[2], install: !!config[3] };
    }

    return { forGame: forGame, parseTrial: parseTrial };
}());
