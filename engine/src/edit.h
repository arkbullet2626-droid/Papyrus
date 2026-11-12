// Papyrus - clip editing without re-encoding the picture.
//
// Reads a Papyrus MP4, keeps a time range of it, and writes a new MP4:
//   - the video is copied as it is, starting at the keyframe at or before the chosen start (keyframes are 1 s apart)
//     and ending at any frame;
//   - each sound track can be kept or dropped, given its own start and end inside the clip, and copied as it is;
//   - a track whose volume was changed is the only thing that gets re-encoded (through `regain`, Windows only).
// Plain C++ (no Windows calls) so the copy path is tested off-Windows. Editing is a one-off job, so unlike the
// recorder this code is allowed to allocate.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "mp4.h"

namespace edit {

#ifdef _WIN32
typedef wchar_t PathChar;
static inline FILE* openFile(const PathChar* p, const wchar_t* mode) { return _wfopen(p, mode); }
#define PAPYRUS_FSEEK _fseeki64
#else
typedef char PathChar;
static inline FILE* openFile(const PathChar* p, const wchar_t* mode) { return fopen(p, mode[0] == L'r' ? "rb" : "wb"); }
#define PAPYRUS_FSEEK fseeko
#endif

struct InTrack {
    bool audio = false;
    char name[32] = "";
    uint32_t timescale = 0, width = 0, height = 0;
    uint32_t emptyMs = 0, skipSamples = 0;
    std::vector<uint32_t> sizes;
    std::vector<uint64_t> offs;
    std::vector<uint32_t> sync;        // 1-based keyframe sample numbers (video)
    HevcParamSets ps;
};

struct Clip {
    std::vector<InTrack> tracks;       // tracks[0] is video once load() succeeds
    FILE* f = nullptr;
    ~Clip() { if (f) fclose(f); }
};

// What to do with one sound track. Times are milliseconds from the start of the ORIGINAL clip; endMs < 0 = to the end.
struct TrackEdit { bool keep = true; int64_t startMs = 0, endMs = -1; int gainPercent = 100; };

// Re-encodes AAC packets at a new volume. in/out are raw AAC frames. Returns false if it cannot.
// *extraDelay receives the number of samples the re-encode adds in front (encoder delay).
typedef bool (*RegainFn)(const std::vector<std::vector<uint8_t>>& in, int gainPercent, std::vector<std::vector<uint8_t>>& out, uint32_t* extraDelay);

static inline uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static inline uint64_t be64(const uint8_t* p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }

// Finds a child box of `type` inside [p, p+n). Returns its payload and payload length, or null.
static inline const uint8_t* findBox(const uint8_t* p, size_t n, const char* type, size_t* len, size_t from = 0, size_t* next = nullptr) {
    size_t i = from;
    while (i + 8 <= n) {
        uint64_t size = be32(p + i);
        size_t hdr = 8;
        if (size == 1 && i + 16 <= n) { size = be64(p + i + 8); hdr = 16; }
        if (size < hdr || i + size > n) return nullptr;
        if (!memcmp(p + i + 4, type, 4)) { *len = (size_t)size - hdr; if (next) *next = i + (size_t)size; return p + i + hdr; }
        i += (size_t)size;
    }
    return nullptr;
}

static inline bool parseTrack(const uint8_t* trak, size_t n, InTrack& t) {
    size_t len, l2;
    const uint8_t* tkhd = findBox(trak, n, "tkhd", &len);
    if (tkhd && len >= 84) { t.width = be32(tkhd + 76) >> 16; t.height = be32(tkhd + 80) >> 16; }
    if (const uint8_t* edts = findBox(trak, n, "edts", &len)) {
        if (const uint8_t* elst = findBox(edts, len, "elst", &l2)) {
            if (l2 >= 8 && elst[0] == 0) {
                const uint32_t count = be32(elst + 4);
                for (uint32_t i = 0; i < count && 8 + (size_t)(i + 1) * 12 <= l2; i++) {
                    const uint32_t dur = be32(elst + 8 + i * 12), mt = be32(elst + 12 + i * 12);
                    if (mt == 0xFFFFFFFF) t.emptyMs += dur; else { t.skipSamples = mt; break; }
                }
            }
        }
    }
    const uint8_t* mdia = findBox(trak, n, "mdia", &len);
    if (!mdia) return false;
    const size_t mdiaLen = len;
    const uint8_t* mdhd = findBox(mdia, mdiaLen, "mdhd", &len);
    if (!mdhd || len < 20 || mdhd[0] != 0) return false;
    t.timescale = be32(mdhd + 12);
    const uint8_t* hdlr = findBox(mdia, mdiaLen, "hdlr", &len);
    if (!hdlr || len < 24) return false;
    t.audio = !memcmp(hdlr + 8, "soun", 4);
    if (!t.audio && memcmp(hdlr + 8, "vide", 4)) return false;
    if (len > 24) { const size_t m = len - 24 < sizeof t.name - 1 ? len - 24 : sizeof t.name - 1; memcpy(t.name, hdlr + 24, m); t.name[m] = 0; }
    const uint8_t* minf = findBox(mdia, mdiaLen, "minf", &len);
    if (!minf) return false;
    const uint8_t* stbl = findBox(minf, len, "stbl", &len);
    if (!stbl) return false;
    const size_t sl = len;

    if (!t.audio) {
        const uint8_t* stsd = findBox(stbl, sl, "stsd", &len);
        if (!stsd) return false;
        const uint8_t* h = nullptr;
        for (size_t i = 0; i + 4 <= len; i++) if (!memcmp(stsd + i, "hvcC", 4)) { h = stsd + i + 4; len -= i + 4; break; }
        if (!h || len < 23) return false;
        size_t o = 23;
        for (int a = 0; a < h[22] && o + 3 <= len; a++) {
            const int type = h[o] & 0x3F;
            const int count = (h[o + 1] << 8) | h[o + 2];
            o += 3;
            for (int k = 0; k < count && o + 2 <= len; k++) {
                const uint32_t nl = ((uint32_t)h[o] << 8) | h[o + 1];
                o += 2;
                if (o + nl > len || nl > HevcParamSets::PS_MAX) return false;
                if (type == 32 && !t.ps.vpsLen) { memcpy(t.ps.vps, h + o, nl); t.ps.vpsLen = nl; }
                if (type == 33 && !t.ps.spsLen) { memcpy(t.ps.sps, h + o, nl); t.ps.spsLen = nl; }
                if (type == 34 && !t.ps.ppsLen) { memcpy(t.ps.pps, h + o, nl); t.ps.ppsLen = nl; }
                o += nl;
            }
        }
        if (!t.ps.complete()) return false;
        if (const uint8_t* stss = findBox(stbl, sl, "stss", &len)) {
            const uint32_t c = be32(stss + 4);
            for (uint32_t i = 0; i < c && 8 + (size_t)(i + 1) * 4 <= len; i++) t.sync.push_back(be32(stss + 8 + i * 4));
        }
    }
    const uint8_t* stsz = findBox(stbl, sl, "stsz", &len);
    if (!stsz || len < 12) return false;
    const uint32_t fixed = be32(stsz + 4), count = be32(stsz + 8);
    if (!fixed && 12 + (size_t)count * 4 > len) return false;
    t.sizes.resize(count);
    for (uint32_t i = 0; i < count; i++) t.sizes[i] = fixed ? fixed : be32(stsz + 12 + i * 4);

    // chunk offsets + sample-to-chunk map -> one offset per sample
    std::vector<uint64_t> chunks;
    if (const uint8_t* co = findBox(stbl, sl, "co64", &len)) { const uint32_t c = be32(co + 4); for (uint32_t i = 0; i < c && 8 + (size_t)(i + 1) * 8 <= len; i++) chunks.push_back(be64(co + 8 + i * 8)); }
    else if (const uint8_t* co32 = findBox(stbl, sl, "stco", &len)) { const uint32_t c = be32(co32 + 4); for (uint32_t i = 0; i < c && 8 + (size_t)(i + 1) * 4 <= len; i++) chunks.push_back(be32(co32 + 8 + i * 4)); }
    const uint8_t* stsc = findBox(stbl, sl, "stsc", &len);
    if (!stsc || chunks.empty()) return false;
    const uint32_t ne = be32(stsc + 4);
    t.offs.resize(count);
    uint32_t sample = 0;
    for (uint32_t e = 0; e < ne && 8 + (size_t)(e + 1) * 12 <= len && sample < count; e++) {
        const uint32_t first = be32(stsc + 8 + e * 12), per = be32(stsc + 12 + e * 12);
        const uint32_t nextFirst = (e + 1 < ne) ? be32(stsc + 8 + (e + 1) * 12) : (uint32_t)chunks.size() + 1;
        for (uint32_t c = first; c < nextFirst && c <= chunks.size() && sample < count; c++) {
            uint64_t o = chunks[c - 1];
            for (uint32_t k = 0; k < per && sample < count; k++) { t.offs[sample] = o; o += t.sizes[sample]; sample++; }
        }
    }
    return sample == count && count > 0;
}

static inline bool load(const PathChar* path, Clip& c, std::string& err) {
    c.f = openFile(path, L"rb");
    if (!c.f) { err = "could not open the clip"; return false; }
    std::vector<uint8_t> moov;
    uint64_t pos = 0;
    for (;;) {
        uint8_t h[16];
        if (PAPYRUS_FSEEK(c.f, (long long)pos, SEEK_SET) || fread(h, 1, 8, c.f) != 8) break;
        uint64_t size = be32(h);
        size_t hdr = 8;
        if (size == 1) { if (fread(h + 8, 1, 8, c.f) != 8) break; size = be64(h + 8); hdr = 16; }
        if (size < hdr) break;
        if (!memcmp(h + 4, "moov", 4)) {
            if (size - hdr > (512u << 20)) break;
            moov.resize((size_t)(size - hdr));
            if (fread(moov.data(), 1, moov.size(), c.f) != moov.size()) moov.clear();
            break;
        }
        pos += size;
    }
    if (moov.empty()) { err = "this file has no index (not a finished MP4)"; return false; }
    size_t from = 0, len, next;
    InTrack video;
    bool haveVideo = false;
    std::vector<InTrack> audio;
    while (const uint8_t* trak = findBox(moov.data(), moov.size(), "trak", &len, from, &next)) {
        InTrack t;
        if (parseTrack(trak, len, t)) {
            if (!t.audio && !haveVideo) { video = t; haveVideo = true; }
            else if (t.audio && audio.size() < Mp4Mux::MAX_TRACKS - 1) audio.push_back(t);
        }
        from = next;
    }
    if (!haveVideo || !video.timescale) { err = "no HEVC video track found (only clips made by Papyrus can be edited)"; return false; }
    c.tracks.push_back(video);
    for (auto& a : audio) c.tracks.push_back(a);
    return true;
}

static inline double audioDurationMs(const InTrack& t) {
    const double samples = (double)t.sizes.size() * (double)Mp4Mux::AUDIO_FRAME - t.skipSamples;
    return t.emptyMs + (samples > 0 ? samples : 0) * 1000.0 / (double)Mp4Mux::AUDIO_RATE;
}

// Writes a description of the clip for the Studio app: one "key=value" per line.
static inline bool writeInfo(const PathChar* in, const PathChar* out, std::string& err) {
    Clip c;
    if (!load(in, c, err)) return false;
    FILE* o = openFile(out, L"wb");
    if (!o) { err = "could not write the info file"; return false; }
    const InTrack& v = c.tracks[0];
    fprintf(o, "width=%u\nheight=%u\nfps=%u\nframes=%u\ndurationMs=%.0f\nkeyframes=%u\n", v.width, v.height, v.timescale,
            (unsigned)v.sizes.size(), (double)v.sizes.size() * 1000.0 / v.timescale, (unsigned)v.sync.size());
    for (size_t i = 1; i < c.tracks.size(); i++)
        fprintf(o, "audio=%u|%s|%.0f|%u\n", (unsigned)i, c.tracks[i].name, audioDurationMs(c.tracks[i]), c.tracks[i].emptyMs);
    fclose(o);
    return true;
}

// startMs/endMs: the part of the clip to keep (endMs < 0 = to the end). edits[i] applies to the i-th sound track.
// On success *actualStartMs is where the new clip really starts (the keyframe at or before startMs).
static inline bool trim(const PathChar* in, const PathChar* out, int64_t startMs, int64_t endMs, const TrackEdit* edits, int nEdits,
                        RegainFn regain, int64_t* actualStartMs, std::string& err) {
    Clip c;
    if (!load(in, c, err)) return false;
    const InTrack& v = c.tracks[0];
    const uint32_t fps = v.timescale;
    const int64_t nFrames = (int64_t)v.sizes.size();

    int64_t fs = (startMs * fps + 500) / 1000;
    if (fs < 0) fs = 0;
    int64_t fe = endMs < 0 ? nFrames : (endMs * fps + 500) / 1000;
    if (fe > nFrames) fe = nFrames;
    int64_t key = 0;                                            // 0-based index of the keyframe we start on
    for (uint32_t s : v.sync) if ((int64_t)s - 1 <= fs) key = (int64_t)s - 1;
    if (fe - key < 1) { err = "the selected part is empty"; return false; }
    const double clipStart = (double)key / fps, clipEnd = (double)fe / fps;      // seconds on the original clip
    if (actualStartMs) *actualStartMs = (int64_t)(clipStart * 1000.0 + 0.5);

    struct OutAudio { int src; int64_t first, count; uint32_t emptyMs, skip; uint64_t play; std::vector<std::vector<uint8_t>> re; };
    std::vector<OutAudio> outs;
    const double R = Mp4Mux::AUDIO_RATE, F = Mp4Mux::AUDIO_FRAME;
    for (size_t i = 1; i < c.tracks.size(); i++) {
        const TrackEdit e = (int)(i - 1) < nEdits ? edits[i - 1] : TrackEdit{};
        if (!e.keep || e.gainPercent <= 0) continue;
        const InTrack& a = c.tracks[i];
        double t0 = clipStart, t1 = clipEnd;
        if (e.startMs / 1000.0 > t0) t0 = e.startMs / 1000.0;
        if (e.endMs >= 0 && e.endMs / 1000.0 < t1) t1 = e.endMs / 1000.0;
        const double trackStart = a.emptyMs / 1000.0;                       // first audible moment of this track
        const double trackEnd = audioDurationMs(a) / 1000.0;
        if (t0 < trackStart) t0 = trackStart;
        if (t1 > trackEnd) t1 = trackEnd;
        if (t1 - t0 < 0.02) continue;                                       // nothing of this track is left
        const int64_t p0 = (int64_t)((t0 - trackStart) * R + 0.5) + a.skipSamples;   // sample positions in the stored stream
        const int64_t p1 = (int64_t)((t1 - trackStart) * R + 0.5) + a.skipSamples;
        int64_t first = p0 / (int64_t)F;
        if (first > 0) first--;                                             // one packet of run-in so the decoder starts clean
        int64_t last = (p1 - 1) / (int64_t)F;
        if (last >= (int64_t)a.sizes.size()) last = (int64_t)a.sizes.size() - 1;
        if (last < first) continue;
        OutAudio o;
        o.src = (int)i; o.first = first; o.count = last - first + 1;
        o.skip = (uint32_t)(p0 - first * (int64_t)F);
        o.play = (uint64_t)(p1 - p0);
        o.emptyMs = (uint32_t)((t0 - clipStart) * 1000.0 + 0.5);
        if (e.gainPercent != 100) {
            if (!regain) { err = "changing a track's volume is not available here"; return false; }
            std::vector<std::vector<uint8_t>> src((size_t)o.count);
            for (int64_t k = 0; k < o.count; k++) {
                src[(size_t)k].resize(a.sizes[(size_t)(first + k)]);
                if (PAPYRUS_FSEEK(c.f, (long long)a.offs[(size_t)(first + k)], SEEK_SET) ||
                    fread(src[(size_t)k].data(), 1, src[(size_t)k].size(), c.f) != src[(size_t)k].size()) { err = "could not read the clip"; return false; }
            }
            uint32_t extra = 0;
            if (!regain(src, e.gainPercent, o.re, &extra) || o.re.empty()) { err = "could not change the volume of a sound track"; return false; }
            o.skip += extra;
        }
        outs.push_back(std::move(o));
    }

    // Build the index. Samples are laid out in time order so the file plays smoothly from any disk.
    Mp4Mux m;
    uint64_t maxAudio = 1;
    for (auto& o : outs) { const uint64_t n = o.re.empty() ? (uint64_t)o.count : o.re.size(); if (n > maxAudio) maxAudio = n; }
    if (!m.init((uint64_t)(fe - key), maxAudio, v.width, v.height, fps)) { err = "out of memory"; return false; }
    struct Item { int track; int64_t idx; };
    std::vector<Item> order;
    std::vector<int64_t> cur(outs.size(), 0);
    for (int64_t f = key; f < fe; f++) {
        const double tv = (double)(f - key) / fps;
        for (size_t a = 0; a < outs.size(); a++) {
            const int64_t n = outs[a].re.empty() ? outs[a].count : (int64_t)outs[a].re.size();
            while (cur[a] < n && outs[a].emptyMs / 1000.0 + ((double)cur[a] * F - outs[a].skip) / R <= tv) order.push_back({ (int)a + 1, cur[a]++ });
        }
        order.push_back({ 0, f });
    }
    for (size_t a = 0; a < outs.size(); a++) {
        const int64_t n = outs[a].re.empty() ? outs[a].count : (int64_t)outs[a].re.size();
        while (cur[a] < n) order.push_back({ (int)a + 1, cur[a]++ });
    }
    uint64_t rel = 0;
    auto sizeOf = [&](const Item& it) -> uint32_t {
        if (it.track == 0) return v.sizes[(size_t)it.idx];
        const OutAudio& o = outs[(size_t)it.track - 1];
        return o.re.empty() ? c.tracks[(size_t)o.src].sizes[(size_t)(o.first + it.idx)] : (uint32_t)o.re[(size_t)it.idx].size();
    };
    for (const Item& it : order) {
        bool isKey = false;
        if (it.track == 0) for (uint32_t s : v.sync) if ((int64_t)s - 1 == it.idx) { isKey = true; break; }
        const uint32_t sz = sizeOf(it);
        m.add(it.track, sz, isKey, rel);
        rel += sz;
    }
    for (size_t a = 0; a < outs.size(); a++) {
        m.setName((int)a + 1, c.tracks[(size_t)outs[a].src].name);
        m.tr[a + 1].emptyMs = outs[a].emptyMs;
        m.tr[a + 1].skipSamples = outs[a].skip;
        m.tr[a + 1].playSamples = outs[a].play;
    }
    uint32_t moovLen = m.moovLength(v.ps);
    if (!moovLen) { err = "could not build the index"; return false; }
    const uint64_t dataStart = (uint64_t)Mp4Mux::FTYP_LEN + moovLen + Mp4Mux::MDAT_HDR_LEN;
    moovLen = m.buildMoov(v.ps, dataStart);

    FILE* o = openFile(out, L"wb");
    if (!o) { err = "could not create the new clip"; return false; }
    uint8_t hdr[64];
    bool ok = fwrite(hdr, 1, Mp4Mux::writeFtyp(hdr), o) == Mp4Mux::FTYP_LEN;
    ok = ok && fwrite(m.moov, 1, moovLen, o) == moovLen;
    ok = ok && fwrite(hdr, 1, Mp4Mux::writeMdatHeader(hdr, rel), o) == Mp4Mux::MDAT_HDR_LEN;
    std::vector<uint8_t> buf(1 << 20);
    for (size_t i = 0; ok && i < order.size(); i++) {
        const Item& it = order[i];
        const uint32_t sz = sizeOf(it);
        if (it.track != 0 && !outs[(size_t)it.track - 1].re.empty()) {
            ok = fwrite(outs[(size_t)it.track - 1].re[(size_t)it.idx].data(), 1, sz, o) == sz;
            continue;
        }
        const uint64_t off = it.track == 0 ? v.offs[(size_t)it.idx]
                                           : c.tracks[(size_t)outs[(size_t)it.track - 1].src].offs[(size_t)(outs[(size_t)it.track - 1].first + it.idx)];
        if (sz > buf.size()) buf.resize(sz);
        ok = !PAPYRUS_FSEEK(c.f, (long long)off, SEEK_SET) && fread(buf.data(), 1, sz, c.f) == sz && fwrite(buf.data(), 1, sz, o) == sz;
    }
    ok = fclose(o) == 0 && ok;
    if (!ok) { err = "could not write the new clip (disk full?)"; return false; }
    return true;
}

} // namespace edit
