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
// One datagram, sent with each JOB_READY. Raw file bytes, so the desktop hash matches the Pi file.
struct InitFiles {
    double uptime_s = 0;
    std::string boot, clock, photo;
};
inline void store_be(uint8_t *p, U64 v, size_t n) {
    for (size_t i = 0; i < n; i++)
        p[n - 1 - i] = uint8_t(v >> (8 * i));
}
inline U64 load_be(const uint8_t *p, size_t n) {
    U64 v = 0;
    for (size_t i = 0; i < n; i++)
        v = (v << 8) | p[i];
    return v;
}
inline std::vector<uint8_t> pack_init(const InitFiles &in) {
    require(in.photo.size() > 0 && in.photo.size() <= 20000 && in.boot.size() <= 20000 &&
                in.clock.size() <= 20000,
            "Initialization files are empty or too large");
    U64 ms = U64(std::llround(in.uptime_s * 1000));
    std::vector<uint8_t> out(27 + in.boot.size() + in.clock.size() + in.photo.size());
    std::memcpy(out.data(), "LFINIT", 6);
    out[6] = 1;
    store_be(out.data() + 7, ms, 8);
    store_be(out.data() + 15, in.boot.size(), 4);
    store_be(out.data() + 19, in.clock.size(), 4);
    store_be(out.data() + 23, in.photo.size(), 4);
    auto at = out.data() + 27;
    std::memcpy(at, in.boot.data(), in.boot.size());
    at += in.boot.size();
    std::memcpy(at, in.clock.data(), in.clock.size());
    at += in.clock.size();
    std::memcpy(at, in.photo.data(), in.photo.size());
    return out;
}
inline bool unpack_init(const uint8_t *p, size_t n, InitFiles &out) {
    if (n < 27 || std::memcmp(p, "LFINIT", 6) || p[6] != 1)
        return false;
    U64 boot_n = load_be(p + 15, 4), clock_n = load_be(p + 19, 4), photo_n = load_be(p + 23, 4);
    if (boot_n > 20000 || clock_n > 20000 || photo_n == 0 || photo_n > 20000 ||
        n != 27 + boot_n + clock_n + photo_n)
        return false;
    out.uptime_s = double(load_be(p + 7, 8)) / 1000;
    const uint8_t *at = p + 27;
    out.boot.assign(reinterpret_cast<const char *>(at), boot_n);
    at += boot_n;
    out.clock.assign(reinterpret_cast<const char *>(at), clock_n);
    at += clock_n;
    out.photo.assign(reinterpret_cast<const char *>(at), photo_n);
    return out.uptime_s > 0;
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
