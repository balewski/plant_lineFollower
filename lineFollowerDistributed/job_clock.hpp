#pragma once
#include "protocol.hpp"
namespace lf {
// Startup estimate only. Positive offset means Teensy's clock is ahead of Desktop.
// All durations below are ns except the on-wire Teensy receive/send stamps (us).
struct JobClock {
    static constexpr size_t required = 100;
    static constexpr double drift = .002; // conservative Desktop/Teensy relative rate bound
    struct Probe { U64 id=0, d1=0, t2=0, t3=0, d4=0; };
    std::array<Probe, required> probes{};
    size_t count = 0;
    U64 pending_id=0, pending_sent=0, started=0, finished=0, reference=0;
    unsigned attempts=0;
    double offset=0, lower=0, upper=0, best_rtt=0, symmetric_offset=0;
    bool ready=false;
    void begin(U64 now) { started=now; }
    void sent(U64 id, U64 stamp) { pending_id=id; pending_sent=stamp; ++attempts; }
    bool receive(const Frame &f, U64 now) {
        if (ready || count>=required || f.kind!=CAL_REPLY || !pending_id ||
            f.get(24,8)!=pending_id || f.get(32,8)!=pending_sent || now<pending_sent ||
            now>U64(INT64_MAX) || now-pending_sent>120000000ULL) return false;
        U64 t2=f.get(40,8), t3=f.get(48,8);
        if (!t2 || t3<t2 || t3>U64(INT64_MAX)/1000 ||
            (t3-t2)*1000 > now-pending_sent ||
            (count && t2<=probes[count-1].t3)) return false;
        probes[count++]={pending_id,pending_sent,t2,t3,now};
        pending_id=pending_sent=0;
        return true;
    }
    void finish(U64 now) {
        require(count==required, "Need 100 valid startup clock replies");
        finished=now;
        best_rtt=1e100;
        for (const auto &p:probes) {
            double rtt=double(p.d4-p.d1)-double(p.t3-p.t2)*1000;
            if (rtt<best_rtt) {
                best_rtt=rtt; reference=p.d1+(p.d4-p.d1)/2;
                symmetric_offset=(double(int64_t(p.t2*1000)-int64_t(p.d1))+
                                  double(int64_t(p.t3*1000)-int64_t(p.d4)))/2;
            }
        }
        lower=-1e100; upper=1e100;
        for (const auto &p:probes) {
            double distance=std::max(std::abs(double(int64_t(reference)-int64_t(p.d1))),
                                     std::abs(double(int64_t(reference)-int64_t(p.d4))));
            double guard=distance*drift+2000; // rate uncertainty + 2 us quantization
            lower=std::max(lower,double(int64_t(p.t3*1000)-int64_t(p.d4))-guard);
            upper=std::min(upper,double(int64_t(p.t2*1000)-int64_t(p.d1))+guard);
        }
        require(lower<=upper,"Inconsistent startup clock bounds; refusing to arm");
        // Best-RTT midpoint is the usual symmetric-path estimate, projected into
        // all causal bounds. A fixed path asymmetry still cannot be identified.
        offset=std::clamp(symmetric_offset,lower,upper);
        ready=true;
    }
    void save(const fs::path &path) const {
        std::ofstream raw(path/"clock_calibration.csv");
        raw << "probe_id,desktop_send_ns,teensy_receive_us,teensy_send_us,desktop_receive_ns\n";
        for(size_t i=0;i<count;++i) {
            const auto &p=probes[i];
            raw<<p.id<<','<<p.d1<<','<<p.t2<<','<<p.t3<<','<<p.d4<<'\n';
        }
        Object out{{"complete",Json(ready)}, {"valid_replies",jnum(count)},
                   {"required_replies",jnum(required)}, {"attempts",jnum(attempts)},
                   {"duration_s",jnum(finished>=started && started ? (finished-started)/1e9 : 0)},
                   {"clock_domain",Json(std::string("Teensy extended micros vs Desktop CLOCK_MONOTONIC"))},
                   {"offset_definition",Json(std::string("Teensy_ns minus Desktop_ns; Desktop_estimate = Teensy_ns - offset"))},
                   {"method",Json(std::string("Minimum-RTT four-timestamp estimate with drift-expanded causal bounds"))},
                   {"relative_drift_bound_ppm",jnum(drift*1e6)}};
        if(ready) {
            out["reference_desktop_ns"]=jnum(reference);
            out["teensy_minus_desktop_ns"]=jnum(offset);
            out["offset_lower_ns"]=jnum(lower); out["offset_upper_ns"]=jnum(upper);
            out["uncertainty_ns"]=jnum(std::max(offset-lower,upper-offset));
            out["minimum_round_trip_ms"]=jnum(best_rtt/1e6);
            out["fastest_symmetric_offset_ns"]=jnum(symmetric_offset);
        }
        std::ofstream f(path/"clock_calibration.json"); f<<Json(out).serialize(true)<<'\n';
        require(bool(raw)&&bool(f),"Cannot save clock calibration");
    }
};
} // namespace lf
