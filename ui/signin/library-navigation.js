var libraryNavigation = (function () {
    "use strict";

    function adjacent(tiles, target, direction) {
        var vertical = direction === "up" || direction === "down";
        var forward = direction === "down" || direction === "right";
        var row = target.offsetTop;
        var column = target.offsetLeft;
        var closest, bestPrimary = Infinity, bestSecondary = Infinity;

        for (var i = 0; i < tiles.length; ++i) {
            var tile = tiles[i];
            var dy = tile.offsetTop - row;
            var dx = tile.offsetLeft - column;
            if (!vertical && dy) continue;
            var primary = (vertical ? dy : dx) * (forward ? 1 : -1);
            if (primary <= 0) continue;
            var secondary = Math.abs(vertical ? dx : dy);
            if (primary < bestPrimary || (primary === bestPrimary && secondary < bestSecondary)) {
                closest = tile;
                bestPrimary = primary;
                bestSecondary = secondary;
            }
        }
        return closest;
    }

    return { adjacent: adjacent };
}());

if (typeof module === "object" && module.exports) module.exports = libraryNavigation;
