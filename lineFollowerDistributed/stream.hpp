#pragma once
#include "common.hpp"
namespace lf {
struct Sample {
    U64 sequence = 0, started = 0, completed = 0, dropped = 0;
    std::array<int, 3> adc{};
};
inline Sample parse_sample(const char *s) {
    unsigned long long seq, start, end, drops;
    int a, b, c;
    char extra;
    require(std::sscanf(s, "S %llu %llu %llu %llu %d %d %d %c", &seq, &start, &end, &drops, &a, &b,
                        &c, &extra) == 7 &&
                seq && seq < (1ULL << 48) && start <= end && end < (1ULL << 54) &&
                end - start <= 300 && drops <= UINT32_MAX && a >= 0 && a <= 1023 && b >= 0 &&
                b <= 1023 && c >= 0 && c <= 1023,
            "Invalid Teensy sample");
    return {seq, start, end, drops, {a, b, c}};
}
// No Desktop clock synchronization. CLOCK is processed after Pi sends its query;
// Pi send time is a lower bound on that Teensy instant. Allow 1000 ppm relative
// oscillator error plus 2 us quantization. Refresh every second; expire in 2 s.
// Thus UART/kernel backlog consumes, rather than resets, command lifetime.
struct ClockMap {
    U64 device = 0, host = 0, received = 0;
    void update(const char *line, U64 sent, U64 recv) {
        unsigned long long value;
        char extra;
        require(std::sscanf(line, "CLOCK %llu %c", &value, &extra) == 1 && value < (1ULL << 54) &&
                    recv >= sent && (!received || (value > device && sent > host)),
                "Invalid/reset Teensy clock");
        device = value;
        host = sent;
        received = recv;
    }
    U64 earliest(U64 us, U64 now) const {
        require(received && now >= received && now - received < 2000000000ULL,
                "Teensy clock mapping expired");
        U64 delta = us >= device ? us - device : device - us;
        require(delta <= 3000000, "Sample outside clock mapping window");
        U64 guard = delta + 2000; // delta us * 1000 ns/us * 0.001 + 2 us
        U64 value;
        if (us >= device) {
            require(host + delta * 1000 > guard, "Clock bound underflow");
            value = host + delta * 1000 - guard;
        } else {
            require(host > delta * 1000 + guard, "Clock bound underflow");
            value = host - delta * 1000 - guard;
        }
        require(value <= now, "Teensy clock ahead of Pi bound");
        return value;
    }
};
} // namespace lf
