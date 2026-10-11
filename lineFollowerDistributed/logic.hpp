#pragma once
#include "protocol.hpp"
#include <cfenv>
namespace lf {
struct Decision {
    std::array<double, 3> low{}, high{}, white{}, black{}, values{};
    std::array<int, 3> held{};
    bool initialized = false, previous_seen = false, seen = false;
    int left = 0, right = 0;
    double speed, sigma;
    std::string polarity;
    Decision(const fs::path &path, double sp, double sig) : speed(sp), sigma(sig) {
        auto j = json_read(path);
        require(string(member(j, "status")) == "complete" && boolean(member(j, "valid")) &&
                    boolean(member(j, "black_is_high")) && number(member(j, "adc_bits")) == 10,
                "Invalid calibration status");
        auto &sensors = member(j, "sensors");
        for (int i = 0; i < 3; i++) {
            auto &s = member(sensors, std::string(1, char('A' + i)));
            require(boolean(member(s, "valid")) &&
                        string(member(s, "analog_pin")) == "A" + std::to_string(11 + i) &&
                        number(member(s, "gpio_pin")) == 25 + i,
                    "Invalid sensor mapping");
            auto &w = member(s, "low_white");
            auto &b = member(s, "high_black");
            low[i] = number(member(w, "mean"));
            high[i] = number(member(b, "mean"));
            white[i] = number(member(w, "stddev"));
            black[i] = number(member(b, "stddev"));
            require(0 <= low[i] && low[i] < high[i] && high[i] <= 1023 && white[i] >= 0 &&
                        black[i] >= 0,
                    "Invalid calibration values");
        }
        polarity = string(member(member(j, "configuration"), "polarity"));
        polarity_index(polarity);
        std::fesetround(FE_TONEAREST);
    }
    static double clamp(double x) {
        return std::max(0., std::min(1., x));
    }
    double normalize(int i, int x) const {
        return clamp((x - low[i]) / (high[i] - low[i]));
    }
    double noise(int i, double fraction) const {
        return (1 - fraction) * white[i] + fraction * black[i];
    }
    void update(const std::array<int, 3> &raw) {
        seen = false;
        for (int i = 0; i < 3; i++) {
            require(raw[i] >= 0 && raw[i] <= 1023, "ADC out of range");
            if (raw[i] > low[i] + sigma * white[i])
                seen = true;
        }
        if (!initialized || !previous_seen || !seen || sigma == 0)
            held = raw;
        else
            for (int i = 0; i < 3; i++)
                if (std::abs(raw[i] - held[i]) >
                    sigma *
                        std::hypot(noise(i, normalize(i, raw[i])), noise(i, normalize(i, held[i]))))
                    held[i] = raw[i];
        initialized = true;
        previous_seen = seen;
        std::array<double, 3> n{};
        for (int i = 0; i < 3; i++) {
            values[i] = normalize(i, held[i]);
            n[i] = noise(i, values[i]) / (high[i] - low[i]);
        }
        if (!seen) {
            left = right = 0;
            return;
        }
        double gl = values[1] - values[0], gr = values[1] - values[2];
        if (std::abs(gl) <= sigma * std::hypot(n[1], n[0]))
            gl = 0;
        if (std::abs(gr) <= sigma * std::hypot(n[1], n[2]))
            gr = 0;
        int duty = int(std::nearbyint(255 * speed));
        left = int(std::nearbyint(duty * std::clamp(.5 + gl, -1., 1.)));
        right = int(std::nearbyint(duty * std::clamp(.5 + gr, -1., 1.)));
    }
};
} // namespace lf
