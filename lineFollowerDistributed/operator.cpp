#include "logic.hpp"
#include "queue_reset.hpp"
#include <iomanip>
#include <sstream>
namespace lf {
class Operator {
    Config c;
    std::unique_ptr<Decision> decision;
    Capture log;
    UDP udp;
    U64 sid = random64(), seq = 0, arm_seq = 0, arm_sample = 0, last_sample = 0, newest_seen = 0,
        last_recv = ns(), last_completion = 0, sensor_rx = 0;
    double midpoint_ms = 0, pi_minus_teensy_s = 0;
    void load_clock() {
        c.clock_matched = false;
        c.latency_uncertainty_ms = 0;
        auto raw = [&](const std::string &why) {
            c.clock_note = why + "; using the raw delivery upper bound";
        };
        try {
            if (c.boot.empty() || c.clock_file.empty() || !(c.pi_uptime_s > 0)) {
                raw("boot, clock file, or Pi uptime is missing");
            } else {
                auto boot = json_read(c.boot);
                auto clock = json_read(c.clock_file);
                auto boot_hash = string(member(boot, "hash8"));
                auto clock_hash = string(member(clock, "hash8"));
                if (boot_hash.size() != 8 || boot_hash != clock_hash) {
                    raw("boot hash8 " + boot_hash + " differs from clock hash8 " + clock_hash);
                } else {
                    auto &teensy = member(clock, "pi_teensy");
                    auto &wifi = member(member(clock, "mac_pi"), "wifi");
                    if (!boolean(member(teensy, "complete")))
                        raw("clock calibration on this boot is incomplete");
                    else if (number(member(wifi, "valid")) < 1)
                        raw("clock calibration has no Wi-Fi probes");
                    else {
                        double half = number(member(member(wifi, "half_width_ms"), "median"));
                        double residual_us = number(member(teensy, "detrended_residual_range_us"));
                        double drift = number(member(teensy, "drift_ppm"));
                        double elapsed = c.pi_uptime_s - number(member(clock, "uptime_s"));
                        if (!(half >= 0 && residual_us >= 0 && elapsed >= 0))
                            raw("clock calibration times are not usable");
                        else {
                            double drift_ms = std::abs(drift) * elapsed * 1e-3;
                            midpoint_ms = number(member(member(wifi, "midpoint_ms"), "median"));
                            pi_minus_teensy_s = number(member(teensy, "nominal_offset_s"));
                            c.latency_uncertainty_ms = half + residual_us / 1000.0 + drift_ms;
                            c.clock_matched = true;
                            std::ostringstream note;
                            note << std::fixed << std::setprecision(3) << "hash8 " << boot_hash
                                 << " matches; Wi-Fi half-width " << half << " ms; Pi-Teensy residual "
                                 << residual_us / 1000.0 << " ms; drift " << drift_ms
                                 << " ms; transmit uncertainty " << c.latency_uncertainty_ms << " ms";
                            c.clock_note = note.str();
                        }
                    }
                }
            }
        } catch (const std::exception &e) {
            c.clock_matched = false;
            raw(e.what());
        }
        std::cout << "CLOCK " << c.clock_note << std::endl;
    }
    void report_latency() {
        auto n = std::min<U64>(log.metric_total[TRANSMIT], log.metric_capacity);
        std::cout << std::fixed << std::setprecision(3);
        if (!n) {
            std::cout << "LATENCY no samples" << std::endl;
            return;
        }
        std::vector<double> values(log.metrics[TRANSMIT].begin(),
                                   log.metrics[TRANSMIT].begin() + long(n));
        std::sort(values.begin(), values.end());
        double median = values[size_t(std::ceil(.5 * n)) - 1];
        if (c.clock_matched)
            std::cout << "LATENCY median " << median << " ms; uncertainty "
                      << c.latency_uncertainty_ms << " ms (" << c.clock_note << ")" << std::endl;
        else
            std::cout << "LATENCY median upper bound " << median << " ms; uncertainty " << median
                      << " ms (" << c.clock_note << ")" << std::endl;
    }
    bool job_prepared=false, init_ready=false, ready_seen=false;
    void write_text(const fs::path &path, const std::string &text) {
        std::ofstream out(path, std::ios::binary);
        require(bool(out), "Cannot write " + path.string());
        out << text;
        require(bool(out), "Cannot write " + path.string());
    }
    void take_init(const InitFiles &files) {
        c.pi_uptime_s = files.uptime_s;
        c.calibration = log.path / "photo3_calib.json";
        write_text(c.calibration, files.photo);
        c.hash = sha256(c.calibration);
        hash_bytes(c.hash);
        if (files.boot.empty())
            c.boot.clear();
        else {
            c.boot = log.path / "boot.json";
            write_text(c.boot, files.boot);
        }
        if (files.clock.empty())
            c.clock_file.clear();
        else {
            c.clock_file = log.path / "clock_calib.json";
            write_text(c.clock_file, files.clock);
        }
        init_ready = true;
        std::cout << "INIT photo3 " << files.photo.size() << " bytes, boot " << files.boot.size()
                  << " bytes, clock " << files.clock.size() << " bytes, uptime " << files.uptime_s
                  << " s" << std::endl;
    }
    void prepare_job() {
        flush_job_queues(udp,log,sid,"startup");
        U64 deadline=ns()+45000000000ULL,next=0;
        while(running && ns()<deadline && !job_prepared){
            if(ns()>=next){send(frame(JOB_REQUEST));next=ns()+500000000;}
            wait_io(udp.fd.value,-1,false,std::min(deadline,next));
            std::array<uint8_t,8192> bytes{};sockaddr_in from{};
            auto count=udp.receive(bytes.data(),bytes.size(),from);
            if(count<0)continue;
            if(!same(from,udp.peer))continue;
            if(count>=6 && std::memcmp(bytes.data(),"LFINIT",6)==0){
                InitFiles files;
                require(unpack_init(bytes.data(),size_t(count),files),"Bad initialization message");
                take_init(files);
            }else{
                Frame f;
                if(!f.parse(bytes.data(),size_t(count))||f.session!=sid)continue;
                require(f.kind!=JOB_FAILED,"Pi job preparation failed; inspect Pi job record");
                if(f.kind==JOB_READY)ready_seen=true;
            }
            job_prepared=init_ready && ready_seen;
        }
        require(running&&job_prepared,"Pi job preparation timed out; no initialization received");
    }
    bool welcomed = false, active = false;
    Frame handshake;
    U64 send(Frame f) {
        U64 stamp = ns();
        bool ok = udp.send(f.bytes.data(), f.size);
        log.event("udp_tx", f.sequence, stamp, f.bytes[5], ok);
        if (!ok) ++log.counts[SEND_ERRORS];
        return f.sequence;
    }
    Frame frame(Kind k) {
        return Frame(k, sid, ++seq);
    }
    bool receive(Frame &newest) {
        bool has_new = false;
        for (int i = 0; i < 128; i++) {
            std::array<uint8_t, 2048> raw{};
            sockaddr_in from{};
            auto n = udp.receive(raw.data(), raw.size(), from);
            if (n < 0)
                break;
            const U64 received_at = ns();
            Frame f;
            if (!same(from, udp.peer) || !f.parse(raw.data(), size_t(n)) || f.session != sid) {
                ++log.counts[REJECTED];
                continue;
            }
            log.event("udp_rx", f.sequence, received_at, f.kind, int(n));
            last_recv = received_at;
            if (f.kind == WELCOME) {
                require(same_hello(f, handshake), "Configuration mismatch");
                welcomed = true;
            } else if (f.kind == ACK && f.get(26, 8) == arm_seq) {
                require(f.get(24, 1) == OK && f.get(25, 1) == ACTIVE,
                        "Arming rejected; fresh session required");
                if (!active) {
                    active = true;
                    log.event("armed", sid);
                    std::cout << "ACTIVE: "
                              << (c.motors ? "MOTOR OUTPUT ENABLED" : "MOTORS DISABLED")
                              << std::endl;
                }
            } else if (f.kind == SENSOR) {
                U64 id = f.get(24, 8);
                if (f.get(32, 8) > f.get(40, 8) || f.get(48, 2) > 1023 || f.get(50, 2) > 1023 ||
                    f.get(52, 2) > 1023 || f.get(58, 8) > f.get(66, 8) || !f.get(74, 8) || id <= newest_seen) {
                    ++log.counts[REJECTED];
                    continue;
                }
                if (newest_seen)
                    log.counts[SEQUENCE_GAPS] += id - newest_seen - 1;
                sensor_rx = received_at;
                log.event("sensor_received", id, received_at,
                          int(f.get(48, 2)), int(f.get(50, 2)));
                log.event("sensor_received_c", id, received_at, int(f.get(52, 2)));
                newest_seen = id;
                ++log.counts[SENSOR_PACKETS];
                if (has_new)
                    ++log.counts[COALESCED];
                newest = f;
                has_new = true;
            } else if (f.kind == STATUS) {
                require(bool(f.get(26, 1)) == c.motors, "Unexpected motor mode");
                log.event("status", f.get(27, 8), f.get(35, 8), int(f.get(24, 1)),
                          int(f.get(25, 1)));
                if (f.get(24, 1) == STOPPED || f.get(24, 1) == FAULT)
                    throw std::runtime_error(
                        "Controller stopped: reason=" + std::to_string(f.get(25, 1)) +
                        "; explicit restart required");
            }
        }
        return has_new;
    }
    bool stop() {
        if (!welcomed)
            return false;
        Frame f = frame(STOP);
        send(f);
        U64 end = ns() + 1000000000ULL, next = ns() + 100000000;
        while (ns() < end) {
            if (ns() >= next) {
                send(f);
                next = ns() + 100000000;
            }
            wait_io(udp.fd.value, -1, false, std::min(end, next));
            for (int i = 0; i < 128; i++) {
                std::array<uint8_t, 2048> raw{};
                sockaddr_in from{};
                auto n = udp.receive(raw.data(), raw.size(), from);
                if (n < 0)
                    break;
                U64 received_at = ns();
                Frame r;
                if (same(from, udp.peer) && r.parse(raw.data(), size_t(n)) && r.session == sid) {
                    log.event("udp_rx", r.sequence, received_at, r.kind, int(n));
                    if (r.kind == ACK && r.get(24, 1) == OK && r.get(25, 1) == STOPPED &&
                        r.get(26, 8) == f.sequence) return true;
                }
            }
        }
        return false;
    }

  public:
    explicit Operator(Config conf)
        : c(std::move(conf)), log(c.output, "operator"), udp(c.bind, c.port, c.peer, c.peer_port) {}
    int run() {
        std::string error;
        bool stopped = false;
        timespec wall{};
        U64 wall_before = ns();
        clock_gettime(CLOCK_REALTIME, &wall);
        U64 wall_after = ns();
        U64 wall_ns = U64(wall.tv_sec) * 1000000000ULL + wall.tv_nsec;
        log.event("wall_anchor", wall_before, wall_ns);
        log.event("wall_anchor_end", wall_after, wall_ns);
        U64 start = ns(), end = start + U64(c.duration * 1e9), retry = start + 100000000,
            previous = start;
        try {
            prepare_job();
            decision = std::make_unique<Decision>(c.calibration, c.speed, c.n_sigma);
            load_clock();
            handshake = hello(HELLO, sid, ++seq, c);
            flush_job_queues(udp,log,sid,"before_hello");
            // Job preparation is before HELLO/ARM and does not consume drive duration.
            start=ns(); end=start+U64(c.duration*1e9); retry=start+100000000;
            previous=last_recv=start;
            U64 next_left=start;
            send(handshake);
            while (running && ns() < end) {
                U64 now = ns();
                if (now >= next_left) {
                    U64 left_ns = end > now ? end - now : 0;
                    std::cout << "TIME LEFT " << (left_ns + 500000000ULL) / 1000000000ULL
                              << " s" << std::endl;
                    next_left = now + 10000000000ULL;
                }
                log.metric(LOOP, (now - previous) / 1e6);
                previous = now;
                if (now - last_recv > 2000000000ULL)
                    throw std::runtime_error("No controller traffic for 2 seconds");
                if (!active && now - start > 5000000000ULL)
                    throw std::runtime_error("Session handshake timed out");
                if (now >= retry) {
                    retry = now + 50000000;
                    if (!welcomed)
                        send(handshake);
                    else if (arm_seq && !active)
                        send(reference(ARM, sid, arm_seq, arm_sample));
                }
                wait_io(udp.fd.value, -1, false, std::min(end, now + 20000000));
                Frame newest;
                if (receive(newest)) {
                    U64 sample = newest.get(24, 8), completed = newest.get(40, 8);
                    if (welcomed && !arm_seq) {
                        arm_sample = sample;
                        arm_seq = ++seq;
                        send(reference(ARM, sid, arm_seq, sample));
                    }
                    if (active && sample > last_sample) {
                        last_sample = sample;
                        std::array<int, 3> raw{int(newest.get(48, 2)), int(newest.get(50, 2)),
                                               int(newest.get(52, 2))};
                        U64 tick = ns();
                        decision->update(raw);
                        const U64 computed_at = ns();
                        log.metric(COMPUTE, (computed_at - tick) / 1e6);
                        log.event("target_computed", sample, computed_at, decision->left, decision->right);
                        auto f = command(sid, ++seq, sample, decision->left, decision->right);
                        send(f);
                        ++log.counts[COMMANDS];
                        if (decision->left || decision->right)
                            ++log.counts[NONZERO_TARGETS];
                        if (last_completion)
                            log.metric(INTERVAL, (completed - last_completion) / 1e3);
                        last_completion = completed;
                        log.metric(ADC, (completed - newest.get(32, 8)) / 1e3);
                        log.metric(DELIVERY, (newest.get(66, 8) - newest.get(58, 8)) / 1e6);
                        double transmit_ms;
                        if (c.clock_matched) {
                            double pi_of_sample =
                                double(newest.get(40, 8)) * 1000.0 + pi_minus_teensy_s * 1e9;
                            transmit_ms = (double(sensor_rx) - (pi_of_sample + midpoint_ms * 1e6)) / 1e6;
                        } else
                            transmit_ms = (double(newest.get(66, 8)) - double(newest.get(58, 8))) / 1e6;
                        if (std::isfinite(transmit_ms))
                            log.metric(TRANSMIT, transmit_ms);
                        log.event("device_sample", newest.get(74, 8), completed, raw[0], raw[2]);
                        log.event("decision", sample, f.sequence, decision->left, decision->right);
                    }
                }
            }
        } catch (const std::exception &e) {
            error = e.what();
            std::cerr << "ERROR " << error << std::endl;
        }
        try {
            stopped = stop();
        } catch (const std::exception &) {
            stopped = false;
        }
        log.event("final_stop", 0, 0, stopped);
        report_latency();
        log.finish(c, false, stopped, error);
        return error.empty() && stopped ? 0 : 1;
    }
};
} // namespace lf
int main(int argc, char **argv) {
    try {
        lf::signals();
        lf::Operator o(lf::config(argc, argv, false));
        return o.run();
    } catch (const std::exception &e) {
        std::cerr << "ERROR " << e.what() << '\n';
        return 1;
    }
}
