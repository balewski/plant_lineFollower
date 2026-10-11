#include "logic.hpp"
#include "integrated_clock.hpp"
#include "queue_reset.hpp"
#include <iomanip>
namespace lf {
class Operator {
    Config c;
    Decision decision;
    Capture log;
    UDP udp;
    U64 sid = random64(), seq = 0, arm_seq = 0, arm_sample = 0, last_sample = 0, newest_seen = 0,
        last_recv = ns(), last_completion = 0, next_sync = 0;
    SyncHistory sync_history;
    JobClock job_clock;
    NetworkClock network_clock;
    GpioClock gpio_clock;
    Json combined_clock;
    bool job_prepared=false;
    void save_calibrations(const std::string &error="") {
        picojson::array raw;
        for(size_t i=0;i<job_clock.count;++i){auto&p=job_clock.probes[i];raw.push_back(Json(Object{
          {"probe_id",jnum(p.id)},{"desktop_send_ns",jnum(p.d1)},
          {"teensy_receive_us",jnum(p.t2)},{"teensy_send_us",jnum(p.t3)},
          {"desktop_receive_ns",jnum(p.d4)}}));}
        Json record(Object{{"schema",jnum(1)},{"job_session",Json(std::to_string(sid))},
          {"complete",Json(!combined_clock.is<picojson::null>())},
          {"run_error",error.empty()?Json():Json(error)},
          {"job_path",Json(log.path.string())},{"gpio",gpio_clock.json()},
          {"desktop_pi",network_clock.json()},{"combined",combined_clock},
          {"end_to_end_probes",Json(raw)},
          {"end_to_end_minimum_rtt_ns",jnum(job_clock.ready?job_clock.best_rtt:0)}});
        atomic_json(log.path/"desktop_teensy_clock.json",record);
        atomic_json(c.output/"desktop_teensy_clock.latest.json",record);
        atomic_json(log.path/"desktop_pi_clock.json",network_clock.json());
        atomic_json(c.output/"desktop_pi_clock.latest.json",network_clock.json());
    }
    void prepare_job() {
        flush_job_queues(udp,log,sid,"startup");
        save_calibrations();
        std::cout<<(c.timing?"PREPARING Pi-Teensy GPIO calibration: three synchronized flashes":
                    "Skipping Pi-Teensy GPIO timing measurement")<<std::endl;
        U64 deadline=ns()+45000000000ULL,next=0;
        while(running && ns()<deadline && !job_prepared){
            if(ns()>=next){send(frame(JOB_REQUEST));next=ns()+500000000;}
            wait_io(udp.fd.value,-1,false,std::min(deadline,next));
            std::array<uint8_t,2048> bytes{};sockaddr_in from{};
            auto count=udp.receive(bytes.data(),bytes.size(),from);Frame f;
            if(count<0)continue;
            if(!same(from,udp.peer)||!f.parse(bytes.data(),size_t(count))||f.session!=sid)continue;
            require(f.kind!=JOB_FAILED,"Pi calibration failed; inspect Pi job record");
            if(f.kind==JOB_READY){if(c.timing)gpio_clock.get(f);job_prepared=true;}
        }
        require(running&&job_prepared,"Pi GPIO calibration preparation timed out; no ARM sent");
        std::cout<<"CALIBRATING 100 Desktop-Pi UDP exchanges"<<std::endl;
        deadline=ns()+10000000000ULL;U64 pending=0,stamp=0;
        while(running&&ns()<deadline&&network_clock.rows.size()<100){
            if(!pending||ns()-stamp>=120000000){auto f=frame(SYNC_REQUEST);pending=f.sequence;stamp=ns();f.put(24,stamp,8);send(f);log.event("startup_sync_sent",pending,stamp);}
            wait_io(udp.fd.value,-1,false,std::min(deadline,stamp+120000000));
            for(int i=0;i<128;++i){std::array<uint8_t,2048> bytes{};sockaddr_in from{};
                auto count=udp.receive(bytes.data(),bytes.size(),from);U64 received=ns();Frame f;
                if(count<0)break;
                if(!same(from,udp.peer)||!f.parse(bytes.data(),size_t(count))||f.session!=sid||f.kind!=SYNC_REPLY||
                   f.get(24,8)!=pending||f.get(32,8)!=stamp)continue;
                network_clock.add(pending,stamp,f.get(40,8),f.get(48,8),received);pending=0;
            }
        }
        require(running,"Interrupted during network calibration");network_clock.finish();save_calibrations();
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
    void calibrate() {
        job_clock.begin(ns());
        const U64 deadline=ns()+6000000000ULL;
        std::cout << "CALIBRATING 100 Desktop-Pi-Teensy exchanges; motors remain stopped" << std::endl;
        while (running && ns()<deadline && job_clock.count<JobClock::required) {
            U64 now=ns();
            if (!job_clock.pending_id || now-job_clock.pending_sent>=120000000ULL) {
                Frame probe=frame(CAL_REQUEST);
                U64 stamp=ns(); probe.put(24,stamp,8);
                job_clock.sent(probe.sequence,stamp); send(probe);
                log.event("cal_sent",probe.sequence,stamp);
            }
            wait_io(udp.fd.value,-1,false,std::min(deadline,job_clock.pending_sent+120000000ULL));
            for(int i=0;i<128;++i) {
                std::array<uint8_t,2048> bytes{}; sockaddr_in from{};
                auto n=udp.receive(bytes.data(),bytes.size(),from);
                if(n<0) break;
                U64 received=ns(); Frame reply;
                if(!same(from,udp.peer)||!reply.parse(bytes.data(),size_t(n))||reply.session!=sid ||
                   !job_clock.receive(reply,received)) { ++log.counts[REJECTED]; continue; }
                U64 id=reply.get(24,8);
                log.event("cal_teensy_rx",id,reply.get(40,8));
                log.event("cal_teensy_tx",id,reply.get(48,8));
                log.event("cal_received",id,received);
            }
        }
        job_clock.finished=ns();
        require(running && job_clock.count==JobClock::required,
                "Startup calibration incomplete; no ARM sent. Requires Pi CAL_REPLY and Teensy TIME_SYNC firmware");
        job_clock.finish(ns());
        std::cout << std::fixed << std::setprecision(3)
                  << "CLOCK offset (Teensy - Desktop) " << job_clock.offset/1e6
                  << " ms; uncertainty +/- " << std::max(job_clock.offset-job_clock.lower,
                                                        job_clock.upper-job_clock.offset)/1e6
                  << " ms; min RTT " << job_clock.best_rtt/1e6 << " ms; calibration "
                  << (job_clock.finished-job_clock.started)/1e9 << " s" << std::endl;
        log.event("cal_ready",job_clock.reference,0,int(job_clock.count));
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
            } else if (f.kind == SYNC_REPLY) {
                U64 id = f.get(24, 8), d1 = f.get(32, 8), p2 = f.get(40, 8), p3 = f.get(48, 8);
                if (!sync_history.consume(id, d1, p2, p3, received_at)) {
                    ++log.counts[REJECTED];
                    continue;
                }
                log.event("sync_pi_rx", id, p2);
                log.event("sync_pi_tx", id, p3);
                log.event("sync_received", id, received_at);
            } else if (f.kind == SENSOR) {
                U64 id = f.get(24, 8);
                if (f.get(32, 8) > f.get(40, 8) || f.get(48, 2) > 1023 || f.get(50, 2) > 1023 ||
                    f.get(52, 2) > 1023 || f.get(58, 8) > f.get(66, 8) || !f.get(74, 8) || id <= newest_seen) {
                    ++log.counts[REJECTED];
                    continue;
                }
                if (newest_seen)
                    log.counts[SEQUENCE_GAPS] += id - newest_seen - 1;
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
        : c(std::move(conf)), decision(c.calibration, c.speed, c.n_sigma),
          log(c.output, "operator"), udp(c.bind, c.port, c.peer, c.peer_port) {
        c.polarity = decision.polarity;
        handshake = hello(HELLO, sid, ++seq, c);
    }
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
            calibrate();
            if(c.timing){
                combined_clock=compose_clock(gpio_clock,network_clock,job_clock);
                save_calibrations();
                std::cout<<"COMBINED clock uncertainty +/- "<<number(member(combined_clock,"estimated_uncertainty_ns"))/1e6<<" ms (conditional GPIO allowance); motors still stopped"<<std::endl;
            }else{
                save_calibrations();
                std::cout<<"Pi-Teensy GPIO timing measurement skipped; motors still stopped"<<std::endl;
            }
            flush_job_queues(udp,log,sid,"before_hello");
            // Calibration is before HELLO/ARM and does not consume drive duration.
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
                        decision.update(raw);
                        const U64 computed_at = ns();
                        log.metric(COMPUTE, (computed_at - tick) / 1e6);
                        log.event("target_computed", sample, computed_at, decision.left, decision.right);
                        auto f = command(sid, ++seq, sample, decision.left, decision.right);
                        send(f);
                        ++log.counts[COMMANDS];
                        if (decision.left || decision.right)
                            ++log.counts[NONZERO_TARGETS];
                        if (last_completion)
                            log.metric(INTERVAL, (completed - last_completion) / 1e3);
                        last_completion = completed;
                        log.metric(ADC, (completed - newest.get(32, 8)) / 1e3);
                        log.metric(DELIVERY, (newest.get(66, 8) - newest.get(58, 8)) / 1e6);
                        log.event("device_sample", newest.get(74, 8), completed, raw[0], raw[2]);
                        log.event("decision", sample, f.sequence, decision.left, decision.right);
                    }
                }
                if (welcomed && ns() >= next_sync) {
                    auto probe = frame(SYNC_REQUEST);
                    U64 stamp = ns();
                    probe.put(24, stamp, 8);
                    sync_history.remember(probe.sequence, stamp);
                    send(probe);
                    log.event("sync_sent", probe.sequence, stamp);
                    next_sync = stamp + 250000000ULL;
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
        log.finish(c, false, stopped, error);
        job_clock.save(log.path);
        save_calibrations(error);
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
