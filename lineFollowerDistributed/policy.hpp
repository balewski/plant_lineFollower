#pragma once
#include "protocol.hpp"
namespace lf {
struct Target {
    U64 command = 0, sample = 0, expiry = 0, completion = 0;
    int left = 0, right = 0;
};
struct Policy {
    struct Sample {
        U64 id = 0, time = 0;
    };
    std::array<Sample, 512> samples{};
    State state = READY;
    Reason reason = NONE;
    U64 freshness = 100000000, duration = 60000000000ULL, expiry = 0, run_end = 0, last_command = 0,
        last_sample = 0, arm_seq = 0;
    int max_pwm = 128;
    Target pending{};
    bool has_pending = false;
    Policy() = default;
    Policy(int fresh, int pwm, U64 duration_ms)
        : freshness(U64(fresh) * 1000000), duration(duration_ms * 1000000), max_pwm(pwm) {}
    void sample(U64 id, U64 time) {
        samples[id % samples.size()] = {id, time};
    }
    U64 completion(U64 id) const {
        const auto &s = samples[id % samples.size()];
        return s.id == id && id ? s.time : 0;
    }
    bool fresh(U64 id, U64 now) const {
        U64 t = completion(id);
        return t && now >= t && now - t < freshness;
    }
    void stop(Reason r) {
        state = r == SERIAL ? FAULT : STOPPED;
        reason = r;
        has_pending = false;
    }
    void tick(U64 now) {
        if (state == ACTIVE) {
            if (now >= run_end)
                stop(DURATION);
            else if (now >= expiry)
                stop(EXPIRED);
        }
    }
    bool arm(U64 seq, U64 id, U64 now) {
        tick(now);
        if (state == ACTIVE)
            return seq == arm_seq;
        if (state != READY || !fresh(id, now))
            return false;
        state = ACTIVE;
        arm_seq = seq;
        expiry = completion(id) + freshness;
        run_end = now + duration;
        return true;
    }
    bool accept(U64 seq, U64 id, int l, int r, U64 now) {
        tick(now);
        if (state != ACTIVE || seq <= last_command || id <= last_sample || !fresh(id, now) ||
            std::abs(l) > max_pwm || std::abs(r) > max_pwm)
            return false;
        last_command = seq;
        last_sample = id;
        expiry = completion(id) + freshness;
        pending = {seq, id, expiry, completion(id), l, r};
        has_pending = true;
        return true;
    }
    bool take(U64 now, Target &t) {
        tick(now);
        if (!has_pending || state != ACTIVE)
            return false;
        t = pending;
        has_pending = false;
        return now < t.expiry;
    }
};
inline void motor_line(char *buffer, size_t size, int l, int r, const std::string &polarity,
                       bool enabled) {
    require(l >= -255 && l <= 255 && r >= -255 && r <= 255, "Invalid motor target");
    polarity_index(polarity);
    if (!enabled || (!l && !r))
        std::snprintf(buffer, size, "STOPALL");
    else
        std::snprintf(buffer, size, "MOTORS %d %d", l * (polarity[0] == 'f' ? 1 : -1),
                      r * (polarity[1] == 'f' ? 1 : -1));
}
} // namespace lf
