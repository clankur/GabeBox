const assert = require("node:assert/strict");
const test = require("node:test");
const create = require("../probe/signin/steam-sdl-word-frame.js");

test("the projected word reader reuses pixel and header arrays and skips empty and resized frames", () => {
    let size = 0, ready = true, fills = [], shown = [], allocations = 0;
    const runner = {
        steamSdlFrameSize: () => size,
        fillSteamSdlFrame(dimensions, words, meta) {
            assert.equal(dimensions, size);
            assert.ok(words instanceof Uint32Array && meta instanceof Uint32Array);
            assert.equal(words.length, (size >>> 16) * (size & 65535));
            fills.push({ words, meta });
            words[0] = 0xff332211;
            meta.set([fills.length, 123, 456]);
            return ready;
        }
    };
    const context = {
        createImageData(w,h) { allocations++; return { data: new Uint8ClampedArray(w*h*4) }; },
        putImageData(image) { shown.push(Array.from(image.data.slice(0,4))); }
    };
    const canvas = {};
    const player = create(runner, canvas, context, () => {});
    assert.equal(player.draw(false), null);
    assert.equal(fills.length, 0);
    size = 2 << 16 | 1;
    assert.deepEqual(player.draw(false), { sequence: 1, ack: 123, events: 456 });
    assert.deepEqual(shown[0], [17,34,51,255]);
    player.draw(false);
    assert.equal(fills[0].words, fills[1].words);
    assert.equal(fills[0].meta, fills[1].meta);
    assert.equal(allocations, 1);
    ready = false;
    assert.equal(player.draw(false), null);
    assert.equal(shown.length, 2);
    ready = true;
    size = 1 << 16 | 2;
    player.draw(false);
    assert.equal(allocations, 2);
    assert.notEqual(fills[0].words, fills[3].words);
    assert.equal(fills[0].meta, fills[3].meta);
});
