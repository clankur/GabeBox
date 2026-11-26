#pragma once
#include <stddef.h>
#include <stdint.h>

/* Row statistics for the opt-in frame probe (clankur/GabeBox#89, a white flash over a game).
   The producer and the presenter sample the same SDL_GPU_PROBE_ROWS full-width rows of the
   raw, untransformed frame (texture row 0 first, before any flip_y) and reduce them here, so
   equal frames give equal statistics and an equal hash on both sides of the exchange.
   No C runtime calls: the SDL producer is built without one. */
#define SDL_GPU_PROBE_ROWS 16
#define SDL_GPU_PROBE_CELLS 32 /* segments per sampled row */
#define SDL_GPU_PROBE_WHITE 235 /* a cell whose mean luma reaches this is near white */

typedef struct SdlGpuProbeStats {
    uint16_t luma;  /* mean Rec. 709 luma over every sampled pixel, 0-255 scaled by 256 */
    uint16_t white; /* cells whose mean luma is at least SDL_GPU_PROBE_WHITE */
    uint16_t cells; /* nonempty cells: SDL_GPU_PROBE_ROWS * SDL_GPU_PROBE_CELLS for width >= 32 */
    uint8_t min, max; /* lowest and highest cell mean luma */
    uint32_t hash;  /* FNV-1a over the sampled 32-bit pixels, in row order */
} SdlGpuProbeStats;

/* Texture row sampled for probe row `row`: the centre of each of ROWS equal bands. */
static inline unsigned SdlGpuProbeRowY(unsigned row, unsigned height) {
    return (unsigned)(((2ull * row + 1) * height) / (2ull * SDL_GPU_PROBE_ROWS));
}

/* Luma of one 8-bit pixel. The weights sum to 256, so white maps to 255. */
static inline unsigned SdlGpuProbeLuma(unsigned r, unsigned g, unsigned b) {
    return (54u * r + 183u * g + 19u * b + 128u) >> 8;
}

/* rows: SDL_GPU_PROBE_ROWS rows of `width` 4-byte pixels, row i at rows + i * pitch.
   bgra: nonzero for B8G8R8A8 data, zero for R8G8B8A8. */
static inline void SdlGpuProbeMeasure(const uint8_t *rows, size_t pitch, unsigned width, int bgra,
                                      SdlGpuProbeStats *out) {
    const unsigned red = bgra ? 2u : 0u, blue = bgra ? 0u : 2u;
    uint64_t total = 0;
    uint32_t hash = 2166136261u;
    unsigned white = 0, cells = 0, low = 255, high = 0;
    for (unsigned row = 0; row < SDL_GPU_PROBE_ROWS; ++row) {
        const uint8_t *line = rows + row * pitch;
        for (unsigned cell = 0; cell < SDL_GPU_PROBE_CELLS; ++cell) {
            const unsigned begin = (unsigned)((uint64_t)cell * width / SDL_GPU_PROBE_CELLS);
            const unsigned end = (unsigned)((uint64_t)(cell + 1) * width / SDL_GPU_PROBE_CELLS);
            if (end <= begin) continue;
            uint64_t sum = 0;
            for (unsigned x = begin; x < end; ++x) {
                const uint8_t *p = line + 4u * x;
                sum += SdlGpuProbeLuma(p[red], p[1], p[blue]);
                const uint32_t word = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
                hash = (hash ^ word) * 16777619u;
            }
            total += sum;
            const unsigned mean = (unsigned)(sum / (end - begin));
            if (mean >= SDL_GPU_PROBE_WHITE) ++white;
            if (mean < low) low = mean;
            if (mean > high) high = mean;
            ++cells;
        }
    }
    const uint64_t pixels = (uint64_t)SDL_GPU_PROBE_ROWS * width;
    out->luma = (uint16_t)(pixels ? total * 256u / pixels : 0);
    out->white = (uint16_t)white;
    out->cells = (uint16_t)cells;
    out->min = (uint8_t)(cells ? low : 0);
    out->max = (uint8_t)high;
    out->hash = hash;
}
