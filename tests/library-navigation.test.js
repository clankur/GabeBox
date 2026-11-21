const test = require("node:test");
const assert = require("node:assert/strict");
const { adjacent } = require("../probe/signin/library-navigation.js");

const tile = (offsetLeft, offsetTop) => ({ offsetLeft, offsetTop });

test("horizontal navigation stays in the visual row", () => {
    const tiles = [tile(0, 0), tile(10, 0), tile(20, 0), tile(0, 12)];
    assert.equal(adjacent(tiles, tiles[1], "right"), tiles[2]);
    assert.equal(adjacent(tiles, tiles[1], "left"), tiles[0]);
    assert.equal(adjacent(tiles, tiles[2], "right"), undefined);
});

test("vertical navigation picks the nearest column on the next visual row", () => {
    const tiles = [tile(0, 0), tile(10, 0), tile(20, 0),
        tile(0, 12), tile(10, 12), tile(0, 24)];
    assert.equal(adjacent(tiles, tiles[2], "down"), tiles[4]);
    assert.equal(adjacent(tiles, tiles[4], "up"), tiles[1]);
    assert.equal(adjacent(tiles, tiles[5], "down"), undefined);
});
