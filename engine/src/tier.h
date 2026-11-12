// Papyrus - quality ladder shared by the recorder and the control window (docs/SPEC.md section 8, extended
// down to 480p and 30 fps). Plain C++.
#pragma once
#include <cstdint>

// Target / maximum bitrate in Mbps for an encode size and frame rate.
static inline void papyrusTier(uint64_t pixels, int fps, int* avgMbps, int* maxMbps) {
    static const int table[5][3][2] = {
        //   120 fps      60 fps      30 fps
        { {100, 150}, { 60,  90}, { 40,  60} },   // above 1440p (4K)
        { { 50,  75}, { 35,  50}, { 22,  33} },   // 1440p
        { { 30,  45}, { 20,  30}, { 12,  18} },   // 1080p
        { { 16,  24}, { 10,  15}, {  6,   9} },   // 720p
        { {  8,  12}, {  5,   8}, {  3,   5} },   // 480p and below
    };
    const int row = pixels > 2560ull * 1440 * 11 / 10 ? 0 : pixels > 1920ull * 1080 * 11 / 10 ? 1
                  : pixels > 1280ull * 720 * 11 / 10 ? 2 : pixels > 854ull * 480 * 11 / 10 ? 3 : 4;
    const int col = fps > 60 ? 0 : fps > 30 ? 1 : 2;
    *avgMbps = table[row][col][0];
    *maxMbps = table[row][col][1];
}

// Encode size for a source and a wanted height. Never upscales; keeps the aspect ratio; even dimensions.
static inline void papyrusEncodeSize(uint32_t srcW, uint32_t srcH, uint32_t wantH, uint32_t* w, uint32_t* h) {
    if (!wantH || wantH >= srcH || !srcH) { *w = srcW & ~1u; *h = srcH & ~1u; return; }
    *h = wantH & ~1u;
    *w = (uint32_t)(((uint64_t)srcW * wantH + srcH / 2) / srcH) & ~1u;
    if (*w < 2) *w = 2;
}

// Bytes of RAM the replay buffer slab takes: (clip + 2 s spare) at the target bitrate plus 15 % headroom.
static inline uint64_t papyrusSlabBytes(int avgMbps, int clipSeconds) {
    return ((uint64_t)avgMbps * 1000000ull / 8 + 80000) * (uint64_t)(clipSeconds + 2) * 115 / 100;   // + up to 4 audio tracks at 160 kbps
}
