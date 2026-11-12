// Papyrus - replay ring: one pre-allocated slab of compressed packets plus a small packet index.
// Only encoded video ever goes in here. Oldest GOPs (keyframe + the frames that depend on it) are evicted whole.
// Not thread-safe by itself: the caller holds one lock around every call. Nothing allocates after init().
// Platform-neutral (plain C++) so it is tested off-Windows.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>

struct ReplayRing {
    struct Pkt { uint64_t off; uint32_t size; uint32_t key; int64_t pts; };

    uint8_t* slab = nullptr;   uint64_t cap = 0;
    Pkt*     pkts = nullptr;   uint64_t maxPkts = 0;
    uint64_t pktHead = 0, pktTail = 0;     // monotonic packet counters: valid packets are [pktTail, pktHead)
    uint64_t byteHead = 0, byteTail = 0;   // monotonic byte counters:   valid bytes   are [byteTail, byteHead)
    uint64_t pendingStart = 0;             // start of the packet currently being written
    int64_t  keepFrames = 0;               // evict GOPs that are entirely older than this many frames
    uint64_t evictedGops = 0;

    // slabMem: caller-provided memory of slabBytes (so the platform layer chooses how to allocate it)
    bool init(uint8_t* slabMem, uint64_t slabBytes, uint64_t packetsMax, int64_t framesToKeep) {
        slab = slabMem; cap = slabBytes; maxPkts = packetsMax; keepFrames = framesToKeep;
        pkts = (Pkt*)calloc((size_t)maxPkts, sizeof(Pkt));
        return slab && pkts;
    }
    const Pkt& at(uint64_t i) const { return pkts[i % maxPkts]; }
    uint64_t count() const { return pktHead - pktTail; }
    uint64_t bytes() const { return byteHead - byteTail; }

    // Drop the oldest GOP (the oldest packet and everything up to the next keyframe).
    bool evictGop() {
        if (pktTail == pktHead) return false;
        pktTail++;
        while (pktTail < pktHead && !at(pktTail).key) pktTail++;
        byteTail = pktTail < pktHead ? at(pktTail).off : pendingStart;
        evictedGops++;
        return true;
    }

    void beginPacket() { pendingStart = byteHead; }
    void write(const uint8_t* p, uint32_t n) {
        while (cap - (byteHead - byteTail) < n) {
            if (!evictGop()) return;                 // packet larger than the whole slab: drop the bytes
        }
        const uint64_t o = byteHead % cap;
        const uint64_t first = (n < cap - o) ? n : cap - o;
        memcpy(slab + o, p, (size_t)first);
        if (first < n) memcpy(slab, p + first, (size_t)(n - first));
        byteHead += n;
    }
    void endPacket(bool key, int64_t pts) {
        const uint32_t size = (uint32_t)(byteHead - pendingStart);
        if (!size) return;
        if (byteTail > pendingStart) { byteTail = byteHead; pktTail = pktHead; return; }   // could not fit: start over
        if (pktTail == pktHead && !key) { byteHead = pendingStart; return; }               // never start the buffer mid-GOP
        if (pktHead - pktTail == maxPkts) evictGop();
        Pkt& k = pkts[pktHead % maxPkts];
        k.off = pendingStart; k.size = size; k.key = key ? 1u : 0u; k.pts = pts;
        pktHead++;
        // time-based trim: drop the oldest GOP while the GOP after it is already old enough to cover the window
        for (;;) {
            uint64_t next = pktTail + 1;
            while (next < pktHead && !at(next).key) next++;
            if (next >= pktHead || at(next).pts > pts - keepFrames) break;
            evictGop();
        }
    }

    // First packet of the clip: the latest keyframe at or before (newest - frames + 1), else the oldest packet.
    // Returns false if the buffer is empty.
    bool clipStart(int64_t frames, uint64_t* startPkt) const {
        if (pktTail == pktHead) return false;
        const int64_t want = at(pktHead - 1).pts - frames + 1;
        uint64_t best = pktTail;
        for (uint64_t i = pktHead; i-- > pktTail;) {
            if (at(i).key && at(i).pts <= want) { best = i; break; }
        }
        *startPkt = best;
        return true;
    }

    // Copies [off, off+n) out of the slab. Returns false if any of it has been evicted already.
    bool read(uint64_t off, uint8_t* dst, uint32_t n) const {
        if (off < byteTail || off + n > byteHead) return false;
        const uint64_t o = off % cap;
        const uint64_t first = (n < cap - o) ? n : cap - o;
        memcpy(dst, slab + o, (size_t)first);
        if (first < n) memcpy(dst + first, slab, (size_t)(n - first));
        return true;
    }
};
