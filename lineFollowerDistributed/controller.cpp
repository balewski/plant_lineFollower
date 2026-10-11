#include "uart.hpp"
#include <iterator>
#include "pi_job.hpp"
#include "queue_reset.hpp"
#include "motor_padding.hpp"
namespace lf {
class Controller {
    Config c;
    Capture log;
    UDP udp;
    std::unique_ptr<UART> uart;
    Policy policy;
    bool session_exists = false, enabled = false, stop_due = false;
    Frame handshake;
    U64 job_session=0, arm_request=0, last_network_id=0;
    unsigned network_probes=0, end_to_end_replies=0;
    bool prepared=false, job_ran=false;
    GpioClock gpio;
    Json gpio_pre, gpio_post;
    std::string phase="waiting", job_error;
    void save_gpio_job() {
        Json record(Object{{"schema",jnum(1)},{"kind",Json(std::string("integrated_gpio_job"))},
          {"job_session",Json(std::to_string(job_session))},{"phase",Json(phase)},
          {"complete",Json(!gpio_pre.is<picojson::null>()&&!gpio_post.is<picojson::null>()&&job_error.empty())},
          {"error",job_error.empty()?Json():Json(job_error)},
          {"pi_boot_id",Json(read_file("/proc/sys/kernel/random/boot_id"))},
          {"job_path",Json(log.path.string())},{"pre",gpio_pre},{"post",gpio_post}});
        if(!gpio_pre.is<picojson::null>()&&!gpio_post.is<picojson::null>()){
            const double a=number(member(gpio_pre,"reference_pi_monotonic_ns"));
            const double b=number(member(gpio_post,"reference_pi_monotonic_ns"));
            if(b>a)record.get<Object>()["pre_post_drift_ppm"]=jnum(
                (number(member(gpio_post,"nominal_offset_at_reference_us"))-
                 number(member(gpio_pre,"nominal_offset_at_reference_us")))*1e9/(b-a));
        }
        atomic_json(log.path/"pi_teensy_clock.json",record);
        atomic_json(c.output/"pi_teensy_clock.latest.json",record);
        // The familiar robot0/out path is a convenience copy for this installation.
        if(fs::weakly_canonical(c.root).filename()=="lineFollowerController")
            atomic_json(fs::weakly_canonical(c.root).parent_path()/"out/pi_teensy_clock.latest.json",record);
    }
    void send_init() {
        InitFiles files;
        std::ifstream uptime("/proc/uptime");
        require(bool(uptime >> files.uptime_s) && files.uptime_s > 0, "Cannot read Pi uptime");
        files.photo = read_file(c.calibration);
        auto optional = [](const fs::path &path) {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return std::string();
            return std::string(std::istreambuf_iterator<char>(in), {});
        };
        fs::path dir = fs::path(c.calibration).parent_path();
        files.boot = optional(dir / "boot.json");
        files.clock = optional(dir / "clock_calib.json");
        auto bytes = pack_init(files);
        require(udp.send(bytes.data(), bytes.size()), "Cannot send initialization");
        std::cout << "INIT sent photo3 " << files.photo.size() << " bytes, boot " << files.boot.size()
                  << " bytes, clock " << files.clock.size() << " bytes, uptime " << files.uptime_s
                  << " s" << std::endl;
    }
    void job_ready() {
        send_init();
        Frame ready(JOB_READY,job_session,++txseq);gpio.put(ready);send(ready);
    }
    void prepare_job() {
        std::cout<<"WAITING for Desktop job; motors disabled"<<std::endl;
        flush_job_queues(udp,log,0,"startup");
        const U64 deadline=ns()+60000000000ULL;
        while(running&&ns()<deadline&&!job_session){
            wait_io(udp.fd.value,-1,false,ns()+50000000);
            std::array<uint8_t,2048> bytes{};sockaddr_in from{};
            auto count=udp.receive(bytes.data(),bytes.size(),from);Frame f;
            if(count<0)continue;
            if(!same(from,udp.peer)||!f.parse(bytes.data(),size_t(count))||f.kind!=JOB_REQUEST){reject();continue;}
            job_session=f.session;
        }
        require(running&&job_session,"No Desktop job request before timeout");
        if(c.timing){
            phase="gpio_pre";save_gpio_job();log.event("gpio_pre_start",job_session);
            uart.reset();gpio_pre=collect_gpio(c,log.path,"pre");gpio.load(gpio_pre);
            save_gpio_job();log.event("gpio_pre_done",job_session,gpio.reference);
            uart=std::make_unique<UART>(c.serial,c.serial_timeout);uart->verify();
            gpio.check_anchor(uart->clock.device,uart->clock.host,uart->clock.received);
            require(uart->job_led,"Install JOB_LED firmware before integrated calibration");
        }
        phase="desktop_calibration";save_gpio_job();
        U64 old_timeout=uart->timeout;uart->timeout=3000000000ULL;
        require(uart->exchange("JOB CAL")=="OK","Cannot signal Desktop calibration");
        uart->timeout=old_timeout;
        flush_job_queues(udp,log,job_session,"before_ready");
        prepared=true;job_ready();
        log.event("desktop_cal_ready",job_session);
    }
    bool cal_pending = false;
    U64 cal_session = 0, cal_sequence = 0, cal_desktop_send = 0;

    U64 session = 0, txseq = 0, sample_id = (random64() & 0x0000ffffffffffffULL) << 12,
        ready_end = 0, stop_request = 0, ack_cmd = 0, ack_sample = 0, sent = 0, acked = 0,
        last_sample_time = 0, last_arrival = 0, last_device_seq = 0, last_stream_receive = 0,
        last_device_drop = 0, next_clock = 0, last_sync = 0;
    std::array<U64, 256> retired{};
    struct WireSample { U64 id=0; size_t bytes=0; };
    std::array<WireSample,512> wire_samples{};
    size_t retire_pos = 0;
    int applied_l = 0, applied_r = 0, last_state = -1, last_reason = -1;
    void send(Frame f) {
        U64 stamp = ns();
        bool ok = udp.send(f.bytes.data(), f.size);
        log.event("udp_tx", f.sequence, stamp, f.bytes[5], ok);
        if (!ok) ++log.counts[SEND_ERRORS];
    }
    Frame frame(Kind k) {
        return Frame(k, session, ++txseq);
    }
    void ack(Code code, U64 seq) {
        auto f = frame(ACK);
        f.put(24, code, 1);
        f.put(25, policy.state, 1);
        f.put(26, seq, 8);
        send(f);
    }
    void reject() {
        ++log.counts[REJECTED];
    }
    void status() {
        if (!session_exists)
            return;
        auto f = frame(STATUS);
        f.put(24, policy.state, 1);
        f.put(25, policy.reason, 1);
        f.put(26, enabled, 1);
        f.put(27, ack_cmd, 8);
        f.put(35, ack_sample, 8);
        f.put(43, sent, 8);
        f.put(51, acked, 8);
        f.put(59, uint16_t(applied_l), 2);
        f.put(61, uint16_t(applied_r), 2);
        f.put(63, log.counts[MISSED], 4);
        f.put(67, log.counts[REJECTED], 4);
        f.put(71, log.counts[ACCEPTED], 4);
        send(f);
    }
    void state_update(U64 now) {
        if (!session_exists)
            return;
        policy.tick(now);
        if (policy.state == READY && now >= ready_end)
            policy.stop(EXPIRED);
        if (last_state != policy.state || last_reason != policy.reason) {
            last_state = policy.state;
            last_reason = policy.reason;
            log.event("state", session, 0, policy.state, policy.reason);
            if (policy.state == STOPPED || policy.state == FAULT)
                stop_due = true;
            status();
        }
    }
    void receive() {
        for (int i = 0; i < 64; i++) {
            std::array<uint8_t, 2048> raw{};
            sockaddr_in from{};
            auto n = udp.receive(raw.data(), raw.size(), from);
            if (n < 0)
                break;
            const U64 received_at = ns();
            Frame f;
            if (!same(from, udp.peer) || !f.parse(raw.data(), size_t(n))) {
                reject();
                continue;
            }
            U64 now = received_at;
            log.event("udp_rx", f.sequence, received_at, f.kind, int(n));
            if(!c.simulation && (f.session!=job_session || !prepared)){reject();continue;}
            if(f.kind==JOB_REQUEST){if(!session_exists)job_ready();else reject();continue;}
            if(f.kind==SYNC_REQUEST && !session_exists && !c.simulation){
                if(!f.get(24,8)||f.sequence<=last_network_id){reject();continue;}
                last_network_id=f.sequence;++network_probes;
                Frame out(SYNC_REPLY,job_session,++txseq);
                out.put(24,f.sequence,8);out.put(32,f.get(24,8),8);
                out.put(40,received_at,8);out.put(48,ns(),8);send(out);continue;
            }
            if (f.kind == CAL_REQUEST) {
                if(!c.simulation && network_probes<100){reject();continue;}
                // Only before arming / after a completed stop, and only from the
                // configured peer. One bounded request may be pending on UART.
                if (!uart->time_sync || cal_pending || uart->job == CAL_CLOCK_JOB ||
                    stop_due || (session_exists && policy.state != STOPPED) ||
                    !f.get(24, 8) || f.get(32, 8) || f.get(40, 8) || f.get(48, 8)) {
                    reject();
                    continue;
                }
                cal_session = f.session;
                cal_sequence = f.sequence;
                cal_desktop_send = f.get(24, 8);
                cal_pending = true;
                log.event("cal_request", cal_sequence, cal_desktop_send);
                continue;
            }
            if (f.kind == HELLO && (cal_pending || uart->job == CAL_CLOCK_JOB)) {
                reject();
                continue;
            }
            if (f.kind == HELLO) {
                if(!c.simulation && uart->job!=NO_JOB){reject();continue;}
                if (session_exists && f.session == session) {
                    if ((policy.state == READY || policy.state == ACTIVE) &&
                        same_hello(f, handshake)) {
                        auto out = handshake;
                        out.kind = WELCOME;
                        out.bytes[5] = WELCOME;
                        out.put(16, ++txseq, 8);
                        send(out);
                    } else
                        ack(REJECT, f.sequence);
                    continue;
                }
                if (std::find(retired.begin(), retired.end(), f.session) != retired.end() ||
                    (session_exists && (policy.state == READY || policy.state == ACTIVE ||
                                        stop_due || uart->job != NO_JOB))) {
                    reject();
                    continue;
                }
                auto expected = hello(HELLO, f.session, f.sequence, c);
                auto duration = f.get(30, 4);
                bool mode = f.get(34, 1);
                if (f.get(24, 2) != U64(c.rate) || f.get(26, 2) != U64(c.freshness) ||
                    !f.get(28, 2) || f.get(28, 2) > U64(c.max_pwm) || duration < 1 ||
                    duration > U64(c.duration * 1000) || f.get(34, 1) > 1 || (mode && !c.motors) ||
                    f.get(35, 1) != expected.get(35, 1) ||
                    !std::equal(f.bytes.begin() + 36, f.bytes.begin() + 68,
                                expected.bytes.begin() + 36)) {
                    reject();
                    continue;
                }
                if (session_exists) {
                    retired[retire_pos++ % retired.size()] = session;
                }
                if(!c.simulation){
                    require(!session_exists,"One job per controller launch; recalibration required");
                    flush_job_queues(udp,log,job_session,"before_welcome");
                    uart->refresh_clock();
                    char stream[32];std::snprintf(stream,sizeof stream,"STREAM %d",c.rate);
                    require(uart->exchange(stream)=="OK","Cannot start calibrated ADC stream");
                    last_stream_receive=ns();phase="ready_to_arm";
                }
                session = f.session;
                session_exists = true;
                handshake = f;
                policy = Policy(c.freshness, int(f.get(28, 2)), duration);
                enabled = mode && c.motors;
                ready_end = now + 5000000000ULL;
                last_state = last_reason = -1;
                ack_cmd = ack_sample = sent = acked = 0;
                applied_l = applied_r = 0;
                last_arrival = 0;
                auto out = f;
                out.bytes[5] = WELCOME;
                out.put(16, ++txseq, 8);
                send(out);
                log.event("hello", session, 0, enabled);
                continue;
            }
            if (!session_exists || f.session != session) {
                reject();
                continue;
            }
            if (f.kind == SYNC_REQUEST) {
                if (!f.get(24, 8) || (last_sync && now - last_sync < 50000000ULL)) {
                    reject();
                    continue;
                }
                last_sync = now;
                auto out = frame(SYNC_REPLY);
                out.put(24, f.sequence, 8);
                out.put(32, f.get(24, 8), 8);
                out.put(40, now, 8); // Pi receipt, before any reply construction.
                out.put(48, ns(), 8); // Pi timestamp immediately before sending.
                send(out);
            } else if (f.kind == ARM) {
                bool ok = policy.arm(f.sequence, f.get(24, 8), now);
                if(ok && !c.simulation){
                    if(!job_ran){arm_request=f.sequence;job_ran=true;phase="running";}
                    else if(!arm_request)ack(OK,f.sequence);
                }else ack(ok ? OK : REJECT, f.sequence);
                if (!ok)
                    reject();
            } else if (f.kind == COMMAND) {
                auto sample = f.get(24, 8);
                int l = f.signed16(32), r = f.signed16(34);
                if (last_arrival)
                    log.metric(ARRIVAL, (now - last_arrival) / 1e6);
                last_arrival = now;
                auto when = policy.completion(sample);
                if (when && now >= when)
                    log.metric(AGE, (now - when) / 1e6);
                bool pending = policy.has_pending;
                if (policy.accept(f.sequence, sample, l, r, now)) {
                    ++log.counts[ACCEPTED];
                    if (pending)
                        ++log.counts[REPLACED];
                    log.event("command", f.sequence, sample, l, r);
                } else
                    reject();
            } else if (f.kind == STOP) {
                policy.stop(REQUESTED);
                stop_due = true;
                stop_request = f.sequence;
                log.event("stop_request", f.sequence);
            } else
                reject();
        }
    }
    void start(const char *line, Job j, U64 now, Target t = {}) {
        log.event("uart_tx", t.command, t.sample, j, 0, line);
        if (j == MOTOR_JOB && enabled && (t.left || t.right))
            ++log.counts[NONZERO_WRITES];
        uart->start(line, j, now, t);
    }
    void completed(const Reply &r) {
        log.event("uart_rx", r.target.command, r.target.sample, r.job, 0, r.text.data());
        if (r.job == SAMPLE_JOB) {
            auto s = parse_sample(r.text.data());
            require(s.sequence > last_device_seq &&
                        (!last_sample_time || s.started > last_sample_time) &&
                        s.dropped >= last_device_drop,
                    "Reset/reordered Teensy stream");
            log.counts[MISSED] += s.sequence - last_device_seq - 1;
            sample_id += s.sequence - last_device_seq;
            last_device_seq = s.sequence;
            wire_samples[sample_id % wire_samples.size()] = {sample_id, std::strlen(r.text.data())+1};
            last_device_drop = s.dropped;
            ++log.counts[SAMPLES];
            log.metric(ADC, (s.completed - s.started) / 1e3);
            if (last_sample_time)
                log.metric(INTERVAL, (s.started - last_sample_time) / 1e3);
            last_sample_time = s.started;
            last_stream_receive = r.completed;
            U64 earliest = uart->clock.earliest(s.completed, r.completed);
            log.metric(DELIVERY, (r.completed - earliest) / 1e6);
            log.event("sample", sample_id, s.completed, s.adc[0], s.adc[1]);
            log.event("device_sample", s.sequence, s.started, s.adc[2], int(s.dropped));
            if (session_exists && (policy.state == READY || policy.state == ACTIVE)) {
                policy.sample(sample_id, earliest);
                auto f = frame(SENSOR);
                f.put(24, sample_id, 8);
                f.put(32, s.started, 8);
                f.put(40, s.completed, 8);
                for (int i = 0; i < 3; i++)
                    f.put(48 + 2 * i, s.adc[i], 2);
                f.put(54, log.counts[MISSED], 4);
                f.put(58, earliest, 8);
                f.put(66, r.completed, 8);
                f.put(74, s.sequence, 8);
                send(f);
            }
        } else if (r.job == LED_JOB) {
            require(!std::strcmp(r.text.data(),"OK"),"Job LED start failed");
            log.event("run_led_on",job_session);ack(OK,arm_request);arm_request=0;
        } else if (r.job == CAL_CLOCK_JOB) {
            auto times = parse_time_sync(r.text.data());
            Frame out(CAL_REPLY, cal_session, ++txseq);
            out.put(24, cal_sequence, 8);
            out.put(32, cal_desktop_send, 8);
            out.put(40, times[0], 8);
            out.put(48, times[1], 8);
            send(out);
            log.event("cal_teensy_rx", cal_sequence, times[0]);
            log.event("cal_teensy_tx", cal_sequence, times[1]);
            ++end_to_end_replies;
        } else if (r.job == CLOCK_JOB) {
            uart->clock.update(r.text.data(), r.started, r.completed);
            if(!c.simulation&&c.timing)gpio.check_anchor(uart->clock.device,r.started,r.completed);
            log.metric(LATENESS, (r.completed - r.started) / 1e6);
            log.event("clock_anchor", uart->clock.device, r.started);
            log.event("clock_anchor_end", uart->clock.device, r.completed);
        } else {
            if (r.job == MOTOR_JOB && uart->motor_timestamps) {
                U64 received_us = parse_motor_time(r.text.data());
                log.event("teensy_target_rx", r.target.command, received_us,
                          enabled ? r.target.left : 0, enabled ? r.target.right : 0);
            } else
                require(!std::strcmp(r.text.data(), "OK"), "Unexpected motor/stop reply");
            if (r.job == MOTOR_JOB) {
                ack_cmd = r.target.command;
                ack_sample = r.target.sample;
                sent = r.started;
                acked = r.completed;
                applied_l = enabled ? r.target.left : 0;
                applied_r = enabled ? r.target.right : 0;
                ++log.counts[ACKS];
                log.metric(RTT, (acked - r.target.completion) / 1e6);
                log.metric(UART_MS, (acked - sent) / 1e6);
                status();
            } else if (r.job == STOP_JOB) {
                applied_l = applied_r = 0;
                ++log.counts[STOP_ACKS];
                if (stop_request) {
                    ack(OK, stop_request);
                    stop_request = 0;
                }
                status();
            }
        }
    }

  public:
    explicit Controller(Config conf)
        : c(std::move(conf)), log(c.output, "controller"),
          udp(c.bind, c.port, c.peer, c.peer_port) {}
    int run() {
        std::string error;
        bool standby = false;
        try {
            uart = std::make_unique<UART>(c.serial, c.serial_timeout);
            uart->verify();
            if(!c.simulation){require(uart->job_led,"Install firmware revision 15 with JOB_LED");prepare_job();}
            log.event("clock_anchor", uart->clock.device, uart->clock.host);
            log.event("clock_anchor_end", uart->clock.device, uart->clock.received);
            char stream[32];
            std::snprintf(stream, sizeof stream, "STREAM %d", c.rate);
            if(c.simulation)require(uart->exchange(stream) == "OK", "Cannot start Teensy timer");
            log.event("verified", 0, 0, c.rate, 0, "Teensy master clock");
            U64 now = ns(), end = now + (c.simulation?U64(c.duration*1e9):30000000000ULL), print = now, previous = now;
            bool run_deadline_set=c.simulation;
            next_clock = now + 1000000000ULL;
            last_stream_receive = now;
            const U64 stream_timeout =
                std::max<U64>(U64(c.serial_timeout) * 1000000, 3 * (1000000000ULL / c.rate));
            std::cout << "READY " << c.bind << ':' << c.port << " rate=" << c.rate
                      << " motors=" << c.motors << " output=" << log.path << std::endl;
            while (running && (now = ns()) < end) {
                log.metric(LOOP, (now - previous) / 1e6);
                previous = now;
                state_update(now);
                Reply r;
                for (int i = 0; i < 64 && uart->pump(ns(), r); ++i)
                    completed(r);
                if(c.simulation||session_exists)require(ns() - last_stream_receive < stream_timeout, "Teensy stream timed out");
                receive();
                now = ns();
                state_update(now);
                if(!run_deadline_set && job_ran){end=now+U64(c.duration*1e9);run_deadline_set=true;}
                if(!c.simulation && session_exists && (policy.state==STOPPED||policy.state==FAULT) &&
                   !stop_due && uart->job==NO_JOB)break;
                if (uart->job == NO_JOB) {
                    if (stop_due) {
                        stop_due = false;
                        start("STOPALL", STOP_JOB, now);
                    } else if (arm_request) {
                        char led[48];std::snprintf(led,sizeof led,"JOB RUN %llu",(unsigned long long)handshake.get(30,4));
                        start(led,LED_JOB,now);
                    } else if (cal_pending && now < next_clock) {
                        cal_pending = false;
                        start("TIMESYNC 00000000000000000000 00000000000000000000", CAL_CLOCK_JOB, now);
                    } else if (now >= next_clock) {
                        next_clock = now + 1000000000ULL;
                        start("CLOCK", CLOCK_JOB, now);
                    } else if (session_exists) {
                        Target t;
                        if (policy.take(now, t)) {
                            char line[32];
                            motor_line(line, sizeof line, t.left, t.right, c.polarity, enabled);
                            if (uart->motor_timestamps) {
                                char timed[64];
                                if (!std::strncmp(line, "MOTORS ", 7))
                                    std::snprintf(timed, sizeof timed, "MOTORST%s", line + 6);
                                else {
                                    require(!std::strcmp(line, "STOPALL"), "Unexpected motor line");
                                    std::snprintf(timed, sizeof timed, "MOTORST 0 0");
                                }
                                if(c.match_motor_wire_size) {
                                    const auto &w=wire_samples[t.sample % wire_samples.size()];
                                    require(w.id==t.sample,"Missing originating sample wire length");
                                    const auto original=std::strlen(timed)+1;
                                    pad_motor_line(timed,sizeof timed,w.bytes);
                                    log.event("motor_wire_padding",t.command,t.sample,original,w.bytes);
                                }
                                start(timed, MOTOR_JOB, now, t);
                            } else
                                start(line, MOTOR_JOB, now, t);
                        }
                    }
                }
                if (now >= print) {
                    print = now + 1000000000ULL;
                    status();
                } // Console I/O is outside the timing loop.
                U64 wake = now + 2000000;
                if (wake <= now)
                    wake = now + 1000000;
                if (session_exists && policy.state == ACTIVE)
                    wake = std::min(wake, policy.expiry);
                wait_io(udp.fd.value, uart->fd.value, uart->tx_pos < uart->tx_size, wake);
            }
        } catch (const std::exception &e) {
            error = e.what();
            std::cerr << "ERROR " << error << std::endl;
            if(job_session && !job_ran){Frame failed(JOB_FAILED,job_session,++txseq);failed.put(24,1,8);send(failed);}
            if (session_exists)
                policy.stop(SERIAL);
        }
        if(!c.simulation && !job_ran && error.empty())error="Job ended before arming";
        if (session_exists && policy.state != STOPPED && policy.state != FAULT) {
            policy.stop(SHUTDOWN);
        }
        if (uart) {
            standby = uart->emergency_stop();
        }
        log.event("final_stop", 0, 0, standby);
        applied_l = applied_r = 0;
        status();
        uart.reset();
        if(!c.simulation && job_session){
            job_error=error;
            try{
                if(c.timing && standby && gpio.ready && running){
                    phase="gpio_post";save_gpio_job();
                    gpio_post=collect_gpio(c,log.path,"post");GpioClock post;post.load(gpio_post);
                    require(post.first_teensy>gpio.last_teensy && post.reference>gpio.reference,
                            "Clock reset between pre/post GPIO calibrations");
                    gpio.check_anchor(post.first_teensy,
                        U64(number(member(member(gpio_post,"records").get<picojson::array>().front(),"request_send_ns"))),
                        U64(number(member(member(gpio_post,"records").get<picojson::array>().front(),"reply_receive_ns"))));
                    phase="complete";
                }else if(!c.timing){phase="timing-skipped";
                }else {phase="incomplete";if(job_error.empty())job_error="Post calibration skipped after interruption or unconfirmed stop";}
            }catch(const std::exception &e){phase="failed";job_error=e.what();if(error.empty())error=job_error;}
            save_gpio_job();
        }
        log.finish(c, standby, false, error);
        return error.empty() && standby ? 0 : 1;
    }
};
} // namespace lf
int main(int argc, char **argv) {
    try {
        lf::signals();
        lf::Controller c(lf::config(argc, argv, true));
        return c.run();
    } catch (const std::exception &e) {
        std::cerr << "ERROR " << e.what() << '\n';
        return 1;
    }
}
