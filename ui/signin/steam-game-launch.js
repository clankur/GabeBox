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
        if (hosted.launch) {
            return {
                backend: hosted.backend,
                start: function (attempt) { return hosted.launch(runner, appId, attempt); }
            };
        }
        return {
            backend: hosted.backend,
            start: function (attempt) {
                return Promise.resolve(hosted.authorize(runner, appId, attempt)).then(function (authorization) {
                    if (authorization.split(/\r?\n/).indexOf(hosted.authorizedLine) === -1) return authorization;
                    return Promise.resolve(hosted.start(function (phase) {
                        return attempt.checkpoint(authorization + hosted.phaseField + "=" + phase + "\r\n");
                    }, attempt.automaticTrial, attempt.onExit)).then(function (result) { return authorization + result; });
                });
            }
        };
    }

    function parseTrial(text) {
        var config = /^(\d{1,8})(?:\r?\n(repeat))?(?:\r?\n(install))?$/.exec(text.trim());
        if (!config || +config[1] < 1 || +config[1] > 16777215) return null;
        return { appId: +config[1], repeat: !!config[2], install: !!config[3] };
    }

    function parsePrerequisiteRestart(args) {
        var request = /^steam-prerequisite-play ([1-9][0-9]{16,19}) ([1-9][0-9]{0,7}) ([1-9][0-9]{0,15})$/.exec(args);
        if (!request || +request[2] > 0xffffff || !Number.isSafeInteger(+request[3]) ||
                (request[1].length === 20 && request[1] > "18446744073709551615")) return null;
        return { account: request[1], appId: +request[2], runId: request[3] };
    }

    function prerequisiteRestartArgs(result, account, appId, runId) {
        if (!/^steam_launch=prerequisite-restart-required\r?$/m.test(result) ||
                !/^steam_launch_redistributables_manifest=complete\r?$/m.test(result)) return "";
        var args = "steam-prerequisite-play " + account + " " + appId + " " + runId;
        return parsePrerequisiteRestart(args) ? args : "";
    }

    return { forGame: forGame, parseTrial: parseTrial, parsePrerequisiteRestart: parsePrerequisiteRestart,
        prerequisiteRestartArgs: prerequisiteRestartArgs };
}());
