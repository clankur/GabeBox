const assert = require('node:assert/strict');
const fs = require('node:fs');
const test = require('node:test');
const vm = require('node:vm');

const source = fs.readFileSync(require.resolve('../big-picture-bridge/xbox-startup-timing.js'), 'utf8');
const total = 'SteamApp Init - After Login total time';

function fixture(locales, enabled = true) {
    let now = 1000;
    const messages = [], output = [], console = {};
    for (const method of ['time', 'timeEnd', 'groupCollapsed', 'groupEnd', 'log']) {
        console[method] = function (...args) {
            assert.equal(this, console);
            output.push([method, ...args]);
            return method;
        };
    }
    const host = {postMessage({method, label, elapsedMs}) { messages.push([method, label, elapsedMs]); }};
    vm.runInNewContext(source + '\ninstallXboxStartupTiming(host, enabled, startupLocales);',
        {host, enabled, startupLocales: () => locales, xboxNativeWindow: {deviceFamily: 'Windows.Xbox'},
            performance: {now: () => now}, console});
    return {messages, console, output, advance(ms) { now += ms; }};
}

test('Valve startup timing preserves console output and exposes only bounded allowlisted numeric timings', () => {
    const app = fixture(['en-US']);
    assert.equal(app.console.time(total), 'time');
    app.advance(30000);
    app.console.groupCollapsed('SteamApp Init - After Login - Key dependencies - 1,200.4ms');
    app.console.log('async CloudStorage - 979.8ms');
    app.console.log('async FriendsChatStore - Startup - 10.6ms');
    app.console.log('async SettingsStore - 43.9ms');
    app.console.log('async CloudStorage - 99ms');
    app.console.log('async Other Store - 5ms');
    app.console.groupEnd();
    app.console.log('async GuidedTourStore - 4ms');
    app.console.groupCollapsed('SteamApp Init - After Login - AppStore - 12ms');
    app.console.log('async AppStore - 11ms');
    app.console.groupEnd();
    app.console.timeEnd(total);
    assert.equal(app.output.length, 13);
    assert.deepEqual(app.messages, [
        ['startup-service-timing', 'key', 1200],
        ['startup-service-timing', 'key_cloud', 980],
        ['startup-service-timing', 'key_friends', 11],
        ['startup-service-timing', 'key_settings', 44],
        ['startup-service-timing', 'apps', 12],
        ['startup-service-timing', 'apps_store', 11],
        ['startup-service-timing', 'total', 30000]
    ]);
});

test('the debugger handles localized durations, ignores unsupported values and defaults to disabled', () => {
    const app = fixture(['de-DE']);
    app.console.groupCollapsed('SteamApp Init - After Login - CollectionStore - 10.020,537ms');
    app.console.log('async CollectionStore - untrusted content');
    app.console.log('async CollectionStore - 120.001ms');
    app.console.groupEnd();
    assert.deepEqual(app.messages, [['startup-service-timing', 'collections', 10021]]);
    const disabled = fixture(['en-US'], false);
    disabled.console.groupCollapsed('SteamApp Init - After Login - FriendStore - 66ms');
    assert.deepEqual(disabled.messages, []);
});
