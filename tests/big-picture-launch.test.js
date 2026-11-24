const assert = require("node:assert/strict");
const fs = require("node:fs");
const vm = require("node:vm");
const test = require("node:test");

const source = fs.readFileSync(require.resolve("../ui/big-picture/big-picture.js"), "utf8");
const drain = () => new Promise(resolve => setImmediate(resolve));

async function launch(component) {
    let time = 0;
    let timer;
    const reports = [];
    const file = { name: "big-picture-report.txt" };
    const localFolder = {
        async createFileAsync(name) {
            assert.equal(name, file.name);
            return file;
        }
    };
    const Windows = {
        Foundation: { GuidHelper: { createNewGuid: () => "fixture-id" } },
        Storage: {
            ApplicationData: { current: { localFolder } },
            CreationCollisionOption: { replaceExisting: 1 },
            FileIO: {
                async writeTextAsync(output, text) {
                    assert.equal(output, file);
                    const fields = Object.fromEntries(text.trim().split("\r\n").map(line => line.split("=")));
                    assert.equal(fields.run_id, "fixture-id");
                    assert.equal(fields.probe, "native-steam-big-picture");
                    reports.push(fields);
                }
            }
        }
    };
    const message = { textContent: "" };
    vm.runInNewContext(source, {
        Windows, BigPicture: { Native: { Launcher: component } },
        document: { body: message, getElementById: () => message },
        Date: { now: () => time },
        setTimeout(callback) { timer = callback; }
    });
    await drain();
    return {
        reports,
        async advance(milliseconds) {
            time += milliseconds;
            const callback = timer;
            timer = undefined;
            callback();
            await drain();
        }
    };
}

test("Steam can exit after its initial startup observation", async () => {
    let exitCode = -1;
    const run = await launch({ start: () => 123, pollExitCode: () => exitCode });
    assert.deepEqual(run.reports.map(r => r.status), ["launching", "client-created"]);
    await run.advance(10000);
    assert.equal(run.reports.at(-1).status, "client-running");
    assert.equal(run.reports.at(-1).client_pid, "0x0000007b");
    exitCode = 0;
    await run.advance(500);
    assert.equal(run.reports.at(-1).status, "client-exited");
    assert.equal(run.reports.at(-1).exit_code, "0x00000000");
});

test("native launch, process observation and early exit retain their failure details", async () => {
    const fail = () => { throw Object.assign(new Error("access denied"), { number: -2147024891 }); };
    const cases = [
        [{ start: fail }, "client-launch-error", "hresult", "0x80070005"],
        [{ start: () => 123, pollExitCode: fail }, "observation-error", "hresult", "0x80070005"],
        [{ start: () => 123, pollExitCode: () => 0xc0000135 }, "client-exited", "exit_code", "0xc0000135"]
    ];
    for (const [component, status, field, expected] of cases) {
        const { reports } = await launch(component);
        assert.equal(reports.at(-1).status, status);
        assert.equal(reports.at(-1)[field], expected);
        assert.ok(reports.every(report => report.status !== "client-running"));
    }
});
