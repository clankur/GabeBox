(function () {
    "use strict";

    var status = document.getElementById("status");
    var canvas = document.getElementById("code");
    var retry = document.getElementById("retry");
    var cancel = document.getElementById("cancel");
    var current;

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
        var qr = qrcode(0, "M");
        qr.addData(url);
        qr.make();
        var size = qr.getModuleCount();
        var scale = 12;
        canvas.width = canvas.height = (size + 8) * scale;
        var context = canvas.getContext("2d");
        context.fillStyle = "#fff";
        context.fillRect(0, 0, canvas.width, canvas.height);
        context.fillStyle = "#000";
        for (var y = 0; y < size; y++) {
            for (var x = 0; x < size; x++) {
                if (qr.isDark(y, x)) context.fillRect((x + 4) * scale, (y + 4) * scale, scale, scale);
            }
        }
        canvas.hidden = false;
    }

    function clearCode() {
        canvas.hidden = true;
        canvas.width = canvas.height = 0;
    }

    function accountId(refresh) {
        var part = refresh.split(".")[1];
        if (!part) throw new Error("Valve did not return an account identity");
        part = part.replace(/-/g, "+").replace(/_/g, "/");
        while (part.length % 4) part += "=";
        var id = JSON.parse(atob(part)).sub;
        if (typeof id !== "string" || !/^[0-9]{17,20}$/.test(id)) throw new Error("Invalid Valve account identity");
        return id;
    }

    function finish(attempt, display, marker) {
        if (current !== attempt || attempt.done) return;
        attempt.done = true;
        clearTimeout(attempt.timer);
        attempt.client.close();
        clearCode();
        cancel.hidden = true;
        retry.hidden = false;
        retry.focus();
        status.textContent = display;
        if (marker && attempt.file) {
            Windows.Storage.FileIO.writeTextAsync(attempt.file, attempt.prefix + marker +
                "\r\nstatus=steam-core-complete\r\n").done(null, function () {});
        }
    }

    function finishNative(attempt) {
        return Windows.Storage.FileIO.readTextAsync(attempt.file).then(function (report) {
            var count = /(?:^|\n)steam_library_count=(\d+)\r?\n/.exec(report);
            if (report.indexOf("steam_library=verified\r\n") >= 0 && count) {
                finish(attempt, "Steam approved sign-in. Library access verified for " + count[1] + " entries.");
            } else if (report.indexOf("steam_signin=authenticated\r\n") >= 0) {
                finish(attempt, "Steam approved sign-in. The library did not finish loading.");
            } else {
                finish(attempt, "Steam Guard approved the request. The native client did not complete sign-in.");
            }
        });
    }

    function submitApproval(attempt, reply) {
        var id = accountId(reply.refresh_token);
        clearCode();
        cancel.hidden = true;
        status.textContent = "Connecting to Steam and reading your library…";
        var approved = attempt.prefix + "steam_approval=received\r\n";
        var operation = XboxProbe.Native.ProbeRunner.runSteamSignIn(
            attempt.file, approved, reply.account_name, reply.refresh_token, id
        );
        return operation.then(function () { return finishNative(attempt); });
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

    function start() {
        if (current && !current.done) return;
        retry.hidden = true;
        status.textContent = "Preparing Steam Guard sign-in…";
        var attempt = {
            client: httpClient(),
            deadline: Date.now() + 150000,
            prefix: "probe=steam-signin\r\nrun_id=" + Date.now() + "\r\n"
        };
        current = attempt;
        Windows.Storage.ApplicationData.current.localFolder.createFileAsync(
            "probe-report.txt", Windows.Storage.CreationCollisionOption.replaceExisting
        ).then(function (file) {
            attempt.file = file;
            return Windows.Storage.FileIO.writeTextAsync(file, attempt.prefix + "status=steam-approval-starting\r\n");
        }).then(function () {
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
            return Windows.Storage.FileIO.writeTextAsync(attempt.file, attempt.prefix +
                "steam_qr_challenge=displayed\r\nstatus=steam-approval-ready\r\n").then(function () {
                schedulePoll(attempt, challenge);
            });
        }).done(null, function () {
            finish(attempt, "Steam could not prepare an approval code. Try again.", "steam_qr_challenge=failed");
        });
    }

    retry.addEventListener("click", start);
    cancel.addEventListener("click", function () {
        finish(current, "Sign-in cancelled.", "steam_approval=cancelled");
    });
    start();
})();
