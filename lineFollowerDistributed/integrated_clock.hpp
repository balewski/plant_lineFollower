#pragma once
#include "job_clock.hpp"
namespace lf {
inline void atomic_json(const fs::path &path, const Json &j) {
    fs::create_directories(path.parent_path());
    auto tmp=path.string()+".tmp."+std::to_string(getpid());
    {std::ofstream f(tmp);f<<j.serialize(true)<<'\n';f.close();require(bool(f),"Cannot save calibration JSON");}
    fs::rename(tmp,path);
}
inline double signed_number(U64 v) {return v<=U64(INT64_MAX)?double(v):-double(~v)-1;}
struct GpioClock {
    U64 reference=0, first_teensy=0, last_teensy=0;
    double offset=0, ppm=0, allowance=100000;
    unsigned samples=0;
    bool ready=false;
    void load(const Json &j) {
        require(boolean(member(j,"complete")) && boolean(member(j,"stop_confirmed")) &&
                number(member(j,"schema"))==2,"GPIO calibration incomplete or unsupported sign convention");
        reference=U64(number(member(j,"reference_pi_monotonic_ns")));
        offset=number(member(j,"nominal_offset_at_reference_us"))*1000;
        ppm=number(member(j,"fitted_drift_ppm"));
        samples=unsigned(number(member(j,"valid_samples")));
        const auto &records=member(j,"records").get<picojson::array>();
        require(samples>=200 && records.size()==samples,"Insufficient GPIO edges");
        first_teensy=U64(number(member(records.front(),"teensy_before_us")));
        last_teensy=U64(number(member(records.back(),"teensy_after_us")));
        require(reference && first_teensy && last_teensy>first_teensy && std::abs(ppm)<500,
                "Invalid GPIO clock fit");ready=true;
    }
    void put(Frame &f) const {
        f.put(24,reference,8);f.put(32,U64(int64_t(std::llround(offset))),8);
        f.put(40,U64(int64_t(std::llround(ppm*1000))),8);f.put(48,U64(allowance),8);
        f.put(56,first_teensy,8);f.put(64,last_teensy,8);f.put(72,samples,8);
    }
    void get(const Frame &f) {
        reference=f.get(24,8);offset=signed_number(f.get(32,8));ppm=signed_number(f.get(40,8))/1000;
        allowance=double(f.get(48,8));first_teensy=f.get(56,8);last_teensy=f.get(64,8);samples=unsigned(f.get(72,8));
        require(reference && samples>=200 && first_teensy && last_teensy>first_teensy &&
                std::abs(ppm)<500 && allowance==100000,"Invalid GPIO summary");ready=true;
    }
    double pi_ns(double teensy_ns) const {
        const long double rate=ppm/1e6L;
        return double((teensy_ns+static_cast<long double>(offset)-rate*reference)/(1-rate));
    }
    double error_ns(double pi_at) const {return allowance+std::abs(pi_at-reference)*50e-6;}
    void check_anchor(U64 teensy_us,U64 before,U64 after) const {
        auto mapped=pi_ns(double(teensy_us)*1000),guard=error_ns(mapped);
        require(teensy_us>=last_teensy && mapped>=before-guard && mapped<=after+guard,
                "GPIO clock mapping expired/reset/inconsistent with UART clock anchor");
    }
    Json json() const {
        return Json(Object{{"complete",Json(ready)},{"reference_pi_ns",jnum(reference)},
         {"pi_minus_teensy_ns",jnum(offset)},{"fitted_drift_ppm",jnum(ppm)},
         {"valid_samples",jnum(samples)},{"first_teensy_us",jnum(first_teensy)},
         {"last_teensy_us",jnum(last_teensy)},{"assumed_latency_allowance_ns",jnum(allowance)},
         {"assumed_extrapolation_rate_error_ppm",jnum(50)},
         {"absolute_accuracy_validated",Json(false)}});
    }
};
struct NetworkClock {
    struct Probe {U64 id,d1,p2,p3,d4;};
    std::vector<Probe> rows;
    double offset=0,lower=0,upper=0,best=1e100;
    U64 reference=0;
    bool ready=false;
    void add(U64 id,U64 d1,U64 p2,U64 p3,U64 d4) {
        require(d4>=d1 && p3>=p2 && p3-p2<=d4-d1+10000,"Invalid network timestamps");
        require(rows.empty()||p2>rows.back().p3,"Pi clock reset/reordered network probe");
        rows.push_back({id,d1,p2,p3,d4});
    }
    void finish() {
        require(rows.size()==100,"Need 100 Desktop-Pi network replies");
        for(auto &p:rows){double r=double(p.d4-p.d1)-double(p.p3-p.p2);if(r<best){best=r;reference=p.d1+(p.d4-p.d1)/2;offset=(double(int64_t(p.p2)-int64_t(p.d1))+double(int64_t(p.p3)-int64_t(p.d4)))/2;}}
        lower=-1e100;upper=1e100;
        for(auto &p:rows){double guard=std::max(std::abs(double(int64_t(reference)-int64_t(p.d1))),std::abs(double(int64_t(reference)-int64_t(p.d4))))*.002+2000;
            lower=std::max(lower,double(int64_t(p.p3)-int64_t(p.d4))-guard);
            upper=std::min(upper,double(int64_t(p.p2)-int64_t(p.d1))+guard);}
        require(lower<=upper,"Inconsistent Desktop-Pi clocks");offset=std::clamp(offset,lower,upper);ready=true;
    }
    Json json() const {
        picojson::array raw;for(auto&p:rows)raw.push_back(Json(Object{{"id",jnum(p.id)},{"desktop_send_ns",jnum(p.d1)},{"pi_receive_ns",jnum(p.p2)},{"pi_send_ns",jnum(p.p3)},{"desktop_receive_ns",jnum(p.d4)}}));
        return Json(Object{{"complete",Json(ready)},{"reference_desktop_ns",jnum(reference)},
          {"pi_minus_desktop_ns",jnum(offset)},{"offset_lower_ns",jnum(lower)},{"offset_upper_ns",jnum(upper)},
          {"minimum_network_rtt_ns",jnum(ready?best:0)},{"relative_drift_bound_ppm",jnum(2000)},
          {"valid_replies",jnum(rows.size())},{"probes",Json(raw)}});
    }
};
inline Json compose_clock(const GpioClock &g,const NetworkClock &n,const JobClock &end_to_end) {
    require(g.ready&&n.ready&&end_to_end.ready,"Calibration phases incomplete");
    const double pi_ref=double(n.reference)+n.offset;
    const double theta=g.offset+g.ppm/1e6*(pi_ref-g.reference);
    const double desktop_minus_teensy=theta-n.offset;
    const double error=g.error_ns(pi_ref)+std::max(n.offset-n.lower,n.upper-n.offset);
    const double predicted_teensy=double(n.reference)-desktop_minus_teensy;
    require(predicted_teensy>=double(g.last_teensy)*1000,"Teensy epoch changed after GPIO calibration");
    const double dt=std::abs(double(int64_t(end_to_end.reference)-int64_t(n.reference)));
    const double check_error=error+dt*.002;
    const double e2e_low=-end_to_end.upper,e2e_high=-end_to_end.lower;
    require(desktop_minus_teensy+check_error>=e2e_low && desktop_minus_teensy-check_error<=e2e_high,
            "GPIO+network clock mapping disagrees with end-to-end calibration");
    return Json(Object{{"reference_desktop_ns",jnum(n.reference)},
      {"desktop_minus_teensy_ns",jnum(desktop_minus_teensy)},
      {"estimated_uncertainty_ns",jnum(error)},{"uncertainty_is_conditional",Json(true)},
      {"absolute_accuracy_validated",Json(false)},
      {"method",Json(std::string("GPIO Pi-Teensy fit plus Desktop-Pi UDP offset; no UART symmetry assumption"))},
      {"end_to_end_consistency_passed",Json(true)}});
}
}
