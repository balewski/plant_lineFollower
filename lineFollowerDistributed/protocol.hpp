#pragma once
#include "common.hpp"
namespace lf {
enum Kind : uint8_t { HELLO = 1, WELCOME, ARM, COMMAND, SENSOR, STATUS, STOP, ACK, SYNC_REQUEST, SYNC_REPLY, CAL_REQUEST, CAL_REPLY, JOB_REQUEST, JOB_READY, JOB_FAILED };
enum State : uint8_t { IDLE, READY, ACTIVE, STOPPED, FAULT };
enum Reason : uint8_t { NONE, REQUESTED, EXPIRED, DURATION, SERIAL, SHUTDOWN };
enum Code : uint8_t { OK, REJECT, BUSY };
inline constexpr std::array<size_t, 16> payloads{0, 44, 44, 8, 12, 58, 51, 0, 10, 8, 32, 32, 32, 0, 56, 8};
struct Frame {
    std::array<uint8_t, 128> bytes{};
    size_t size = 24;
    Kind kind = STOP;
    U64 session = 0, sequence = 0;
    void put(size_t pos, U64 v, size_t n) {
        for (size_t i = 0; i < n; i++)
            bytes[pos + n - 1 - i] = uint8_t(v >> (8 * i));
    }
    U64 get(size_t pos, size_t n) const {
        U64 v = 0;
        for (size_t i = 0; i < n; i++)
            v = (v << 8) | bytes[pos + i];
        return v;
    }
    int signed16(size_t pos) const {
        auto n = get(pos, 2);
        return n >= 32768 ? int(n) - 65536 : int(n);
    }
    Frame() = default;
    Frame(Kind k, U64 sid, U64 seq) : size(24 + payloads[k]), kind(k), session(sid), sequence(seq) {
        require(sid && seq, "Zero protocol identity");
        std::memcpy(bytes.data(), "LF02", 4);
        bytes[4] = 2;
        bytes[5] = k;
        put(6, payloads[k], 2);
        put(8, sid, 8);
        put(16, seq, 8);
    }
    bool parse(const uint8_t *p, size_t n) {
        if (n < 24 || n > 128 || std::memcmp(p, "LF02", 4) || p[4] != 2 || p[5] < 1 || p[5] > 15)
            return false;
        std::copy(p, p + n, bytes.begin());
        kind = Kind(p[5]);
        size = n;
        session = get(8, 8);
        sequence = get(16, 8);
        return session && sequence && get(6, 2) == payloads[kind] && n == 24 + payloads[kind];
    }
};
// Diagnostic clock probes never arm, refresh command lifetime, or change clocks.
// At most 16 outstanding probes; validate echoed ID/time and reject replay/stale replies.
struct SyncHistory {
    struct Entry { U64 id = 0, sent = 0; };
    std::array<Entry, 16> entries{};
    void remember(U64 id, U64 sent) { entries[id % entries.size()] = {id, sent}; }
    bool consume(U64 id, U64 echoed, U64 pi_rx, U64 pi_tx, U64 received) {
        auto &e = entries[id % entries.size()];
        if (!id || e.id != id || !echoed || e.sent != echoed || received < echoed ||
            received - echoed > 2000000000ULL || !pi_rx || pi_tx < pi_rx ||
            pi_tx - pi_rx > received - echoed + 10000)
            return false;
        e = {};
        return true;
    }
};
inline int polarity_index(const std::string &p) {
    if (p == "ff")
        return 0;
    if (p == "fr")
        return 1;
    if (p == "rf")
        return 2;
    if (p == "rr")
        return 3;
    throw std::runtime_error("Invalid polarity");
}
inline Frame hello(Kind k, U64 sid, U64 seq, const Config &c) {
    Frame f(k, sid, seq);
    f.put(24, c.rate, 2);
    f.put(26, c.freshness, 2);
    f.put(28, c.max_pwm, 2);
    f.put(30, U64(std::nearbyint(c.duration * 1000)), 4);
    f.put(34, c.motors, 1);
    f.put(35, polarity_index(c.polarity), 1);
    auto h = hash_bytes(c.hash);
    std::copy(h.begin(), h.end(), f.bytes.begin() + 36);
    return f;
}
inline bool same_hello(const Frame &a, const Frame &b) {
    return std::equal(a.bytes.begin() + 24, a.bytes.begin() + 68, b.bytes.begin() + 24);
}
inline Frame reference(Kind k, U64 sid, U64 seq, U64 sample) {
    Frame f(k, sid, seq);
    f.put(24, sample, 8);
    return f;
}
inline Frame command(U64 sid, U64 seq, U64 sample, int l, int r) {
    auto f = reference(COMMAND, sid, seq, sample);
    f.put(32, uint16_t(l), 2);
    f.put(34, uint16_t(r), 2);
    return f;
}
} // namespace lf
