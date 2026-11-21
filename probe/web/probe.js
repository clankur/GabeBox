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

    try {
        var storage = Windows.Storage;
        var packageName = Windows.ApplicationModel.Package.current.id.name;
        var control = packageName === "XboxPortabilityProbe.ChildProcess.Control";
        var steamCore = packageName === "XboxPortabilityProbe.ChildProcess.SteamCore";
        var probeName = steamCore ? "packaged-steam-core" : "packaged-child-process";
        var header = "probe=" + (control ? "packaged-startup-control" : probeName) +
            "\r\nrun_id=" + Date.now() + "\r\n";
        storage.ApplicationData.current.localFolder.createFileAsync(
            "probe-report.txt", storage.CreationCollisionOption.replaceExisting
        ).then(function (file) {
            return storage.FileIO.writeTextAsync(file, header + "status=web-started\r\n").then(function () {
                function finish(inventory) {
                    var result;
                    try {
                        result = steamCore ? XboxProbe.Native.ProbeRunner.runSteamCore(inventory) :
                            (control ? "status=control-ready\r\n" : XboxProbe.Native.ProbeRunner.run());
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
        }).done(null, function (error) {
            output.textContent = "Unable to write the LocalState report.\r\n" + errorText(error);
        });
    } catch (error) {
        output.textContent = "Unable to start the diagnostic.\r\n" + errorText(error);
    }
})();
