(function () {
    "use strict";

    var output = document.getElementById("result");
    function errorText(error) {
        var number = typeof error.number === "number" ? "0x" + (error.number >>> 0).toString(16) : "unavailable";
        return "hresult=" + number + "\r\nmessage=" + error.message + "\r\n";
    }

    function bounded(operation) {
        var timer = setTimeout(function () { operation.cancel(); }, 7000);
        return operation.then(function (value) {
            clearTimeout(timer);
            return value;
        }, function (error) {
            clearTimeout(timer);
            throw error;
        });
    }

    function connectSteamSocket(endpoint, report) {
        var socket = new Windows.Networking.Sockets.MessageWebSocket();
        return bounded(socket.connectAsync(new Windows.Foundation.Uri("wss://" + endpoint + "/cmsocket/")))
            .then(function () {
                socket.close();
                return report + "steam_cm_websocket=connected\r\n";
            }, function (error) {
                socket.close();
                return report + "steam_cm_websocket=failed\r\n" + errorText(error);
            });
    }

    function checkSteamApproval(client) {
        var http = Windows.Web.Http;
        var name = "Xbox Dev Mode Steam client";
        // Authentication.BeginAuthSessionViaQR device_details, with SteamClient platform type.
        var device = String.fromCharCode(10, name.length) + name + String.fromCharCode(16, 1);
        var request = btoa(String.fromCharCode(26, device.length) + device);
        var form = new http.HttpStringContent(
            "input_protobuf_encoded=" + encodeURIComponent(request),
            Windows.Storage.Streams.UnicodeEncoding.utf8, "application/x-www-form-urlencoded"
        );
        var url = new Windows.Foundation.Uri(
            "https://api.steampowered.com/IAuthenticationService/BeginAuthSessionViaQR/v1/?format=json"
        );
        return bounded(client.postAsync(url, form)).then(function (response) {
            var result = "steam_qr_http=" + Number(response.statusCode) + "\r\n";
            if (!response.isSuccessStatusCode) return result + "steam_qr_challenge=unavailable\r\n";
            return bounded(response.content.readAsStringAsync()).then(function (body) {
                var challenge = JSON.parse(body).response;
                var valid = challenge && /^https:\/\/s\.team\/q\/[a-zA-Z0-9/_-]+$/.test(challenge.challenge_url) &&
                    challenge.client_id && challenge.request_id;
                // Discard the unapproved request. Challenge URLs and identifiers stay out of diagnostics.
                return result + "steam_qr_challenge=" + (valid ? "issued" : "invalid") + "\r\n";
            });
        }).then(null, function () {
            return "steam_qr_challenge=failed\r\n";
        });
    }

    function checkSteamNetwork() {
        var http = Windows.Web.Http;
        var filter = new http.Filters.HttpBaseProtocolFilter();
        filter.allowAutoRedirect = false;
        var client = new http.HttpClient(filter);
        var directory = new Windows.Foundation.Uri(
            "https://api.steampowered.com/ISteamDirectory/GetCMListForConnect/v1/?cellid=0&cmtype=websockets"
        );
        return bounded(client.getAsync(directory)).then(function (response) {
            var report = "steam_cm_directory_http=" + Number(response.statusCode) + "\r\n";
            if (!response.isSuccessStatusCode) return report;

            return bounded(response.content.readAsStringAsync()).then(function (body) {
                var list = JSON.parse(body).response.serverlist;
                var endpoints = list.filter(function (server) {
                    return server.type === "websockets" &&
                        /^[a-z0-9.-]+\.steamserver\.net:443$/i.test(server.endpoint);
                });
                report += "steam_cm_candidates=" + endpoints.length + "\r\n";
                return endpoints.length ? connectSteamSocket(endpoints[0].endpoint, report) : report;
            });
        }).then(function (report) {
            return checkSteamApproval(client).then(function (approval) {
                return report + approval;
            });
        }).then(function (report) {
            client.close();
            return report;
        }, function (error) {
            client.close();
            return "steam_cm_directory=failed\r\n" + errorText(error);
        });
    }

    function writeReport(storage, name, report) {
        output.textContent = report;
        return storage.ApplicationData.current.localFolder.createFileAsync(
            name, storage.CreationCollisionOption.openIfExists
        ).then(function (file) {
            return storage.FileIO.writeTextAsync(file, report);
        });
    }

    function reportError(error) {
        output.textContent = "Unable to write the LocalState report.\r\n" + errorText(error);
    }

    function controlProbe(storage) {
        var wasVisible = false;
        var foregroundRun = "";
        var folder = storage.ApplicationData.current.localFolder;

        function watchReturnRequest(run, deadline) {
            if (document.hidden || run !== foregroundRun || Date.now() >= deadline) return;
            function retry() {
                setTimeout(function () { watchReturnRequest(run, deadline); }, 500);
            }

            folder.tryGetItemAsync("lifetime-return.txt").then(function (file) {
                if (!file) return null;
                return storage.FileIO.readTextAsync(file);
            }).done(function (request) {
                if (request === null || request.trim() !== run) {
                    retry();
                    return;
                }
                if (document.hidden || run !== foregroundRun) return;

                var header = "run_id=" + run + "\r\n";
                writeReport(storage, "protocol-return.txt", header + "protocol_return=requested\r\n").then(function () {
                    var uri = new Windows.Foundation.Uri("xbox-portability-probe-return:");
                    return Windows.System.Launcher.launchUriAsync(uri);
                }).done(function (launched) {
                    if (!launched) {
                        writeReport(storage, "protocol-return.txt", header + "protocol_return=declined\r\n"
                        ).done(null, reportError);
                    }
                }, function (error) {
                    writeReport(storage, "protocol-return.txt", header + "protocol_return=failed\r\n" +
                        errorText(error)).done(null, reportError);
                });
            }, retry);
        }

        function publishForeground() {
            var visible = !document.hidden;
            if (visible && !wasVisible) {
                foregroundRun = String(Date.now());
                writeReport(storage, "probe-report.txt", "probe=packaged-startup-control\r\n" +
                    "run_id=" + foregroundRun + "\r\ncontrol_visible=yes\r\nstatus=control-ready\r\n"
                ).done(function () {
                    watchReturnRequest(foregroundRun, Date.now() + 120000);
                }, reportError);
            }
            wasVisible = visible;
        }
        document.addEventListener("visibilitychange", publishForeground);
        publishForeground();
    }

    function lifetimeProbe(storage) {
        var header = "probe=packaged-helper-lifetime\r\nrun_id=" + Date.now() + "\r\n";
        var app = Windows.UI.WebUI.WebUIApplication;
        var hidden = false;
        var background = false;
        var suspended = false;
        var resumed = false;
        var ready = false;
        var finished = false;

        function invoke(method) {
            try {
                return method(XboxProbe.Native.ProbeRunner);
            } catch (error) {
                return "lifetime=failed\r\nlifetime_stage=native-component\r\n" + errorText(error);
            }
        }

        function flag(value) {
            return value ? "yes" : "no";
        }

        function finishIfVisible() {
            if (!ready || finished || document.hidden || (!hidden && !background)) return;
            finished = true;
            var result = invoke(function (runner) { return runner.finishLifetime(); });
            var events = "observed_hidden=" + flag(hidden) + "\r\nobserved_background=" + flag(background) +
                "\r\nobserved_suspending=" + flag(suspended) + "\r\nobserved_resuming=" + flag(resumed) + "\r\n";
            writeReport(storage, "lifetime-report.txt", header + events + result).done(null, reportError);
        }

        document.addEventListener("visibilitychange", function () {
            if (document.hidden) hidden = true;
            else finishIfVisible();
        });
        app.addEventListener("enteredbackground", function () { background = true; });
        app.addEventListener("leavingbackground", function () { setTimeout(finishIfVisible, 0); });
        app.addEventListener("suspending", function () { suspended = true; });
        app.addEventListener("resuming", function () { resumed = true; });

        writeReport(storage, "lifetime-report.txt", header + "lifetime=web-started\r\n").then(function () {
            var result = invoke(function (runner) { return runner.startLifetime(); });
            return writeReport(storage, "lifetime-report.txt", header + result).then(function () {
                ready = result.indexOf("lifetime=ready\r\n") === 0;
            });
        }).done(null, reportError);
    }

    try {
        var storage = Windows.Storage;
        var packageName = Windows.ApplicationModel.Package.current.id.name;
        if (packageName === "XboxPortabilityProbe.ChildProcess.Control") {
            controlProbe(storage);
            return;
        }
        if (packageName === "XboxPortabilityProbe.ChildProcess.Lifetime") {
            lifetimeProbe(storage);
            return;
        }
        var steamCore = packageName === "XboxPortabilityProbe.ChildProcess.SteamCore";
        var probeName = steamCore ? "packaged-steam-core" : "packaged-child-process";
        var header = "probe=" + probeName +
            "\r\nrun_id=" + Date.now() + "\r\n";
        storage.ApplicationData.current.localFolder.createFileAsync(
            "probe-report.txt", storage.CreationCollisionOption.replaceExisting
        ).then(function (file) {
            return storage.FileIO.writeTextAsync(file, header + "status=web-started\r\n").then(function () {
                function finish(inventory) {
                    var result;
                    try {
                        result = steamCore ? XboxProbe.Native.ProbeRunner.runSteamCore(inventory) :
                            XboxProbe.Native.ProbeRunner.run();
                    } catch (error) {
                        result = "status=native-component-error\r\n" + errorText(error);
                    }
                    output.textContent = result;
                    if (steamCore && result.indexOf("create_interface=present\r\n") !== -1) {
                        var prefix = header + result.replace("status=steam-core-complete\r\n", "");
                        return checkSteamNetwork().then(function (network) {
                            return XboxProbe.Native.ProbeRunner.runSteamRuntime(file, prefix + network);
                        }).then(function () {
                            return storage.FileIO.readTextAsync(file).then(function (report) {
                                output.textContent = report;
                            });
                        });
                    }
                    return storage.FileIO.writeTextAsync(file, header + result);
                }

                if (!steamCore) return finish("");
                return Windows.ApplicationModel.Package.current.installedLocation
                    .tryGetItemAsync("steam-core-imports.txt").then(function (inventory) {
                        return inventory ? storage.FileIO.readTextAsync(inventory) : "";
                    }).then(finish);
            });
        }).done(null, reportError);
    } catch (error) {
        output.textContent = "Unable to start the diagnostic.\r\n" + errorText(error);
    }
})();
