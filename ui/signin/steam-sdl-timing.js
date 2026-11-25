(function () {
    "use strict";

    // The frame count in (t, t+1000] can fall only when a frame leaves the window.
    function slowestSecond(start, end, timestamps) {
        var min = Infinity, right = 0, left = 0, t = start;
        while (t + 1000 <= end) {
            while (right < timestamps.length && timestamps[right] <= t + 1000 + 1e-7) right++;
            min = Math.min(min, right - left);
            if (left === timestamps.length) break;
            t = timestamps[left++];
            while (left < timestamps.length && timestamps[left] === t) left++;
            right = Math.max(right, left);
        }
        return min === Infinity ? 0 : min;
    }

    function create() {
        var start, previous, lastSequence, timestamps, intervals, raf, nativeMs, readMs, uploadMs, done;
        var longestGap, longestGapAt;

        function reset() {
            start = previous = lastSequence = undefined;
            timestamps = [];
            intervals = [];
            raf = nativeMs = readMs = uploadMs = longestGap = longestGapAt = 0;
        }
        reset();

        function observe(now, frame) {
            if (done) return null;
            if (start === undefined) {
                start = now;
                if (frame) {
                    previous = now;
                    lastSequence = frame.sequence;
                }
                return null;
            }
            raf++;
            if (frame && frame.sequence !== lastSequence) {
                if (previous !== undefined) {
                    var interval = now - previous;
                    intervals.push(interval);
                    if (interval > longestGap) {
                        longestGap = interval;
                        longestGapAt = previous - start;
                    }
                }
                previous = now;
                lastSequence = frame.sequence;
                timestamps.push(now);
                nativeMs += frame.nativeMs;
                readMs += frame.readMs;
                uploadMs += frame.uploadMs;
            }
            var elapsed = now - start;
            if (elapsed < 30000) return null;
            done = true;
            var frames = timestamps.length;
            intervals.sort(function (a, b) { return a - b; });
            function percentile(p) {
                return intervals.length ? intervals[Math.ceil(intervals.length * p) - 1].toFixed(2) : "unavailable";
            }
            function average(total) { return frames ? (total / frames).toFixed(2) : "unavailable"; }
            return "steam_sdl_sample_ms=" + Math.round(elapsed) + "\r\n" +
                "steam_sdl_presented_frames=" + frames + "\r\n" +
                "steam_sdl_presented_fps=" + (1000 * frames / elapsed).toFixed(2) + "\r\n" +
                "steam_sdl_min_rolling_1s_fps=" + slowestSecond(start, now, timestamps) + "\r\n" +
                "steam_sdl_interval_max_ms=" + longestGap.toFixed(2) + "\r\n" +
                "steam_sdl_interval_max_start_ms=" + Math.round(longestGapAt) + "\r\n" +
                "steam_sdl_interval_p50_ms=" + percentile(0.50) + "\r\n" +
                "steam_sdl_interval_p95_ms=" + percentile(0.95) + "\r\n" +
                "steam_sdl_interval_p99_ms=" + percentile(0.99) + "\r\n" +
                "steam_sdl_intervals_over_33_33_ms=" + intervals.filter(function (v) { return v > 1000 / 30 + 0.001; }).length + "\r\n" +
                "steam_sdl_intervals_over_50_ms=" + intervals.filter(function (v) { return v > 50.001; }).length + "\r\n" +
                "steam_sdl_raf_ticks=" + raf + "\r\n" +
                "steam_sdl_raf_hz=" + (1000 * raf / elapsed).toFixed(2) + "\r\n" +
                "steam_sdl_native_frame_mean_ms=" + average(nativeMs) + "\r\n" +
                "steam_sdl_read_pixels_mean_ms=" + average(readMs) + "\r\n" +
                "steam_sdl_canvas_upload_mean_ms=" + average(uploadMs) + "\r\n";
        }
        return { observe: observe, reset: reset };
    }

    if (typeof module === "object" && module.exports) module.exports = { create: create };
    else window.steamSdlTiming = { create: create };
}());
