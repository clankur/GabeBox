const assert = require("node:assert/strict");
const test = require("node:test");
const { create } = require("../ui/signin/steam-sdl-timing.js");

function sample(frameForTick, resetAt) {
    const timing = create();
    let report;
    for (let tick = 0; tick <= 3600 && !report; tick++) {
        if (tick === resetAt) timing.reset();
        const sequence = frameForTick(tick);
        const frame = sequence === null ? null : { sequence, nativeMs: 2, readMs: 3, uploadMs: 4 };
        report = timing.observe(tick * (1000 / 60), frame);
    }
    return Object.fromEntries(report.trim().split("\r\n").map(line => line.split("=")));
}

test("measures unique canvas frames, intervals and WinRT stage costs independently of RAF", () => {
    const values = sample(tick => Math.floor(tick / 2));
    assert.equal(values.steam_sdl_raf_ticks, "1800");
    assert.equal(values.steam_sdl_presented_frames, "900");
    assert.equal(values.steam_sdl_presented_fps, "30.00");
    assert.equal(values.steam_sdl_min_rolling_1s_fps, "30");
    assert.equal(values.steam_sdl_interval_p95_ms, "33.33");
    assert.equal(values.steam_sdl_interval_p99_ms, "33.33");
    assert.equal(values.steam_sdl_intervals_over_33_33_ms, "0");
    assert.equal(values.steam_sdl_intervals_over_50_ms, "0");
    assert.equal(values.steam_sdl_read_pixels_mean_ms, "3.00");
});

test("a one-second stall between second boundaries reaches the rolling minimum", () => {
    const values = sample(tick => tick >= 106 && tick <= 167 ? null : tick);
    assert.equal(values.steam_sdl_min_rolling_1s_fps, "0");
    assert.equal(values.steam_sdl_intervals_over_50_ms, "1");
    assert.equal(values.steam_sdl_interval_p50_ms, "16.67");
    assert.equal(values.steam_sdl_interval_max_ms, "1050.00");
    assert.equal(values.steam_sdl_interval_max_start_ms, "1750");
});

test("discarding a sample on blur excludes a background stall", () => {
    const values = sample(tick => tick < 1200 ? null : tick, 1200);
    assert.equal(values.steam_sdl_min_rolling_1s_fps, "60");
    assert.equal(values.steam_sdl_presented_fps, "60.00");
});

test("a responsive host with a stopped producer reports zero FPS", () => {
    const values = sample(() => null);
    assert.equal(values.steam_sdl_presented_fps, "0.00");
    assert.equal(values.steam_sdl_min_rolling_1s_fps, "0");
    assert.equal(values.steam_sdl_interval_p99_ms, "unavailable");
});
