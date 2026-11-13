// Papyrus - clip editing without re-encoding the picture.
//
// Reads a Papyrus MP4, keeps a time range of it, and writes a new MP4:
//   - the video is copied as it is. The cut can start on ANY frame: the file carries the frames from the keyframe
//     before it (keyframes are 1 s apart), and those lead-in frames are given a display time of practically zero, so
//     every player starts on the chosen frame without needing edit-list support;
//   - each sound track can be kept or dropped and has its own start and end, which may reach outside the video's
//     range. Where there is sound but no picture, black frames are inserted (made by `black`, Windows only);
//   - a track whose volume was changed is the only thing that gets re-encoded (through `regain`, Windows only).
// Plain C++ (no Windows calls) so the copy path is tested off-Windows. Editing is a one-off job, so unlike the
// recorder this code is allowed to allocate.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <map>
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
    std::vector<uint32_t> deltas;      // how long each sample lasts, in timescale units
    bool inband = false;               // video: keyframes carry their own parameter sets ("hev1")
    HevcParamSets ps;
};

struct Clip {
    std::vector<InTrack> tracks;       // tracks[0] is video once load() succeeds
    FILE* f = nullptr;
    ~Clip() { if (f) fclose(f); }
};

// What to do with one sound track. Times are milliseconds from the start of the ORIGINAL clip.
// startMs < 0 = where the video starts, endMs < 0 = where the video ends.
struct TrackEdit { bool keep = true; int64_t startMs = -1, endMs = -1; int gainPercent = 100; };

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
        for (size_t i = 0; i + 4 <= len; i++) if (!memcmp(stsd + i, "hev1", 4)) { t.inband = true; break; }
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
    t.deltas.assign(count, t.audio ? (uint32_t)Mp4Mux::AUDIO_FRAME : 1);
    if (const uint8_t* stts = findBox(stbl, sl, "stts", &len)) {
        const uint32_t runs = be32(stts + 4);
        uint32_t at = 0, last = t.deltas.empty() ? 1 : t.deltas[0];
        for (uint32_t r = 0; r < runs && 8 + (size_t)(r + 1) * 8 <= len; r++) {
            const uint32_t c2 = be32(stts + 8 + r * 8);
            last = be32(stts + 12 + r * 8);
            for (uint32_t k = 0; k < c2 && at < count; k++) t.deltas[at++] = last;
        }
        while (at < count) t.deltas[at++] = last;
    }

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

// When each video frame appears, in seconds on the clip's timeline (negative = a hidden lead-in frame).
// Returns the time the last frame stops being shown.
static inline double videoTimes(const InTrack& v, std::vector<double>& times) {
    times.resize(v.sizes.size());
    const double ts = v.timescale ? (double)v.timescale : 1.0;
    uint64_t cum = 0;
    for (size_t i = 0; i < times.size(); i++) {
        times[i] = v.emptyMs / 1000.0 + ((double)cum - (double)v.skipSamples) / ts;
        cum += v.deltas[i];
    }
    return v.emptyMs / 1000.0 + ((double)cum - (double)v.skipSamples) / ts;
}

// The frame rate the clip was recorded at: from the most common frame duration.
static inline uint32_t nominalFps(const InTrack& v) {
    std::map<uint32_t, uint32_t> hist;
    for (uint32_t d : v.deltas) hist[d]++;
    uint32_t best = 1, bestCount = 0;
    for (auto& kv : hist) if (kv.second > bestCount) { best = kv.first; bestCount = kv.second; }
    const uint32_t fps = best ? (uint32_t)((double)v.timescale / best + 0.5) : 0;
    return fps ? fps : 60;
}

// Writes a description of the clip for the Papyrus app: one "key=value" per line.
static inline bool writeInfo(const PathChar* in, const PathChar* out, std::string& err) {
    Clip c;
    if (!load(in, c, err)) return false;
    FILE* o = openFile(out, L"wb");
    if (!o) { err = "could not write the info file"; return false; }
    const InTrack& v = c.tracks[0];
    std::vector<double> times;
    const double seconds = videoTimes(v, times);
    fprintf(o, "width=%u\nheight=%u\nfps=%u\nframes=%u\ndurationMs=%.0f\nkeyframes=%u\n", v.width, v.height, nominalFps(v),
            (unsigned)v.sizes.size(), seconds * 1000.0, (unsigned)v.sync.size());
    for (size_t i = 1; i < c.tracks.size(); i++)
        fprintf(o, "audio=%u|%s|%.0f|%u\n", (unsigned)i, c.tracks[i].name, audioDurationMs(c.tracks[i]), c.tracks[i].emptyMs);
    fclose(o);
    return true;
}

// Decodes one whole sound track to 16-bit stereo PCM (Windows only; see aud::decodeTrack).
typedef bool (*DecodeFn)(const std::vector<std::vector<uint8_t>>& in, std::vector<int16_t>& pcm, uint32_t* missingFrames);

// Writes the loudness outline of every sound track for the editor's waveforms: one line per track,
// "track=<n>|<hex>", where hex is `buckets` bytes (0-255 peak level) spread evenly over the clip's full length.
static inline bool writePeaks(const PathChar* in, const PathChar* out, int buckets, DecodeFn decode, std::string& err) {
    Clip c;
    if (!load(in, c, err)) return false;
    if (!decode) { err = "waveforms are not available here"; return false; }
    FILE* o = openFile(out, L"wb");
    if (!o) { err = "could not write the waveform file"; return false; }
    std::vector<double> frameTimes;
    const double clipSeconds = videoTimes(c.tracks[0], frameTimes);
    for (size_t i = 1; i < c.tracks.size(); i++) {
        const InTrack& a = c.tracks[i];
        std::vector<std::vector<uint8_t>> pk(a.sizes.size());
        bool ok = true;
        for (size_t k = 0; k < pk.size() && ok; k++) {
            pk[k].resize(a.sizes[k]);
            ok = !PAPYRUS_FSEEK(c.f, (long long)a.offs[k], SEEK_SET) && fread(pk[k].data(), 1, pk[k].size(), c.f) == pk[k].size();
        }
        std::vector<int16_t> pcm;
        uint32_t missing = 0;
        std::vector<uint8_t> peak((size_t)buckets, 0);
        if (ok && decode(pk, pcm, &missing)) {
            const size_t frames = pcm.size() / 2;
            for (size_t j = 0; j < frames; j++) {
                // where this sample plays on the clip's timeline
                const double t = a.emptyMs / 1000.0 + ((double)j + missing - a.skipSamples) / (double)Mp4Mux::AUDIO_RATE;
                if (t < 0 || t >= clipSeconds) continue;
                const size_t b = (size_t)(t / clipSeconds * buckets);
                int v = pcm[j * 2] < 0 ? -pcm[j * 2] : pcm[j * 2];
                const int r = pcm[j * 2 + 1] < 0 ? -pcm[j * 2 + 1] : pcm[j * 2 + 1];
                if (r > v) v = r;
                const uint8_t lvl = (uint8_t)(v >= 32767 ? 255 : v * 255 / 32767);
                if (b < peak.size() && lvl > peak[b]) peak[b] = lvl;
            }
        }
        fprintf(o, "track=%u|", (unsigned)i);
        for (uint8_t v : peak) fprintf(o, "%02x", v);
        fprintf(o, "\n");
    }
    fclose(o);
    return true;
}

// Encodes 16-bit stereo 48 kHz PCM to raw AAC frames. *delay = samples of encoder delay in front (Windows only).
typedef bool (*EncodeFn)(const std::vector<int16_t>& pcm, std::vector<std::vector<uint8_t>>& out, uint32_t* delay);

// Optional extras for trim(). Any of them may be null; the edit that needs a missing one fails with a clear message.
struct Tools {
    RegainFn regain = nullptr;      // change one track's volume
    DecodeFn decode = nullptr;      // \ together: mix several tracks into one
    EncodeFn encode = nullptr;      // /
    bool (*black)(uint32_t, uint32_t, uint32_t, int, std::vector<std::vector<uint8_t>>&, HevcParamSets&) = nullptr;   // black frames (see BlackFn)
};

// Makes `frames` black video frames of the given size (first one a keyframe) as length-prefixed HEVC samples
// without parameter sets, and reports the parameter sets they need. Windows only (uses the graphics card's encoder).
typedef bool (*BlackFn)(uint32_t width, uint32_t height, uint32_t fps, int frames, std::vector<std::vector<uint8_t>>& samples, HevcParamSets& ps);

static inline void appendNal(std::vector<uint8_t>& out, const uint8_t* nal, uint32_t len) {
    const uint8_t be[4] = { (uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len };
    out.insert(out.end(), be, be + 4);
    out.insert(out.end(), nal, nal + len);
}
static inline std::vector<uint8_t> paramSetPrefix(const HevcParamSets& ps) {
    std::vector<uint8_t> out;
    appendNal(out, ps.vps, ps.vpsLen); appendNal(out, ps.sps, ps.spsLen); appendNal(out, ps.pps, ps.ppsLen);
    return out;
}
static inline bool sameParamSets(const HevcParamSets& a, const HevcParamSets& b) {
    return a.vpsLen == b.vpsLen && a.spsLen == b.spsLen && a.ppsLen == b.ppsLen &&
           !memcmp(a.vps, b.vps, a.vpsLen) && !memcmp(a.sps, b.sps, a.spsLen) && !memcmp(a.pps, b.pps, a.ppsLen);
}

// startMs/endMs: the part of the clip's picture to keep (endMs < 0 = to the end). edits[i] applies to the i-th sound track.
// On success *actualStartMs is where the new clip's picture starts (the chosen frame).
// mix = true: the kept sound tracks are blended into one track called "Mixed", so every player plays all of them.
static inline bool trim(const PathChar* in, const PathChar* out, int64_t startMs, int64_t endMs, const TrackEdit* edits, int nEdits,
                        bool mix, const Tools& tools, int64_t* actualStartMs, std::string& err) {
    const RegainFn regain = tools.regain;
    const BlackFn black = tools.black;
    Clip c;
    if (!load(in, c, err)) return false;
    const InTrack& v = c.tracks[0];
    const uint32_t fps = nominalFps(v);
    const int64_t nFrames = (int64_t)v.sizes.size();
    std::vector<double> times;
    const double srcEnd = videoTimes(v, times);

    // The frames that will be shown: [fs, fe). The copy starts at the keyframe at or before fs.
    const double S = startMs / 1000.0, E = endMs < 0 ? srcEnd + 1 : endMs / 1000.0;
    int64_t fs = 0;
    while (fs < nFrames && (times[(size_t)fs] < S - 0.0005 || times[(size_t)fs] < -0.0000001)) fs++;
    int64_t fe = fs;
    while (fe < nFrames && times[(size_t)fe] < E - 0.0005) fe++;
    if (fs >= fe) { err = "the selected part is empty"; return false; }
    int64_t key = 0;
    for (uint32_t s : v.sync) if ((int64_t)s - 1 <= fs) key = (int64_t)s - 1;
    const double vStart = times[(size_t)fs];
    const double vEnd = fe < nFrames ? times[(size_t)fe] : srcEnd;
    if (actualStartMs) *actualStartMs = (int64_t)(vStart * 1000.0 + 0.5);

    // What each sound track keeps. A track may start before the picture or end after it.
    struct Want { int src; double t0, t1; TrackEdit e; };
    std::vector<Want> wants;
    const double R = Mp4Mux::AUDIO_RATE, F = Mp4Mux::AUDIO_FRAME;
    double movieStart = vStart, movieEnd = vEnd;
    for (size_t i = 1; i < c.tracks.size(); i++) {
        const TrackEdit e = (int)(i - 1) < nEdits ? edits[i - 1] : TrackEdit{};
        if (!e.keep || e.gainPercent <= 0) continue;
        const InTrack& a = c.tracks[i];
        double t0 = e.startMs < 0 ? vStart : e.startMs / 1000.0;
        double t1 = e.endMs < 0 ? vEnd : e.endMs / 1000.0;
        const double trackStart = a.emptyMs / 1000.0, trackEnd = audioDurationMs(a) / 1000.0;
        if (t0 < trackStart) t0 = trackStart;
        if (t1 > trackEnd) t1 = trackEnd;
        if (t1 - t0 < 0.02) continue;                                            // nothing of this track is left
        if (t0 < movieStart) movieStart = t0;
        if (t1 > movieEnd) movieEnd = t1;
        wants.push_back({ (int)i, t0, t1, e });
    }

    // Sound without picture gets black frames.
    int64_t nLead = (int64_t)((vStart - movieStart) * fps + 0.5), nTail = (int64_t)((movieEnd - vEnd) * fps + 0.5);
    std::vector<std::vector<uint8_t>> blackSamples;
    HevcParamSets blackPs;
    if (nLead > 0 || nTail > 0) {
        if (!black) { err = "black frames for sound-only parts are not available here"; return false; }
        if (!black(v.width, v.height, fps, (int)(nLead > nTail ? nLead : nTail), blackSamples, blackPs) || blackSamples.empty() || !blackPs.complete()) {
            err = "could not make the black frames for the sound-only part";
            return false;
        }
        if (nLead > (int64_t)blackSamples.size()) nLead = (int64_t)blackSamples.size();
        if (nTail > (int64_t)blackSamples.size()) nTail = (int64_t)blackSamples.size();
    }
    const bool haveBlack = nLead > 0 || nTail > 0;
    // If the black frames need different parameter sets from the clip, every keyframe carries its own.
    const bool mixed = haveBlack && !sameParamSets(blackPs, v.ps);
    const bool inband = v.inband || mixed;
    const std::vector<uint8_t> origPrefix = (inband && !v.inband) ? paramSetPrefix(v.ps) : std::vector<uint8_t>();
    const std::vector<uint8_t> blackPrefix = (inband && haveBlack) ? paramSetPrefix(blackPs) : std::vector<uint8_t>();

    struct OutAudio { int src; int64_t first, count; uint32_t emptyMs, skip; uint64_t play; int gain; std::string name; std::vector<std::vector<uint8_t>> re; };
    std::vector<OutAudio> outs;
    const bool mixing = mix && (wants.size() > 1 || (wants.size() == 1 && wants[0].e.gainPercent != 100));
    for (const Want& w : wants) {
        const InTrack& a = c.tracks[(size_t)w.src];
        const double trackStart = a.emptyMs / 1000.0;
        auto pos = [&](double t) { return (int64_t)((t - trackStart) * R + 0.5) + (int64_t)a.skipSamples; };   // sample position in the stored stream
        const int64_t p0 = pos(w.t0), p1 = pos(w.t1);
        int64_t first = p0 / (int64_t)F;
        if (first > 0) first--;                                                  // one packet of run-in so the decoder starts clean
        int64_t last = (p1 - 1) / (int64_t)F;
        if (last >= (int64_t)a.sizes.size()) last = (int64_t)a.sizes.size() - 1;
        if (last < first) continue;
        OutAudio o;
        o.src = w.src; o.first = first; o.count = last - first + 1; o.gain = w.e.gainPercent; o.name = a.name;
        o.skip = (uint32_t)(p0 - first * (int64_t)F);
        o.play = (uint64_t)(p1 - p0);
        o.emptyMs = (uint32_t)((w.t0 - movieStart) * 1000.0 + 0.5);
        if (!mixing && w.e.gainPercent != 100) {
            if (!regain) { err = "changing a track's volume is not available here"; return false; }
            std::vector<std::vector<uint8_t>> src((size_t)o.count);
            for (int64_t k = 0; k < o.count; k++) {
                src[(size_t)k].resize(a.sizes[(size_t)(first + k)]);
                if (PAPYRUS_FSEEK(c.f, (long long)a.offs[(size_t)(first + k)], SEEK_SET) ||
                    fread(src[(size_t)k].data(), 1, src[(size_t)k].size(), c.f) != src[(size_t)k].size()) { err = "could not read the clip"; return false; }
            }
            uint32_t extra = 0;
            if (!regain(src, w.e.gainPercent, o.re, &extra) || o.re.empty()) { err = "could not change the volume of a sound track"; return false; }
            o.skip += extra;
        }
        outs.push_back(std::move(o));
    }

    // Blend the kept tracks into one: decode each, add them up where they belong on the new clip's timeline, encode once.
    if (mixing) {
        if (!tools.decode || !tools.encode) { err = "mixing sound tracks is not available here"; return false; }
        const size_t total = (size_t)((movieEnd - movieStart) * R + 0.5);
        std::vector<int32_t> sum(total * 2, 0);
        for (const OutAudio& o : outs) {
            const InTrack& a = c.tracks[(size_t)o.src];
            std::vector<std::vector<uint8_t>> src((size_t)o.count);
            for (int64_t k = 0; k < o.count; k++) {
                src[(size_t)k].resize(a.sizes[(size_t)(o.first + k)]);
                if (PAPYRUS_FSEEK(c.f, (long long)a.offs[(size_t)(o.first + k)], SEEK_SET) ||
                    fread(src[(size_t)k].data(), 1, src[(size_t)k].size(), c.f) != src[(size_t)k].size()) { err = "could not read the clip"; return false; }
            }
            std::vector<int16_t> pcm;
            uint32_t missing = 0;
            if (!tools.decode(src, pcm, &missing)) { err = "could not decode a sound track for mixing"; return false; }
            const int64_t frames = (int64_t)(pcm.size() / 2);
            const int64_t from = (int64_t)o.skip - (int64_t)missing;             // first wanted frame in the decoded stream
            const int64_t at = (int64_t)(o.emptyMs / 1000.0 * R + 0.5);          // where it starts in the mix
            for (int64_t j = 0; j < (int64_t)o.play; j++) {
                const int64_t d = from + j, m = at + j;
                if (d < 0 || d >= frames || m < 0 || m >= (int64_t)total) continue;
                sum[(size_t)m * 2] += (int32_t)pcm[(size_t)d * 2] * o.gain / 100;
                sum[(size_t)m * 2 + 1] += (int32_t)pcm[(size_t)d * 2 + 1] * o.gain / 100;
            }
        }
        std::vector<int16_t> mixed(sum.size());
        for (size_t i = 0; i < sum.size(); i++) mixed[i] = (int16_t)(sum[i] > 32767 ? 32767 : sum[i] < -32768 ? -32768 : sum[i]);
        OutAudio one;
        one.src = outs.empty() ? 1 : outs[0].src; one.first = 0; one.count = 0; one.emptyMs = 0; one.gain = 100; one.name = "Mixed";
        uint32_t delay = 0;
        if (!tools.encode(mixed, one.re, &delay) || one.re.empty()) { err = "could not encode the mixed sound track"; return false; }
        one.skip = delay;
        one.play = total;
        outs.clear();
        outs.push_back(std::move(one));
    }

    // The video, in order: black lead, hidden lead-in frames (keyframe up to the cut), the shown frames, black tail.
    // Time is counted in 1/(fps*1000) s: a normal frame lasts 1000, a hidden lead-in frame lasts 1.
    const uint32_t outScale = fps * 1000;
    struct VItem { bool isBlack; int64_t idx; uint32_t dur; bool key; };
    std::vector<VItem> video;
    auto blackKey = [&](int64_t i) {                         // does this sample hold a keyframe slice?
        const auto& b = blackSamples[(size_t)i];
        for (size_t o = 0; o + 5 <= b.size();) {
            const uint32_t len = be32(b.data() + o);
            const int t = (b[o + 4] >> 1) & 0x3F;
            if (t >= 16 && t <= 21) return true;
            o += 4 + (size_t)len;
        }
        return false;
    };
    auto origKey = [&](int64_t i) { for (uint32_t s : v.sync) if ((int64_t)s - 1 == i) return true; return false; };
    for (int64_t i = 0; i < nLead; i++) video.push_back({ true, i, 1000, blackKey(i) });
    for (int64_t f = key; f < fs; f++) video.push_back({ false, f, 1, origKey(f) });
    for (int64_t f = fs; f < fe; f++) {
        double d = (double)v.deltas[(size_t)f] / v.timescale * outScale + 0.5;
        video.push_back({ false, f, d < 1 ? 1u : (uint32_t)d, origKey(f) });
    }
    for (int64_t i = 0; i < nTail; i++) video.push_back({ true, i, 1000, blackKey(i) });

    Mp4Mux m;
    uint64_t maxAudio = 1;
    for (auto& o : outs) { const uint64_t n = o.re.empty() ? (uint64_t)o.count : o.re.size(); if (n > maxAudio) maxAudio = n; }
    if (!m.init((uint64_t)video.size(), maxAudio, v.width, v.height, fps)) { err = "out of memory"; return false; }
    m.videoTimescale = outScale;
    m.inbandParamSets = inband;

    // Lay samples out in time order so the file plays smoothly from any disk.
    struct Item { int track; int64_t idx; };                 // track 0: idx into `video`; else idx into that sound track
    std::vector<Item> order;
    std::vector<int64_t> cur(outs.size(), 0);
    auto audioTime = [&](size_t a) { return outs[a].emptyMs / 1000.0 + ((double)cur[a] * F - outs[a].skip) / R; };
    uint64_t ticks = 0;
    for (size_t i = 0; i < video.size(); i++) {
        const double tv = (double)ticks / outScale;
        for (size_t a = 0; a < outs.size(); a++) {
            const int64_t n = outs[a].re.empty() ? outs[a].count : (int64_t)outs[a].re.size();
            while (cur[a] < n && audioTime(a) <= tv) order.push_back({ (int)a + 1, cur[a]++ });
        }
        order.push_back({ 0, (int64_t)i });
        ticks += video[i].dur;
    }
    for (size_t a = 0; a < outs.size(); a++) {
        const int64_t n = outs[a].re.empty() ? outs[a].count : (int64_t)outs[a].re.size();
        while (cur[a] < n) order.push_back({ (int)a + 1, cur[a]++ });
    }
    auto prefixOf = [&](const VItem& it) -> const std::vector<uint8_t>* {
        static const std::vector<uint8_t> none;
        if (!it.key) return &none;
        return it.isBlack ? &blackPrefix : &origPrefix;
    };
    auto sizeOf = [&](const Item& it) -> uint32_t {
        if (it.track == 0) {
            const VItem& vi = video[(size_t)it.idx];
            return (uint32_t)prefixOf(vi)->size() + (vi.isBlack ? (uint32_t)blackSamples[(size_t)vi.idx].size() : v.sizes[(size_t)vi.idx]);
        }
        const OutAudio& o = outs[(size_t)it.track - 1];
        return o.re.empty() ? c.tracks[(size_t)o.src].sizes[(size_t)(o.first + it.idx)] : (uint32_t)o.re[(size_t)it.idx].size();
    };
    uint64_t rel = 0;
    for (const Item& it : order) {
        const uint32_t sz = sizeOf(it);
        if (it.track == 0) m.add(0, sz, video[(size_t)it.idx].key, rel, video[(size_t)it.idx].dur);
        else m.add(it.track, sz, false, rel);
        rel += sz;
    }
    for (size_t a = 0; a < outs.size(); a++) {
        m.setName((int)a + 1, outs[a].name.c_str());
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
    auto copyFromSource = [&](uint64_t off, uint32_t sz) {
        if (sz > buf.size()) buf.resize(sz);
        return !PAPYRUS_FSEEK(c.f, (long long)off, SEEK_SET) && fread(buf.data(), 1, sz, c.f) == sz && fwrite(buf.data(), 1, sz, o) == sz;
    };
    for (size_t i = 0; ok && i < order.size(); i++) {
        const Item& it = order[i];
        if (it.track == 0) {
            const VItem& vi = video[(size_t)it.idx];
            const std::vector<uint8_t>* pre = prefixOf(vi);
            if (!pre->empty()) ok = fwrite(pre->data(), 1, pre->size(), o) == pre->size();
            if (!ok) break;
            if (vi.isBlack) { const auto& b = blackSamples[(size_t)vi.idx]; ok = fwrite(b.data(), 1, b.size(), o) == b.size(); }
            else ok = copyFromSource(v.offs[(size_t)vi.idx], v.sizes[(size_t)vi.idx]);
            continue;
        }
        const OutAudio& oa = outs[(size_t)it.track - 1];
        if (!oa.re.empty()) { const auto& b = oa.re[(size_t)it.idx]; ok = fwrite(b.data(), 1, b.size(), o) == b.size(); }
        else {
            const InTrack& a = c.tracks[(size_t)oa.src];
            ok = copyFromSource(a.offs[(size_t)(oa.first + it.idx)], a.sizes[(size_t)(oa.first + it.idx)]);
        }
    }
    ok = fclose(o) == 0 && ok;
    if (!ok) { err = "could not write the new clip (disk full?)"; return false; }
    return true;
}

} // namespace edit
