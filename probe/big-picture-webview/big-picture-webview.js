(function () {
    "use strict";
    var session;

    function switchView(to, from) {
        if (to === from) return Promise.resolve();
        var views = Windows.UI.ViewManagement.ApplicationViewSwitcher;
        function replace() { return views.switchAsync(to, from); }
        // The FHD presenter and Valve are two views of the same Xbox game.
        // First request an in-place switch in the current foreground slot.
        return Promise.resolve().then(replace).catch(function () {
            return Promise.resolve(views.tryShowAsStandaloneAsync(to)).then(replace);
        });
    }

    function open(broker, onFailure, timing) {
        if (session && session.cleanup) return session.cleanup.then(function () { return open(broker, onFailure, timing); });
        if (session) {
            if (session.broker === broker) return session.ready;
            if (!session.replaceable()) return Promise.reject(new Error("steam-view-busy"));
            return session.finish("account-changed", true).then(function () { return open(broker, onFailure, timing); });
        }
        function markStartup(stage) { if (timing) timing.mark(stage); }
        markStartup("shell_open");
        var storage = Windows.Storage, views = Windows.UI.ViewManagement;
        var documentRoot = window.document && window.document.documentElement;
        var primaryXaml = !!(documentRoot && documentRoot.getAttribute("data-steam-primary-xaml") === "true");
        var earlyPrimaryLibrary = primaryXaml && documentRoot.getAttribute("data-steam-early-library-reveal") === "true";
        var run = Windows.Foundation.GuidHelper.createNewGuid();
        var network, networkDiagnostic = "", storeNavigation = "";
        var storeStages = [];
        var storeJsStages = [];
        var file, report = Promise.resolve();
        var messages = [], last = "", servedGames, rendererError, surfaceState, startupRoute, popupPhase, captures = {}, memory = {}, gameFrames = {};
        var surfaceSizing = "", startupMovieOverride = "";
        var startupServices = {}, startupCloudReads = {};
        var warmReport = "", memoryRequest = "", triedMemory = false;
        var warming = false, warmStarted = 0, warmProgress = 0, warmSwitch = false;
        var nextChargeCheck = 0, heldForMemory = false, belowBudgetSince = 0;
        function sampleMemory(stage) {
            try {
                var manager = Windows.System.MemoryManager;
                var usage = manager.appMemoryUsage, limit = manager.appMemoryUsageLimit;
                if (typeof usage === "number" && isFinite(usage) && usage >= 0 &&
                    typeof limit === "number" && isFinite(limit) && limit > 0)
                    memory[stage] = "memory_" + stage + "_usage_bytes=" + usage + "\r\n" +
                        "memory_" + stage + "_limit_bytes=" + limit + "\r\n";
                var expected = manager.expectedAppMemoryUsageLimit;
                if (memory[stage] && Number.isSafeInteger(expected) && expected > 0)
                    memory[stage] += "memory_" + stage + "_expected_limit_bytes=" + expected + "\r\n";
                // Only a cold signed shell can make this one bounded request.
                // The platform has to advertise more headroom than the observed 1-GiB floor.
                var target = 1610612736;
                if (warming && !settled && !playing && !triedMemory && limit > 0 && limit < 1207959552 &&
                    Number.isSafeInteger(expected) && expected >= target && typeof manager.trySetAppMemoryUsageLimit === "function") {
                    triedMemory = true;
                    memoryRequest = "unavailable";
                    var granted = manager.trySetAppMemoryUsageLimit(target);
                    var currentLimit = manager.appMemoryUsageLimit;
                    memoryRequest = (granted ? "granted" : "declined") + " requested_bytes=" + target +
                        (Number.isSafeInteger(currentLimit) && currentLimit > 0 ? " current_bytes=" + currentLimit : "");
                }
                return Number.isSafeInteger(usage) && usage > 0 ? usage : undefined;
            } catch (_) {}
        }
        var resolveReady, rejectReady, readinessTimer, settled = false, failed = false;
        var ready = new Promise(function (resolve, reject) {
            resolveReady = resolve; rejectReady = reject;
        });
        function record(stage, refresh) {
            if (stage !== last) {
                last = stage;
                messages.push(stage);
                if (messages.length > 24) messages.shift();
            } else if (!refresh) return;
            var text = "run_id=" + run + "\r\nstatus=" + stage + "\r\n" +
                (servedGames === undefined ? "" : "owned_games_served=" + servedGames + "\r\n") +
                (warmReport ? "warmup=" + warmReport + "\r\n" : "") +
                (startupMovieOverride ? "startup_movie_override_js=" + startupMovieOverride + "\r\n" : "") +
                (Object.keys(startupServices).length ? "startup_service_elapsed_ms=" + Object.keys(startupServices).map(function (key) {
                    return key + ":" + startupServices[key];
                }).join(",") + "\r\n" : "") +
                (Object.keys(startupCloudReads).length ? "startup_cloud_cache_reads=" + Object.keys(startupCloudReads).map(function (key) {
                    return key + ":" + startupCloudReads[key];
                }).join(",") + "\r\n" : "") +
                (timing ? "startup_elapsed_ms=" + Object.keys(timing.marks).map(function (key) {
                    return key + ":" + timing.marks[key];
                }).join(",") + "\r\n" : "") +
                (memoryRequest ? "shell_memory_request=" + memoryRequest + "\r\n" : "") +
                (surfaceSizing ? "surface_sizing=" + surfaceSizing + "\r\n" : "") +
                (networkDiagnostic ? "xbox_network=" + networkDiagnostic + "\r\n" : "") +
                (storeJsStages.length ? "store_js_stages=" + storeJsStages.join(",") + "\r\n" : "") +
                (storeNavigation ? "public_store_navigation=" + storeNavigation + "\r\n" : "") +
                (storeStages.length ? "public_store_native=" + storeStages.join(",") + "\r\n" : "") +
                (surfaceState ? "valve_surface=" + surfaceState + "\r\n" : "") +
                (popupPhase ? "valve_popup=" + popupPhase + "\r\n" : "") +
                (startupRoute ? "valve_startup=" + startupRoute + "\r\n" : "") +
                Object.keys(captures).map(function (kind) { return kind + "_capture=" + captures[kind] + "\r\n"; }).join("") +
                Object.keys(gameFrames).map(function (key) { return "game_frame_" + key + "=" + gameFrames[key] + "\r\n"; }).join("") +
                (rendererError ? "renderer_error=" + rendererError + "\r\n" : "") +
                Object.keys(memory).map(function (key) { return memory[key]; }).join("") + messages.join("\r\n") + "\r\n";
            // Device Portal may be reading the report during a cold launch. Its
            // file lifetime must never gate Steam startup, input or foregrounding.
            report = report.then(function () {
                return file || storage.ApplicationData.current.localFolder.createFileAsync("big-picture-webview-report.txt",
                    storage.CreationCollisionOption.openIfExists);
            }).then(function (opened) {
                file = opened;
                return storage.FileIO.writeTextAsync(file, text);
            }).catch(function () { file = null; });
        }
        var host, viewId, mainId, visible = false, activeSwitch, timer, playing = false;
        function show(to, from) {
            return switchView(to, from);
        }
        var playingApp = 0, presented = false, returnToGame, quitGame;
        function recordGameFrame(appId, stage, stats) {
            if (failed || !playing || appId !== playingApp || typeof stats !== "string" ||
                (stage !== "first" && stage !== "sample" && stage !== "fallback" && stage !== "media" && stage !== "media_capture")) return;
            var fields = stats.split(/\r?\n/).filter(function (line) {
                return /^(width|height|visible|activation_mode|presented|gpu_presented|cpu_presented|primary_media|primary_panel|media_error|sample_us|sample_frames)=[0-9]{1,16}$/.test(line) ||
                    /^(submitted|processed|requests|pending|outstanding|slots_busy|source_sequence|cpu_submitted|gpu_submitted|pool_waits|source_waits|spill_allocations|texture_peak|gpu_sequence|cpu_sequence)=[0-9]{1,16}$/.test(line) ||
                    /^sample_(render_callbacks|source_empty_callbacks|present_busy_callbacks|gpu_sequence)=[0-9]{1,16}$/.test(line) ||
                    /^(capture_saved|video_ms|sample_window)=[0-9]{1,16}$/.test(line) || /^media_phase=(open|attach|playback|panel)$/.test(line);
            });
            if (!fields.length) return;
            if (stage === "sample" && gameFrames.sample) stage = "latest_sample";
            gameFrames[stage] = "appid=" + appId + " " + fields.join(" ");
            record(last, true);
        }
        var gameForeground = false;
        var firstActivation = true;
        function setGameForeground(value) {
            if (!host || failed || value === gameForeground) return;
            gameForeground = value;
            host.reply(JSON.stringify({nativeGameForeground: value}));
            if (window.dispatchEvent) window.dispatchEvent(new CustomEvent("steam-game-foreground", {detail: value}));
        }
        function gameEvent(overlay) {
            if (!host || failed) return;
            var hosted = (window.steamSdlRuntimes || []).some(function (runtime) { return runtime.appId === playingApp; });
            host.reply(JSON.stringify({event: "xbox-game", appId: presented && hosted ? playingApp : 0, overlay: overlay}));
        }
        function showValve(from) {
            if (failed || !viewId || visible) return Promise.resolve();
            var wasGame = gameForeground;
            setGameForeground(false);
            // The initial Steam window needs its own Xbox foreground allocation.
            // Replacing the QR view directly can retire the primary WWA host.
            if (!activeSwitch) activeSwitch = (firstActivation && viewId !== mainId ?
                Promise.resolve(views.ApplicationViewSwitcher.tryShowAsStandaloneAsync(viewId)).then(function (shown) {
                    if (!shown) throw new Error("Xbox could not show the Steam window");
                    return views.ApplicationViewSwitcher.switchAsync(viewId, mainId);
                }) : show(viewId, from || mainId)).then(function () {
                firstActivation = false;
                markStartup("shell_revealed");
                sampleMemory("steam_view");
                record(last, true);
                visible = true; activeSwitch = null;
            }, function (error) { activeSwitch = null; if (wasGame) setGameForeground(true); throw error; });
            return activeSwitch;
        }
        function handoffWarmup(reason) {
            if (warmSwitch || failed || !viewId) return;
            var now = Date.now();
            if (now < nextChargeCheck) return;
            nextChargeCheck = now + 500;
            // Xbox gives the secondary Steam view 1 GiB. A time-based switch
            // while Chromium compiled Valve's shell was observed at 2.28 GiB.
            // Keep the QR view's allocation until the native charge settles.
            var usage = sampleMemory("warm_candidate");
            if (!primaryXaml && (usage > 992 * 1024 * 1024 || (heldForMemory && !usage))) {
                belowBudgetSince = 0;
                if (!heldForMemory) {
                    heldForMemory = true;
                    warmReport = "waiting-for-memory " + reason;
                    record(last, true);
                }
                return;
            }
            if (heldForMemory) {
                if (!belowBudgetSince) belowBudgetSince = now;
                if (now - belowBudgetSince < 1500) return;
            }
            warmSwitch = true;
            markStartup("shell_warmup_handoff");
            warmReport = reason + " elapsed_ms=" + Math.max(0, Date.now() - warmStarted);
            sampleMemory("warm_handoff");
            record(last, true);
            showValve(mainId).then(function () {
                if (failed) return;
                host.reply(JSON.stringify({nativeWarmupComplete: true}));
            }).catch(fail);
        }
        function showForGame(appId, from, restore, requestExit) {
            if (!played(appId) || !from || typeof restore !== "function")
                return Promise.resolve(false);
            returnToGame = restore; quitGame = requestExit;
            return showValve(from).then(function () {
                if (!playing || playingApp !== appId) return false;
                sampleMemory("overlay"); gameEvent(true); record("game-switch-to-steam");
                // Xbox can revise the budget after the view switch resolves.
                [500, 2000, 5000].forEach(function (delay) {
                    setTimeout(function () {
                        if (!visible || !played(appId) || returnToGame !== restore) return;
                        sampleMemory("overlay_" + delay + "ms");
                        record(last, true);
                    }, delay);
                });
                return true;
            });
        }
        function raiseGame(appId) {
            if (!visible || !played(appId) || !returnToGame) return Promise.resolve("unavailable");
            var restore = returnToGame;
            returnToGame = null;
            return Promise.resolve().then(function () { return restore(viewId); }).then(function (shown) {
                if (!shown || !played(appId)) return "unavailable";
                visible = false;
                setGameForeground(true);
                sampleMemory("resume_game"); gameEvent(false); record("game-switch-to-game");
                return "presented";
            }, function () {
                if (played(appId)) returnToGame = restore;
                return "failed";
            });
        }
        function played(appId) { return !failed && playing && presented && appId === playingApp; }
        function playGame(appId) {
            if (playing) {
                if (!played(appId) || !returnToGame) return "busy";
                return broker.session().then(function (user) { return user && user.steamId ? raiseGame(appId) : "signed-out"; });
            }
            playing = true; playingApp = appId; gameFrames = {};
            // Make the expensive view handoff only after the broker has
            // verified this account, the title and its local installation.
            return broker.play(appId, function () {
                // The QR host creates the game's fullscreen presenter.
                return show(mainId, viewId).then(function () {
                    visible = false;
                    setGameForeground(true);
                    sampleMemory("handoff");
                    return record("game-window-backgrounded");
                });
            }).then(function (outcome) {
                if (outcome === "presented" || outcome === "window-ready") {
                    presented = playing && playingApp === appId && outcome === "presented";
                    sampleMemory("first_frame"); gameEvent(false);
                    record("game-" + outcome);
                    return outcome;
                }
                return resume().then(function () { return outcome; });
            }, function (err) { return resume().then(function () { throw err; }); });
        }
        function playConfiguredTrial(appId) {
            return Promise.resolve().then(function () {
                if (!broker || !settled || !visible || failed) return "unavailable";
                if (playing) return "busy";
                if (!Number.isInteger(appId) || appId <= 0 || appId > 0x00ffffff) return "invalid-app";
                markStartup("configured_play");
                record("configured-game-trial");
                return playGame(appId);
            });
        }
        function resume(exitedApp) {
            if (warming && !settled) return Promise.resolve();
            if (exitedApp && playingApp && exitedApp !== playingApp) return Promise.resolve();
            var returning = playing;
            playing = presented = false;
            playingApp = 0; returnToGame = quitGame = null;
            return showValve(mainId).then(function () {
                if (returning && !playing && !failed) {
                    sampleMemory("return"); gameEvent(false); return record("game-returned-to-steam");
                }
            });
        }
        function respond(id, ok, value) {
            if (failed) return;
            if (ok) host.reply(JSON.stringify({id: id, result: value}));
            else host.reply(JSON.stringify({id: id, error: value}));
        }
        function handle(raw) {
            if (failed) return;
            var request;
            try { request = JSON.parse(raw); } catch (_) { return; }
            if (!request || typeof request !== "object") return;
            if (request.method === "store-native-stage") {
                if (!/^(create-received|creating-controller|configuring-controller|initial-document-confirmed|opening-bootstrap|bootstrap-started|bootstrap-served|bootstrap-denied|initial-about-blank|bootstrap-complete|bootstrap-failed|public-requested|command-error|error|visible|hidden|http-status-unavailable|browser-process-failed)$/.test(request.stage) ||
                    !Number.isInteger(request.code)) return;
                if (request.stage === "create-received") { storeStages = []; storeNavigation = ""; }
                if (storeStages.length >= 16) storeStages.shift();
                storeStages.push(request.stage + (request.code ? "=" + request.code : ""));
                record(last, true);
                return;
            }
            if (request.method === "store-js-stage") {
                var stage = request.stage;
                if (["create", "visible", "geometry", "blank-complete", "initial-ready", "navigate", "remote-start",
                        "remote-complete", "failed", "closed"].indexOf(stage) < 0) return;
                if (stage === "create") storeJsStages = [];
                if (storeJsStages.indexOf(stage) < 0) { storeJsStages.push(stage); record(last, true); }
                return;
            }
            if (request.method === "store-navigation") {
                if (typeof request.complete !== "boolean" || typeof request.success !== "boolean" ||
                    !Number.isInteger(request.http) || request.http < 0 || request.http > 599) return;
                storeNavigation = request.complete ? (request.success ? "loaded" : "failed") +
                    " http=" + request.http : "pending";
                record(last, true);
                return;
            }
            if (request.method === "capture-library-result" || request.method === "capture-surface-result") {
                var kind = request.method === "capture-library-result" ? "library" : "surface";
                if (captures[kind] !== "requested") return;
                if (request.state === "saved" && request.viewId === viewId && Number.isInteger(request.browserPid) &&
                    request.browserPid > 0 && Number.isInteger(request.width) && request.width > 0 && request.width <= 16384 &&
                    Number.isInteger(request.height) && request.height > 0 && request.height <= 16384) {
                    captures[kind] = "saved width=" + request.width + " height=" + request.height +
                        " view=" + viewId + " browser_pid=" + request.browserPid;
                } else if (request.state === "error" || request.state === "not-visible") captures[kind] = request.state;
                else return;
                record(last, true);
                return;
            }
            if (request.method === "renderer-error") {
                // Keep only the first short script failure. Exclude URLs or sign-in tokens.
                if (!rendererError) {
                    rendererError = String(request.message || "script error").slice(0, 180)
                        .replace(/https?:\/\/\S+/gi, "[url]").replace(/[\r\n]/g, " ");
                    // Independent stores can fail while the gamepad window goes on to render.
                    record(last, true);
                }
                return;
            }
            if (request.method === "surface-sizing") {
                var labels = ["view_width", "view_height", "screen_width", "screen_height", "display_scale_milli",
                    "bounds_width", "bounds_height", "raster_scale_milli", "css_width", "css_height", "dpr_milli", "backing_scale_milli"];
                if (Array.isArray(request.values) && request.values.length === labels.length &&
                    request.values.every(function (n) { return Number.isInteger(n) && n >= 0 && n <= 16384; })) {
                    surfaceSizing = labels.map(function (label, i) { return label + "=" + request.values[i]; }).join(" ");
                    record(last, true);
                }
                return;
            }
            if (request.method === "surface-state" && Number.isInteger(request.state) && request.state >= 1 && request.state <= 8) {
                surfaceState = ["", "setup", "library-or-home", "no-portal", "empty-portal", "error-boundary", "grid", "content", "empty-content"][request.state];
                record(last, true);
                return;
            }
            if (request.method === "startup-popup" && /^(?:[01]-){4}(?:[0-9]{1,2}-){4}[01]-[01]-[01]$/.test(request.phase)) {
                if (warming && request.phase !== popupPhase) warmProgress = Date.now();
                popupPhase = request.phase;
                record(last, true);
                return;
            }
            if (request.method === "startup-route" && /^(waiting-user|waiting-store|waiting-window|waiting-portal|selecting-library|library-selected|selection-pending)$/.test(request.stage)) {
                if (warming && request.stage !== startupRoute) warmProgress = Date.now();
                startupRoute = request.stage;
                if (request.stage === "library-selected") {
                    markStartup("shell_library_selected");
                    sampleMemory("library_selected");
                }
                record(last, true);
                return;
            }
            if (request.method === "startup-movie-override-js" && !startupMovieOverride && request.state === "absent-404") {
                markStartup("browser_startup_movie_override_js");
                startupMovieOverride = request.state;
                record(last, true);
                return;
            }
            if (request.method === "startup-service-timing" &&
                    /^(total|key|key_cloud|key_friends|key_settings|key_tour|apps|apps_store|friends|friends_store|collections|collections_store|remaining|remaining_appinfo|remaining_ui|remaining_library)$/.test(request.label) &&
                    Number.isInteger(request.elapsedMs) && request.elapsedMs >= 0 && request.elapsedMs <= 120000 &&
                    startupServices[request.label] === undefined) {
                startupServices[request.label] = request.elapsedMs;
                if (/^(total|key|apps|friends|collections|remaining)$/.test(request.label))
                    markStartup("service_" + request.label + "_reported");
                record(last, true);
                return;
            }
            if (request.method === "startup-cloud-cache-read" && /^(index|namespace-[134](?:\.modified)?)$/.test(request.label) &&
                    [200, 403, 404, 503].indexOf(request.status) >= 0 && startupCloudReads[request.label] === undefined) {
                startupCloudReads[request.label] = request.status;
                markStartup("cloud_cache_" + request.label.replace(/[^a-z0-9]/g, "_"));
                record(last, true);
                return;
            }
            var id = request.id, appId = request.appId;
            if (typeof id !== "number" || !isFinite(id) || id <= 0 || id > 0x7fffffff || id % 1) return;
            if (!broker) return respond(id, false, "signed-out");
            var action;
            if (request.method === "library") action = function () { return broker.library(); };
            else if (request.method === "session") action = function () { return broker.session(); };
            else if (request.method === "storage-list") action = function () { return broker.storage("list", ""); };
            else if (request.method === "storage-select" || request.method === "storage-move") {
                if (!settled || !visible || playing) return respond(id, false, "unavailable");
                var argument = request.argument;
                if (request.method === "storage-select" && (argument === "D" || argument === "E"))
                    action = function () { return broker.storage("select", argument); };
                else if (request.method === "storage-move" && argument &&
                        (argument.destination === "D" || argument.destination === "E") &&
                        Array.isArray(argument.appIds) && argument.appIds.length === 1 &&
                        Number.isInteger(argument.appIds[0]) && argument.appIds[0] > 0 && argument.appIds[0] <= 0xffffff) {
                    action = function () {
                        return broker.storage("move", {appIds: argument.appIds, destination: argument.destination});
                    };
                } else return respond(id, false, "unavailable");
            }
            else if (request.method === "sign-out") action = function () {
                if (!settled || !visible || playing || failed) return "busy";
                return broker.signOut();
            };
            else if (request.method === "restart-client") action = function () {
                return broker.session().then(function (user) {
                    if (!user || !user.steamId) return "signed-out";
                    return settled && visible && !playing && !failed ? "restart" : "busy";
                });
            };
            else if (request.method === "network-state") action = function () {
                if (!network) network = window.steamXboxNetwork(Windows, function () {
                    if (!failed && host) host.reply(JSON.stringify({event: "xbox-network-changed"}));
                });
                var state = network.read();
                var diagnostic = state.level + (state.kind ? " " + state.kind : "");
                if (diagnostic !== networkDiagnostic) { networkDiagnostic = diagnostic; record(last, true); }
                return state;
            };
            else if (request.method === "system-mode") action = function () { return window.steamXboxSystemMode(Windows); };
            else if ((request.method === "play" || request.method === "install" || request.method === "resume-game" || request.method === "quit-game") &&
                     typeof appId === "number" && appId > 0 && appId <= 0xffffffff && appId % 1 === 0) {
                if (!settled) return respond(id, false, "unavailable");
                if (request.method === "resume-game" || request.method === "quit-game") action = function () {
                    if (request.method === "quit-game") record("game-quit-received");
                    return broker.session().then(function (user) {
                        if (!user || !user.steamId) return "signed-out";
                        if (request.method === "resume-game") return raiseGame(appId);
                        if (!visible || !played(appId) || !quitGame) return "unavailable";
                        if (!quitGame()) return "unavailable";
                        quitGame = null;
                        record("game-quit-requested");
                        return "quit-requested";
                    });
                };
                else if (request.method === "install") action = function () {
                    if (!visible || playing) return "busy";
                    return broker.install(appId, function (stage) {
                        if (!failed && host) {
                            try { host.reply(JSON.stringify({event: "xbox-install", appId: appId, stage: stage})); }
                            catch (_) { /* The native download continues if the view closes. */ }
                        }
                    });
                };
                else action = function () { return playGame(appId); };
            } else return respond(id, false, "unavailable");
            Promise.resolve().then(function () { if (!failed) return action(); }).then(function (result) {
                respond(id, true, result);
                if (request.method === "sign-out" && result === "signed-out" && !failed) {
                    finish("signed-out", true);
                    return;
                }
                if (request.method === "restart-client" && result === "restart" && !playing && !failed) {
                    record("client-restart");
                    host.reply(JSON.stringify({nativeRestartClient: true}));
                    return;
                }
                if (!failed && request.method === "library" && Array.isArray(result) && servedGames !== result.length) {
                    if (warming) warmProgress = Date.now();
                    markStartup("shell_library_served");
                    servedGames = result.length;
                    host.reply(JSON.stringify({nativeEmptyLibrary: servedGames === 0}));
                    record(last, true);
                }
            }, function () { respond(id, false, "failed"); });
        }
        function fail(error, stage) {
            var code = typeof error.number === "number" ? "0x" + (error.number >>> 0).toString(16) : "unavailable";
            finish(stage || "host-error:" + code);
        }
        function finish(stage, intentional) {
            if (failed) return session && session.cleanup || Promise.resolve();
            failed = true;
            if (network) network.close();
            clearInterval(timer);
            clearTimeout(readinessTimer);
            record(stage);
            var wasReady = settled;
            if (!settled) { settled = true; rejectReady(new Error(stage)); }
            if (broker) {
                var restore = activeSwitch || Promise.resolve();
                var current = session;
                current.cleanup = restore.catch(function () {}).then(function () {
                    if (visible && viewId) return show(mainId, viewId);
                }).catch(function () {}).then(function () {
                    if (!host) return;
                    try { host.close(); } catch (_) { return; }
                    // Closing the CoreWindow is dispatched to its UI thread.
                    var deadline = Date.now() + 2500;
                    function check() {
                        if (!host.viewId() || Date.now() >= deadline) return;
                        return new Promise(function (resolve) { setTimeout(resolve, 50); }).then(check);
                    }
                    return check();
                }).catch(function () {}).then(function () {
                    if (session === current) session = null;
                    if (wasReady && !intentional && typeof onFailure === "function") onFailure(new Error(stage));
                });
                return current.cleanup;
            }
            return Promise.resolve();
        }
        var lastNative, nextCaptureCheck = 0, lookingForCapture = false;
        function checkCaptureRequest() {
            if (!broker || !settled || !visible || failed || lookingForCapture || Date.now() < nextCaptureCheck) return;
            if (captures.library === "requested" || captures.surface === "requested") return;
            nextCaptureCheck = Date.now() + 4000;
            var folder = storage.ApplicationData.current.localFolder;
            if (!folder.tryGetItemAsync) return;
            lookingForCapture = true;
            var kind = "library";
            Promise.resolve().then(function () {
                return folder.tryGetItemAsync("big-picture-capture-library.request");
            }).then(function (request) {
                if (request) return request;
                kind = "surface";
                return folder.tryGetItemAsync("big-picture-surface.request");
            }).then(function (request) {
                if (!request || failed || !visible) return;
                return request.deleteAsync().then(function () {
                    if (failed || !visible) return;
                    captures[kind] = "requested";
                    host.reply(JSON.stringify(kind === "library" ? {nativeCaptureLibrary: true} : {nativeCaptureSurface: true}));
                    record(last, true);
                });
            }).then(function () { lookingForCapture = false; }, function () { lookingForCapture = false; });
        }
        function poll() {
            try {
                var stage = host.status();
                if (stage !== lastNative) {
                    if (warming) warmProgress = Date.now();
                    if (/^(environment|controller|navigation|document-loaded|gamepad-window-created|app-initialized|warmup-gamepad-layout|warmup-setup-layout)$/.test(stage))
                        markStartup("browser_" + stage.replace(/-/g, "_"));
                    if (visible && (stage === "setup-ready" || stage === "gamepad-ready"))
                        markStartup("shell_" + stage.replace(/-/g, "_"));
                    if (stage === "gamepad-window-created") sampleMemory("steam_popup");
                    if (stage === "gamepad-ready") sampleMemory("steam_ready");
                    record(stage);
                    lastNative = stage;
                }
                if (broker && /-error(?:$|:)/.test(stage)) return fail(new Error(stage), stage);
                if (!viewId) {
                    viewId = host.viewId();
                    if (viewId) {
                        if (warming) {
                            warmStarted = warmProgress = Date.now();
                            markStartup("shell_warmup_started");
                            sampleMemory("warm_view");
                            warmReport = "preparing";
                            record(last, true);
                        } else resume().catch(fail);
                    }
                }
                if (!settled && visible && (stage === "gamepad-ready" || stage === "setup-ready")) {
                    markStartup("shell_" + stage.replace(/-/g, "_"));
                    sampleMemory("steam_foreground");
                    settled = true; clearTimeout(readinessTimer);
                    warming = false;
                    record(last, true);
                    resolveReady();
                }
                for (var i = 0; i < 16; ++i) {
                    var request = host.read();
                    if (!request) break;
                    handle(request);
                }
                if (warming && viewId && !warmSwitch) {
                    if (stage === "warmup-gamepad-layout" || stage === "warmup-setup-layout") handoffWarmup(stage);
                    // The primary XAML layout stays within the same foreground window.
                    // Reveal Library sooner while native observation still gates readiness.
                    else if (earlyPrimaryLibrary && viewId === mainId && servedGames !== undefined)
                        handoffWarmup("primary-library-served");
                    else if (Date.now() - warmStarted >= 32000) handoffWarmup("time-limit");
                    else if (Date.now() - warmProgress >= 14000) handoffWarmup("no-layout-progress");
                }
                checkCaptureRequest();
            } catch (error) { fail(error); }
        }
        if (broker) readinessTimer = setTimeout(function () {
            if (heldForMemory && !warmSwitch) {
                sampleMemory("warm_timeout");
                return fail(new Error("Steam did not fit the Xbox foreground memory budget"), "warmup-memory-timeout");
            }
            fail(new Error("Timed out waiting for Valve's gamepad window"), "gamepad-timeout:" + (lastNative || "starting"));
        }, 120000);
        sampleMemory("startup");
        record("starting");
        var startup = Promise.resolve().then(function () {
            mainId = views.ApplicationView.getForCurrentView().id;
            host = BigPictureView.Native.Renderer;
            record("winrt-projected");
            record("status-before-open:" + host.status());
            if (!broker) { host.open(); return; }
            return broker.session().then(function (identity) {
                if (failed) return;
                var owner = identity && identity.steamId;
                if (typeof owner !== "string" || !/^[1-9][0-9]{16,19}$/.test(owner) ||
                    (owner.length === 20 && owner > "18446744073709551615")) throw new Error("signed-out");
                markStartup("shell_identity_verified");
                warming = true;
                if (primaryXaml) host.openForPrimaryAccount(owner);
                else host.openForAccount(owner);
            });
        }).then(function () {
            if (failed) return;
            record("open-returned");
            timer = setInterval(poll, 80);
            poll();
        }).catch(fail);
        if (!broker) ready.catch(function () {});
        session = {broker: broker, replaceable: function () { return !!broker && !playing; }, finish: finish,
            resume: resume, showForGame: showForGame, playConfiguredTrial: playConfiguredTrial,
            sampleMemory: sampleMemory, recordGameFrame: recordGameFrame,
            ready: broker ? ready : startup};
        return session.ready;
    }
    window.steamBigPictureHost = {
        open: open,
        switchView: switchView,
        recordGameFrame: function (appId, stage, stats) { if (session) session.recordGameFrame(appId, stage, stats); },
        samplePresenter: function (stage) {
            if (session && (stage === "presenter_created" || stage === "presenter_foreground")) session.sampleMemory(stage);
        },
        resume: function (appId) { return session ? session.resume(appId) : Promise.resolve(); },
        playConfiguredTrial: function (appId) {
            return session ? session.playConfiguredTrial(appId) : Promise.resolve("unavailable");
        },
        showForGame: function (appId, viewId, restore, requestExit) {
            return session ? session.showForGame(appId, viewId, restore, requestExit) : Promise.resolve(false);
        }
    };
}());
