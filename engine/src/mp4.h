// Papyrus - small self-contained MP4 writer: one HEVC video track plus up to 4 AAC audio tracks (no third-party code).
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
    enum { FTYP_LEN = 28, MDAT_HDR_LEN = 16, MAX_TRACKS = 5, AUDIO_RATE = 48000, AUDIO_FRAME = 1024 };

    // Track 0 is video. Tracks 1.. are audio. Every sample is its own chunk, so samples of different tracks can
    // sit in the file in any order (they are stored in the order they were recorded).
    struct Track {
        uint32_t* sizes = nullptr;
        uint64_t* offs = nullptr;      // relative to the first byte of sample data
        uint32_t* sync = nullptr;      // video only: 1-based keyframe sample numbers
        uint64_t  maxSamples = 0, n = 0;
        uint32_t  maxSync = 0, nSync = 0;
        char      name[32] = "";       // shown by players as the track name
        uint32_t  emptyMs = 0;         // nothing plays on this track for this long at the start (lines tracks up with each other)
        uint32_t  skipSamples = 0;     // audio: samples to skip at the start; video: frames decoded but not shown (frame-exact cut)
        uint64_t  playSamples = 0;     // audio: samples to play after the skip (0 = everything that is left)
    };

    uint32_t width = 0, height = 0, fps = 0;
    Track    tr[MAX_TRACKS];
    uint64_t dataBytes = 0;            // running total, for callers that write samples back to back
    uint64_t overflow = 0;
    uint8_t* moov = nullptr;           size_t moovCap = 0;

    bool init(uint64_t videoSamplesMax, uint64_t audioSamplesMax, uint32_t w, uint32_t h, uint32_t framesPerSecond) {
        width = w; height = h; fps = framesPerSecond;
        moovCap = 16384;
        bool ok = true;
        for (int t = 0; t < MAX_TRACKS; t++) {
            Track& k = tr[t];
            k.maxSamples = t == 0 ? videoSamplesMax + fps : audioSamplesMax;
            if (!k.maxSamples) continue;
            k.sizes = (uint32_t*)calloc((size_t)k.maxSamples, sizeof(uint32_t));
            k.offs = (uint64_t*)calloc((size_t)k.maxSamples, sizeof(uint64_t));
            if (t == 0) { k.maxSync = (uint32_t)(k.maxSamples / fps + 2) * 4; k.sync = (uint32_t*)calloc(k.maxSync, sizeof(uint32_t)); }
            ok = ok && k.sizes && k.offs && (t != 0 || k.sync);
            moovCap += (size_t)k.maxSamples * 12 + (size_t)k.maxSync * 4 + 2048;
        }
        moov = (uint8_t*)malloc(moovCap);
        return ok && moov;
    }
    void reset() {
        for (int t = 0; t < MAX_TRACKS; t++) { tr[t].n = 0; tr[t].nSync = 0; tr[t].emptyMs = 0; tr[t].skipSamples = 0; tr[t].playSamples = 0; }
        dataBytes = 0; overflow = 0;
    }
    void setName(int track, const char* name) { strncpy(tr[track].name, name, sizeof tr[track].name - 1); }

    // Record one sample: `size` bytes at `relOffset` from the start of the sample data.
    void add(int track, uint32_t size, bool keyframe, uint64_t relOffset) {
        if (!size || track < 0 || track >= MAX_TRACKS) return;
        Track& k = tr[track];
        if (k.n >= k.maxSamples) { overflow++; return; }
        k.offs[k.n] = relOffset;
        k.sizes[k.n++] = size;
        if (track == 0 && keyframe && k.nSync < k.maxSync) k.sync[k.nSync++] = (uint32_t)k.n;
    }
    // Same, for samples written one after another (streaming mode).
    void addIndexed(uint32_t size, bool keyframe) { add(0, size, keyframe, dataBytes); dataBytes += size; }

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

    uint32_t trackDurationMs(int t) const {
        const Track& k = tr[t];
        if (t == 0) return (uint32_t)((k.n > k.skipSamples ? k.n - k.skipSamples : 0) * 1000 / fps) + k.emptyMs;   // for video, skipSamples counts frames
        const uint64_t samples = k.n * AUDIO_FRAME;
        uint64_t play = samples > k.skipSamples ? samples - k.skipSamples : 0;
        if (k.playSamples && k.playSamples < play) play = k.playSamples;
        return (uint32_t)(play * 1000 / AUDIO_RATE) + k.emptyMs;
    }

    // Builds the index into the internal buffer. dataStart = absolute file offset of the first sample byte.
    // Returns its length, or 0 if there is nothing to write.
    uint32_t buildMoov(const HevcParamSets& ps, uint64_t dataStart) {
        if (!tr[0].n || !ps.complete()) return 0;
        W w{ moov, 0 };
        uint32_t durMovie = 0;
        for (int t = 0; t < MAX_TRACKS; t++) if (tr[t].n && trackDurationMs(t) > durMovie) durMovie = trackDurationMs(t);

        size_t bMoov = w.begin("moov");
        {
            size_t b = w.begin("mvhd");
            w.u32(0); w.u32(0); w.u32(0); w.u32(1000); w.u32(durMovie);
            w.u32(0x00010000); w.u16(0x0100); w.zero(10);
            matrix(w); w.zero(24); w.u32((uint32_t)MAX_TRACKS + 1);
            w.end(b);
        }
        for (int t = 0; t < MAX_TRACKS; t++) {
            const Track& k = tr[t];
            if (!k.n) continue;
            const bool audio = t != 0;
            const uint32_t timescale = audio ? (uint32_t)AUDIO_RATE : fps;
            const uint32_t delta = audio ? (uint32_t)AUDIO_FRAME : 1;
            const uint32_t durTrack = trackDurationMs(t);

            size_t trak = w.begin("trak");
            {
                size_t b = w.begin("tkhd");
                w.u32(0x00000003); w.u32(0); w.u32(0); w.u32((uint32_t)t + 1); w.u32(0); w.u32(durTrack);
                w.zero(8); w.u16(0);
                w.u16(audio ? 1 : 0);                        // audio tracks are alternates of each other: players pick one
                w.u16(audio ? 0x0100 : 0); w.u16(0);
                matrix(w);
                w.u32(audio ? 0 : width << 16); w.u32(audio ? 0 : height << 16);
                w.end(b);
            }
            if (k.emptyMs || k.skipSamples || (audio && k.playSamples)) {
                size_t edts = w.begin("edts");
                size_t elst = w.begin("elst");
                w.u32(0); w.u32(k.emptyMs ? 2 : 1);
                if (k.emptyMs) { w.u32(k.emptyMs); w.u32(0xFFFFFFFF); w.u32(0x00010000); }
                w.u32(durTrack - k.emptyMs); w.u32(k.skipSamples); w.u32(0x00010000);
                w.end(elst); w.end(edts);
            }
            size_t mdia = w.begin("mdia");
            {
                size_t b = w.begin("mdhd");
                w.u32(0); w.u32(0); w.u32(0); w.u32(timescale); w.u32((uint32_t)(k.n * delta)); w.u16(0x55C4); w.u16(0);
                w.end(b);
                b = w.begin("hdlr");
                w.u32(0); w.u32(0); w.bytes(audio ? "soun" : "vide", 4); w.zero(12);
                const char* nm = k.name[0] ? k.name : (audio ? "Audio" : "Papyrus Video");
                w.bytes(nm, strlen(nm) + 1);
                w.end(b);
            }
            size_t minf = w.begin("minf");
            {
                size_t b;
                if (audio) { b = w.begin("smhd"); w.u32(0); w.u32(0); w.end(b); }
                else       { b = w.begin("vmhd"); w.u32(1); w.zero(8); w.end(b); }
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
                if (audio) {
                    size_t mp4a = w.begin("mp4a");
                    w.zero(6); w.u16(1);
                    w.zero(8);
                    w.u16(2); w.u16(16); w.u32(0);           // stereo, 16-bit
                    w.u32((uint32_t)AUDIO_RATE << 16);
                    size_t esds = w.begin("esds");
                    w.u32(0);
                    w.u8(0x03); w.u8(25); w.u16((uint16_t)(t + 1)); w.u8(0);             // ES descriptor
                    w.u8(0x04); w.u8(17); w.u8(0x40); w.u8(0x15); w.u8(0); w.u16(0x1800); // AAC, audio stream, buffer size
                    w.u32(192000); w.u32(160000);                                         // max / average bitrate
                    w.u8(0x05); w.u8(2); w.u8(0x11); w.u8(0x90);                          // AAC-LC, 48 kHz, stereo
                    w.u8(0x06); w.u8(1); w.u8(0x02);
                    w.end(esds);
                    w.end(mp4a);
                } else {
                    size_t hvc1 = w.begin("hvc1");
                    w.zero(6); w.u16(1);
                    w.zero(16);
                    w.u16((uint16_t)width); w.u16((uint16_t)height);
                    w.u32(0x00480000); w.u32(0x00480000); w.u32(0);
                    w.u16(1);
                    w.zero(32);
                    w.u16(0x0018); w.u16(0xFFFF);
                    writeHvcC(w, ps);
                    size_t colr = w.begin("colr");           // BT.709, limited range - matches what the engine encodes
                    w.bytes("nclx", 4); w.u16(1); w.u16(1); w.u16(1); w.u8(0);
                    w.end(colr);
                    w.end(hvc1);
                }
                w.end(stsd);

                size_t b = w.begin("stts");
                w.u32(0); w.u32(1); w.u32((uint32_t)k.n); w.u32(delta);
                w.end(b);
                if (!audio) {
                    b = w.begin("stss");
                    w.u32(0); w.u32(k.nSync);
                    for (uint32_t i = 0; i < k.nSync; i++) w.u32(k.sync[i]);
                    w.end(b);
                }
                b = w.begin("stsc");                         // one sample per chunk
                w.u32(0); w.u32(1); w.u32(1); w.u32(1); w.u32(1);
                w.end(b);
                b = w.begin("stsz");
                w.u32(0); w.u32(0); w.u32((uint32_t)k.n);
                for (uint64_t i = 0; i < k.n; i++) w.u32(k.sizes[i]);
                w.end(b);
                b = w.begin("co64");
                w.u32(0); w.u32((uint32_t)k.n);
                for (uint64_t i = 0; i < k.n; i++) w.u64(dataStart + k.offs[i]);
                w.end(b);
            }
            w.end(stbl); w.end(minf); w.end(mdia); w.end(trak);
        }
        w.end(bMoov);
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
