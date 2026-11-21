(function () {
    "use strict";
    function create(runner, canvas, context, fit) {
        var metadata = new Uint32Array(3), pixels;
        var player = { image: null, draw: draw };
        function draw(measure) {
            var begin = measure && performance.now();
            var size = runner.steamSdlFrameSize();
            if (!size) return null;
            var width = size >>> 16, height = size & 65535;
            if (!width || width > 1920 || !height || height > 1080)
                throw new Error("Unexpected SDL word frame dimensions");
            if (!pixels || width !== canvas.width || height !== canvas.height) {
                canvas.width = width;
                canvas.height = height;
                player.image = context.createImageData(width, height);
                var bytes = player.image.data;
                pixels = new Uint32Array(bytes.buffer, bytes.byteOffset, width * height);
                fit();
            }
            var peeked = measure && performance.now();
            if (!runner.fillSteamSdlFrame(size, pixels, metadata)) return null;
            var filled = measure && performance.now();
            context.putImageData(player.image, 0, 0);
            var frame = { sequence: metadata[0], ack: metadata[1], events: metadata[2] };
            if (measure) {
                frame.nativeMs = peeked - begin;
                frame.readMs = filled - peeked;
                frame.uploadMs = performance.now() - filled;
            }
            return frame;
        }
        return player;
    }
    if (typeof module === "object" && module.exports) module.exports = create;
    else window.createSteamSdlWordFrame = create;
}());
