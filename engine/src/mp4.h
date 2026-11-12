// Papyrus - small self-contained MP4 writer for one HEVC video track (no third-party code).
//
// Two ways to use it, both without any allocation after init():
//   streaming (manual recording):  ftyp | mdat (frames as they arrive) | moov (index, written at the end)
//   clip (instant replay):         ftyp | moov | mdat   - "faststart": the index is at the front of the file
//
// The encoder produces Annex-B packets (start codes). MP4 wants each NAL unit prefixed with its length and
// the parameter sets (VPS/SPS/PPS) stored once in the header; hevcToLengthPrefixed() does that rewrite.
// Platform-neutral on purpose (plain C++), so it is tested off-Windows.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>

struct HevcParamSets {
    enum { PS_MAX = 1024 };
    uint8_t  vps[PS_MAX], sps[PS_MAX], pps[PS_MAX];
    uint32_t vpsLen = 0, spsLen = 0, ppsLen = 0;
    bool complete() const { return vpsLen && spsLen && ppsLen; }
};

// Finds "00 00 01" at or after `from`. *after = index just past it, *at = index of its first zero.
static inline bool hevcFindStartCode(const uint8_t* p, uint32_t n, uint32_t from, uint32_t* after, uint32_t* at) {
    uint32_t i = from;
    while (i + 2 < n) {
        const uint8_t c = p[i + 2];
        if (c > 1) { i += 3; continue; }
        if (c == 1 && p[i] == 0 && p[i + 1] == 0) { *at = i; *after = i + 3; return true; }
        i++;
    }
    return false;
}

// Rewrites one Annex-B access unit as length-prefixed NAL units, handing the pieces to sink(ctx, data, size).
// Parameter sets are captured into `ps` (first copy only) and left out of the output. Returns bytes emitted.
template <class Sink>
static inline uint32_t hevcToLengthPrefixed(const uint8_t* p, uint32_t n, HevcParamSets& ps, Sink&& sink) {
    uint32_t total = 0, after = 0, at = 0;
    uint32_t nalStart = hevcFindStartCode(p, n, 0, &after, &at) ? after : n;
    while (nalStart < n) {
        uint32_t nalEnd, nextStart;
        if (hevcFindStartCode(p, n, nalStart, &after, &at)) {
            nalEnd = at; nextStart = after;
            while (nalEnd > nalStart && p[nalEnd - 1] == 0) nalEnd--;
        } else { nalEnd = n; nextStart = n; }
        const uint32_t len = nalEnd - nalStart;
        if (len >= 2) {
            const uint32_t type = (p[nalStart] >> 1) & 0x3F;
            if (type == 32)      { if (!ps.vpsLen && len <= HevcParamSets::PS_MAX) { memcpy(ps.vps, p + nalStart, len); ps.vpsLen = len; } }
            else if (type == 33) { if (!ps.spsLen && len <= HevcParamSets::PS_MAX) { memcpy(ps.sps, p + nalStart, len); ps.spsLen = len; } }
            else if (type == 34) { if (!ps.ppsLen && len <= HevcParamSets::PS_MAX) { memcpy(ps.pps, p + nalStart, len); ps.ppsLen = len; } }
            else {
                const uint8_t be[4] = { (uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len };
                sink(be, 4u);
                sink(p + nalStart, len);
                total += 4 + len;
            }
        }
        nalStart = nextStart;
    }
    return total;
}

struct Mp4Mux {
    enum { FTYP_LEN = 28, MDAT_HDR_LEN = 16 };

    uint32_t width = 0, height = 0, fps = 0;

    // index tables (allocated once)
    uint32_t* sizes = nullptr;     uint64_t maxSamples = 0, nSamples = 0;
    uint64_t* chunkOff = nullptr;  uint32_t maxChunks = 0,  nChunks = 0;     // relative to the first byte of sample data
    uint32_t* sync = nullptr;      uint32_t maxSync = 0,    nSync = 0;
    uint32_t  samplesPerChunk = 0;
    uint64_t  dataBytes = 0;       // total sample bytes indexed so far
    uint64_t  overflow = 0;        // samples that did not fit the tables (should stay 0)
    uint8_t*  moov = nullptr;      size_t moovCap = 0;

    bool init(uint64_t samplesMax, uint32_t w, uint32_t h, uint32_t framesPerSecond) {
        width = w; height = h; fps = framesPerSecond;
        samplesPerChunk = fps;
        maxSamples = samplesMax + fps;
        maxChunks = (uint32_t)(maxSamples / samplesPerChunk) + 2;
        maxSync = maxChunks * 4;
        sizes = (uint32_t*)calloc((size_t)maxSamples, sizeof(uint32_t));
        chunkOff = (uint64_t*)calloc(maxChunks, sizeof(uint64_t));
        sync = (uint32_t*)calloc(maxSync, sizeof(uint32_t));
        moovCap = (size_t)maxSamples * 4 + (size_t)maxChunks * 8 + (size_t)maxSync * 4 + 8192;
        moov = (uint8_t*)malloc(moovCap);
        return sizes && chunkOff && sync && moov;
    }
    void reset() { nSamples = 0; nChunks = 0; nSync = 0; dataBytes = 0; overflow = 0; }

    // Record one sample of `size` bytes (already length-prefixed) in the index.
    void addIndexed(uint32_t size, bool keyframe) {
        if (!size) return;
        if (nSamples >= maxSamples) { overflow++; dataBytes += size; return; }
        if (nSamples % samplesPerChunk == 0 && nChunks < maxChunks) chunkOff[nChunks++] = dataBytes;
        sizes[nSamples++] = size;
        if (keyframe && nSync < maxSync) sync[nSync++] = (uint32_t)nSamples;   // 1-based
        dataBytes += size;
    }

    static uint32_t writeFtyp(uint8_t* out) {
        static const uint8_t ftyp[FTYP_LEN] = {
            0, 0, 0, 28, 'f', 't', 'y', 'p', 'm', 'p', '4', '2', 0, 0, 0, 0,
            'm', 'p', '4', '2', 'i', 's', 'o', 'm', 'h', 'v', 'c', '1' };
        memcpy(out, ftyp, FTYP_LEN);
        return FTYP_LEN;
    }
    // mdat box header with a 64-bit size. payloadBytes may be 0 and patched later (size field is at +8).
    static uint32_t writeMdatHeader(uint8_t* out, uint64_t payloadBytes) {
        static const uint8_t h[8] = { 0, 0, 0, 1, 'm', 'd', 'a', 't' };
        memcpy(out, h, 8);
        const uint64_t s = payloadBytes + MDAT_HDR_LEN;
        for (int b = 0; b < 8; b++) out[8 + b] = (uint8_t)(s >> (56 - 8 * b));
        return MDAT_HDR_LEN;
    }

    // Builds the index into the internal buffer. dataStart = absolute file offset of the first sample byte.
    // Returns its length, or 0 if there is nothing to write.
    uint32_t buildMoov(const HevcParamSets& ps, uint64_t dataStart) {
        if (!nSamples || !ps.complete()) return 0;
        W w{ moov, 0 };
        const uint32_t durMovie = (uint32_t)(nSamples * 1000 / fps);
        const uint32_t durMedia = (uint32_t)nSamples;          // media timescale = fps, every frame lasts 1

        size_t bMoov = w.begin("moov");
        {
            size_t b = w.begin("mvhd");
            w.u32(0); w.u32(0); w.u32(0); w.u32(1000); w.u32(durMovie);
            w.u32(0x00010000); w.u16(0x0100); w.zero(10);
            matrix(w); w.zero(24); w.u32(2);
            w.end(b);
        }
        size_t trak = w.begin("trak");
        {
            size_t b = w.begin("tkhd");
            w.u32(0x00000003); w.u32(0); w.u32(0); w.u32(1); w.u32(0); w.u32(durMovie);
            w.zero(8); w.u16(0); w.u16(0); w.u16(0); w.u16(0);
            matrix(w); w.u32(width << 16); w.u32(height << 16);
            w.end(b);
        }
        size_t mdia = w.begin("mdia");
        {
            size_t b = w.begin("mdhd");
            w.u32(0); w.u32(0); w.u32(0); w.u32(fps); w.u32(durMedia); w.u16(0x55C4); w.u16(0);
            w.end(b);
            b = w.begin("hdlr");
            w.u32(0); w.u32(0); w.bytes("vide", 4); w.zero(12); w.bytes("Papyrus Video", 14);
            w.end(b);
        }
        size_t minf = w.begin("minf");
        {
            size_t b = w.begin("vmhd");
            w.u32(1); w.zero(8);
            w.end(b);
            size_t dinf = w.begin("dinf");
            size_t dref = w.begin("dref");
            w.u32(0); w.u32(1);
            size_t url = w.begin("url ");
            w.u32(1);
            w.end(url); w.end(dref); w.end(dinf);
        }
        size_t stbl = w.begin("stbl");
        {
            size_t stsd = w.begin("stsd");
            w.u32(0); w.u32(1);
            size_t hvc1 = w.begin("hvc1");
            w.zero(6); w.u16(1);
            w.zero(16);
            w.u16((uint16_t)width); w.u16((uint16_t)height);
            w.u32(0x00480000); w.u32(0x00480000); w.u32(0);
            w.u16(1);
            w.zero(32);
            w.u16(0x0018); w.u16(0xFFFF);
            writeHvcC(w, ps);
            size_t colr = w.begin("colr");                   // BT.709, limited range - matches what the engine encodes
            w.bytes("nclx", 4); w.u16(1); w.u16(1); w.u16(1); w.u8(0);
            w.end(colr);
            w.end(hvc1);
            w.end(stsd);

            size_t b = w.begin("stts");
            w.u32(0); w.u32(1); w.u32((uint32_t)nSamples); w.u32(1);
            w.end(b);

            b = w.begin("stss");
            w.u32(0); w.u32(nSync);
            for (uint32_t i = 0; i < nSync; i++) w.u32(sync[i]);
            w.end(b);

            b = w.begin("stsc");
            const uint32_t rem = (uint32_t)(nSamples % samplesPerChunk);
            const bool partialLast = rem != 0 && nChunks > 1;
            w.u32(0);
            if (nChunks == 1) { w.u32(1); w.u32(1); w.u32((uint32_t)nSamples); w.u32(1); }
            else {
                w.u32(partialLast ? 2 : 1);
                w.u32(1); w.u32(samplesPerChunk); w.u32(1);
                if (partialLast) { w.u32(nChunks); w.u32(rem); w.u32(1); }
            }
            w.end(b);

            b = w.begin("stsz");
            w.u32(0); w.u32(0); w.u32((uint32_t)nSamples);
            for (uint64_t i = 0; i < nSamples; i++) w.u32(sizes[i]);
            w.end(b);

            b = w.begin("co64");
            w.u32(0); w.u32(nChunks);
            for (uint32_t i = 0; i < nChunks; i++) w.u64(dataStart + chunkOff[i]);
            w.end(b);
        }
        w.end(stbl); w.end(minf); w.end(mdia); w.end(trak); w.end(bMoov);
        return (uint32_t)w.n;
    }

    // Clip layout helper: length of the index, so the caller knows where the sample data will start.
    // (The index length does not depend on the offsets stored in it.)
    uint32_t moovLength(const HevcParamSets& ps) { return buildMoov(ps, 0); }

private:
    struct W {
        uint8_t* p; size_t n;
        void u8(uint8_t v) { p[n++] = v; }
        void u16(uint16_t v) { u8((uint8_t)(v >> 8)); u8((uint8_t)v); }
        void u32(uint32_t v) { u16((uint16_t)(v >> 16)); u16((uint16_t)v); }
        void u64(uint64_t v) { u32((uint32_t)(v >> 32)); u32((uint32_t)v); }
        void zero(size_t c) { memset(p + n, 0, c); n += c; }
        void bytes(const void* d, size_t c) { memcpy(p + n, d, c); n += c; }
        size_t begin(const char* fourcc) { size_t at = n; u32(0); bytes(fourcc, 4); return at; }
        void end(size_t at) {
            const uint32_t s = (uint32_t)(n - at);
            p[at] = (uint8_t)(s >> 24); p[at + 1] = (uint8_t)(s >> 16); p[at + 2] = (uint8_t)(s >> 8); p[at + 3] = (uint8_t)s;
        }
    };
    static void matrix(W& w) {
        w.u32(0x00010000); w.u32(0); w.u32(0);
        w.u32(0); w.u32(0x00010000); w.u32(0);
        w.u32(0); w.u32(0); w.u32(0x40000000);
    }
    static void writeHvcC(W& w, const HevcParamSets& ps) {
        // Profile/tier/level live at the start of the SPS; undo emulation prevention (00 00 03) first.
        uint8_t s[64] = {};
        uint32_t o = 0, zeros = 0;
        for (uint32_t i = 0; i < ps.spsLen && o < sizeof s; i++) {
            if (zeros >= 2 && ps.sps[i] == 3) { zeros = 0; continue; }
            zeros = ps.sps[i] == 0 ? zeros + 1 : 0;
            s[o++] = ps.sps[i];
        }
        const uint8_t layers = (uint8_t)(((s[2] >> 1) & 7) + 1);
        const uint8_t nested = s[2] & 1;
        size_t b = w.begin("hvcC");
        w.u8(1);
        w.u8(s[3]);
        w.bytes(s + 4, 4);
        w.bytes(s + 8, 6);
        w.u8(s[14]);
        w.u16(0xF000);
        w.u8(0xFC);
        w.u8(0xFC | 1);
        w.u8(0xF8);
        w.u8(0xF8);
        w.u16(0);
        w.u8((uint8_t)((layers << 3) | (nested << 2) | 3));
        w.u8(3);
        w.u8(0x80 | 32); w.u16(1); w.u16((uint16_t)ps.vpsLen); w.bytes(ps.vps, ps.vpsLen);
        w.u8(0x80 | 33); w.u16(1); w.u16((uint16_t)ps.spsLen); w.bytes(ps.sps, ps.spsLen);
        w.u8(0x80 | 34); w.u16(1); w.u16((uint16_t)ps.ppsLen); w.bytes(ps.pps, ps.ppsLen);
        w.end(b);
    }
};
