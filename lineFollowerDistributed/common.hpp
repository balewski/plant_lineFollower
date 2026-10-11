#pragma once
#include "vendor/picojson.h"
#include "vendor/picosha2.h"
#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
namespace lf {
using U64 = uint64_t;
namespace fs = std::filesystem;
using Json = picojson::value;
using Object = picojson::object;
inline U64 ns() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return U64(t.tv_sec) * 1000000000ULL + t.tv_nsec;
}
inline double cpu() {
    timespec t{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
inline volatile sig_atomic_t running = 1;
inline void stop_signal(int) {
    running = 0;
}
inline void signals() {
    struct sigaction a {};
    a.sa_handler = stop_signal;
    sigemptyset(&a.sa_mask);
    for (int s : {SIGINT, SIGTERM, SIGHUP, SIGTSTP})
        sigaction(s, &a, nullptr);
}
inline void require(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
inline void require(bool ok, const std::string &why) {
    if (!ok)
        throw std::runtime_error(why);
}
inline std::string read_file(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    require(bool(f), "Cannot read " + p.string());
    return {std::istreambuf_iterator<char>(f), {}};
}
inline Json json_read(const fs::path &p) {
    Json j;
    auto e = picojson::parse(j, read_file(p));
    require(e.empty(), "Invalid JSON: " + e);
    return j;
}
inline const Json &member(const Json &j, const std::string &k) {
    require(j.is<Object>() && j.contains(k), "Missing JSON field " + k);
    return j.get(k);
}
inline double number(const Json &j) {
    require(j.is<double>() && std::isfinite(j.get<double>()), "Expected finite number");
    return j.get<double>();
}
inline std::string string(const Json &j) {
    require(j.is<std::string>(), "Expected string");
    return j.get<std::string>();
}
inline bool boolean(const Json &j) {
    require(j.is<bool>(), "Expected boolean");
    return j.get<bool>();
}
inline Json jnum(double v) {
    return Json(v);
}
inline std::array<uint8_t, 32> hash_bytes(const std::string &hex) {
    require(hex.size() == 64, "Invalid SHA256");
    std::array<uint8_t, 32> a{};
    for (size_t i = 0; i < 32; i++) {
        auto digit = [](char c) {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            throw std::runtime_error("Invalid SHA256 hex");
        };
        a[i] = uint8_t(16 * digit(hex[2 * i]) + digit(hex[2 * i + 1]));
    }
    return a;
}
inline std::string sha256(const fs::path &p) {
    auto bytes = read_file(p);
    return picosha2::hash256_hex_string(bytes);
}
inline U64 random64() {
    int f = open("/dev/urandom", O_RDONLY);
    require(f >= 0, "Cannot open random source");
    U64 n = 0;
    auto got = read(f, &n, sizeof n);
    close(f);
    require(got == sizeof n, "Cannot read random source");
    return n ? n : 1;
}
struct FD {
    int value = -1;
    explicit FD(int v = -1) : value(v) {}
    ~FD() {
        if (value >= 0)
            close(value);
    }
    FD(const FD &) = delete;
    FD &operator=(const FD &) = delete;
};
inline sockaddr_in address(const std::string &ip, int port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(uint16_t(port));
    require(port > 0 && port < 65536 && inet_pton(AF_INET, ip.c_str(), &a.sin_addr) == 1,
            "Invalid IP/port");
    return a;
}
inline bool same(const sockaddr_in &a, const sockaddr_in &b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}
// Resolve once at startup; a UDP connect selects a route without sending packets.
inline std::string route_local_ipv4(const std::string &peer_ip, int peer_port) {
    auto destination = address(peer_ip, peer_port);
    const uint32_t ip = ntohl(destination.sin_addr.s_addr);
    require(ip != 0 && ip != 0xffffffffU && (ip & 0xf0000000U) != 0xe0000000U,
            "Automatic bind requires a unicast peer IPv4 address");
    FD probe(socket(AF_INET, SOCK_DGRAM, 0));
    require(probe.value >= 0, "Cannot create route probe socket");
    require(connect(probe.value, reinterpret_cast<sockaddr *>(&destination),
                    sizeof destination) == 0,
            "Cannot find route to Pi: " + std::string(strerror(errno)));
    sockaddr_in local{};
    socklen_t length = sizeof local;
    require(getsockname(probe.value, reinterpret_cast<sockaddr *>(&local), &length) == 0 &&
                local.sin_family == AF_INET && local.sin_addr.s_addr != INADDR_ANY,
            "Route to Pi has no local IPv4 address");
    std::array<char, INET_ADDRSTRLEN> text{};
    require(inet_ntop(AF_INET, &local.sin_addr, text.data(), text.size()) != nullptr,
            "Cannot format selected local address");
    return text.data();
}
struct UDP {
    FD fd;
    sockaddr_in peer{};
    UDP(const std::string &bind_ip, int port, const std::string &peer_ip, int peer_port)
        : fd(socket(AF_INET, SOCK_DGRAM, 0)), peer(address(peer_ip, peer_port)) {
        require(fd.value >= 0, "Cannot create UDP socket");
        require(fcntl(fd.value, F_SETFL, O_NONBLOCK) == 0, "Cannot set nonblocking UDP");
        auto a = address(bind_ip, port);
        require(bind(fd.value, reinterpret_cast<sockaddr *>(&a), sizeof a) == 0,
                "UDP bind failed: " + std::string(strerror(errno)));
    }
    bool send(const void *p, size_t n) {
        return sendto(fd.value, p, n, 0, reinterpret_cast<sockaddr *>(&peer), sizeof peer) ==
               ssize_t(n);
    }
    ssize_t receive(void *p, size_t n, sockaddr_in &from) {
        socklen_t l = sizeof from;
        auto k = recvfrom(fd.value, p, n, 0, reinterpret_cast<sockaddr *>(&from), &l);
        if (k < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            throw std::runtime_error("UDP receive failed");
        return k;
    }
};
struct Config {
    std::string bind, peer, serial = "/dev/serial0", polarity = "rr", hash;
    int port = 5000, peer_port = 5001, rate = 100, freshness = 100, max_pwm = 128,
        serial_timeout = 80;
    double duration = 120, speed = .5, n_sigma = 2.5;
    bool motors = false, simulation = false, match_motor_wire_size = false, timing = false,
        clock_matched = false;
    double pi_uptime_s = 0, latency_uncertainty_ms = 0;
    fs::path root, output, calibration, boot, clock_file;
    std::string clock_note;
};
inline double argument_number(const std::string &s) {
    size_t pos = 0;
    double x = std::stod(s, &pos);
    require(pos == s.size() && std::isfinite(x), "Invalid numeric argument");
    return x;
}
inline int integer(double x) {
    require(x == std::floor(x) && x >= 0 && x <= 1000000000, "Invalid integer");
    return int(x);
}
inline Config config(int argc, char **argv, bool pi) {
    Config c;
    c.root = fs::absolute(argv[0]).parent_path();
    auto j = json_read(c.root / "config.json");
    c.bind = string(member(j, "bind"));
    c.peer = string(member(j, "peer"));
    c.port = integer(number(member(j, "port")));
    c.peer_port = integer(number(member(j, "peer_port")));
    c.rate = integer(number(member(j, "rate")));
    c.freshness = integer(number(member(j, "freshness_ms")));
    c.max_pwm = integer(number(member(j, "max_pwm")));
    c.duration = number(member(j, "duration"));
    c.output = c.root / "out";
    // The Pi controller reads the calibration file and sends those bytes to the
    // desktop operator in the UDP initialization message.
    if (pi)
        c.calibration = "/home/pi/plant_lineFollower/config/photo3_calib.json";
    else
        c.calibration = c.root / "calibration.json";
    if (pi) {
        c.serial = string(member(j, "serial"));
        c.serial_timeout = integer(number(member(j, "serial_timeout_ms")));
        c.polarity = string(member(j, "polarity"));
    } else {
        c.speed = number(member(j, "speed"));
        c.n_sigma = number(member(j, "n_sigma"));
    }
    for (int i = 1; i < argc; i++) {
        std::string k = argv[i];
        if(k=="--simulation"){c.simulation=true;continue;}
        if(k=="--match-motor-wire-size"){require(pi,"UART padding is Pi-only");c.match_motor_wire_size=true;continue;}
        if (k == "--enable-motors") {
            c.motors = true;
            continue;
        }
        if (k == "--help") {
            std::cout
                << "C++ line follower; motors DISABLED by default.\n--bind IP|auto --peer IP --port N "
                   "--peer-port N --rate 1..100 --freshness-ms 20..1000\n--duration SECONDS "
                   "--max-pwm 1..255 --output DIR --calibration FILE\nPi: --serial DEVICE "
                   "--serial-timeout-ms N --polarity ff|fr|rf|rr --match-motor-wire-size\nDesktop: --speed 0..1 --n-sigma "
                   "N\nThe Pi controller sends boot.json, clock_calib.json, and photo3_calib.json over UDP at job start.\n"
                   "Real motor output requires --enable-motors on BOTH programs.\n"
                   "Clock delay is measured by onDesktop/runClocksCalib. A robot run uses that file only when its hash8 matches boot.json.\n";
            std::exit(0);
        }
        require(i + 1 < argc, "Missing value for " + k);
        std::string v = argv[++i];
        if (k == "--bind")
            c.bind = v;
        else if (k == "--peer")
            c.peer = v;
        else if (k == "--serial" && pi)
            c.serial = v;
        else if (k == "--polarity" && pi)
            c.polarity = v;
        else if (k == "--output")
            c.output = v;
        else if (k == "--calibration")
            c.calibration = v;
        else if (k == "--boot" && !pi)
            c.boot = v;
        else if (k == "--clock" && !pi)
            c.clock_file = v;
        else if (k == "--pi-uptime-s" && !pi)
            c.pi_uptime_s = argument_number(v);
        else if (k == "--port")
            c.port = integer(argument_number(v));
        else if (k == "--peer-port")
            c.peer_port = integer(argument_number(v));
        else if (k == "--rate")
            c.rate = integer(argument_number(v));
        else if (k == "--freshness-ms")
            c.freshness = integer(argument_number(v));
        else if (k == "--max-pwm")
            c.max_pwm = integer(argument_number(v));
        else if (k == "--duration")
            c.duration = argument_number(v);
        else if (k == "--serial-timeout-ms" && pi)
            c.serial_timeout = integer(argument_number(v));
        else if (k == "--speed" && !pi)
            c.speed = argument_number(v);
        else if (k == "--n-sigma" && !pi)
            c.n_sigma = argument_number(v);
        else
            throw std::runtime_error("Unknown option " + k);
    }
    require(c.rate >= 1 && c.rate <= 100 && c.freshness >= 20 && c.freshness <= 1000 &&
                c.max_pwm >= 1 && c.max_pwm <= 255 && c.duration > 0 && c.duration <= 3600,
            "Invalid rate/freshness/PWM/duration");
    require(c.polarity == "ff" || c.polarity == "fr" || c.polarity == "rf" || c.polarity == "rr",
            "Invalid polarity");
    if (pi) {
        require(c.serial_timeout >= 5 && c.serial_timeout <= c.freshness,
                "Serial timeout must be 5..freshness ms");
    } else {
        require(c.speed >= 0 && c.speed <= 1 && c.n_sigma >= 0 &&
                    std::nearbyint(255 * c.speed) <= c.max_pwm,
                "Invalid speed/noise/PWM");
    }
    require(!c.simulation || (pi && c.bind=="127.0.0.1" && c.peer=="127.0.0.1" &&
            (c.serial.rfind("/dev/pts/",0)==0 || c.serial.rfind("/dev/ttys",0)==0)),
            "Simulation bypass requires loopback and a virtual serial device");
    if (pi) {
        c.hash = sha256(c.calibration);
        hash_bytes(c.hash);
    }
    if (c.bind == "auto") {
        require(!pi, "Automatic bind is a Desktop option; use the Pi SSH launcher");
        c.bind = route_local_ipv4(c.peer, c.peer_port);
        std::cout << "NETWORK Desktop " << c.bind << ':' << c.port
                  << " -> Pi " << c.peer << ':' << c.peer_port << std::endl;
    }
    return c;
}
inline Object config_json(const Config &c) {
    return {{"bind", Json(c.bind)},
            {"peer", Json(c.peer)},
            {"rate", jnum(c.rate)},
            {"freshness_ms", jnum(c.freshness)},
            {"serial_timeout_ms", jnum(c.serial_timeout)},
            {"duration", jnum(c.duration)},
            {"enable_motors", Json(c.motors)},
            {"match_motor_wire_size", Json(c.match_motor_wire_size)},
            {"calibration_sha256", Json(c.hash)},
            {"clock_matched", Json(c.clock_matched)},
            {"latency_uncertainty_ms", jnum(c.latency_uncertainty_ms)},
            {"clock_note", Json(c.clock_note)}};
}
// All event and metric storage is allocated before the loop. No formatting/I/O in it.
enum Metric {
    LOOP,
    ADC,
    INTERVAL,
    AGE,
    RTT,
    UART_MS,
    ARRIVAL,
    COMPUTE,
    LATENESS,
    DELIVERY,
    TRANSMIT,
    METRIC_COUNT
};
inline constexpr const char *metric_names[] = {"loop_gap_ms",
                                               "teensy_adc_ms",
                                               "sample_interval_ms",
                                               "command_age_upper_bound_ms",
                                               "sample_to_ack_upper_bound_ms",
                                               "motor_uart_ms",
                                               "command_arrival_interval_ms",
                                               "compute_ms",
                                               "clock_query_rtt_ms",
                                               "delivery_age_upper_bound_ms",
                                               "transmit_latency_ms"};
enum Count {
    SAMPLES,
    MISSED,
    ACCEPTED,
    ACKS,
    REPLACED,
    REJECTED,
    NONZERO_WRITES,
    STOP_ACKS,
    SEND_ERRORS,
    SENSOR_PACKETS,
    COMMANDS,
    COALESCED,
    SEQUENCE_GAPS,
    NONZERO_TARGETS,
    COUNT_SIZE
};
inline constexpr const char *count_names[] = {"samples",
                                              "missing_teensy_samples",
                                              "commands_accepted",
                                              "command_acks",
                                              "pending_commands_replaced",
                                              "rejected",
                                              "nonzero_motor_writes",
                                              "stop_acks",
                                              "send_errors",
                                              "sensor_packets",
                                              "commands_sent",
                                              "coalesced_samples",
                                              "sensor_sequence_gaps",
                                              "nonzero_targets"};
struct Event {
    U64 t = 0, a = 0, b = 0;
    int c = 0, d = 0;
    char name[20]{}, detail[96]{};
};
struct Capture {
    static constexpr size_t capacity = 60000, metric_capacity = 30000;
    std::unique_ptr<Event[]> rows{new Event[capacity]};
    std::array<std::vector<double>, METRIC_COUNT> metrics;
    std::array<U64, METRIC_COUNT> metric_total{};
    std::array<double, METRIC_COUNT> metric_max{};
    std::array<U64, COUNT_SIZE> counts{};
    U64 total = 0, start = ns();
    double cpu_start = cpu();
    fs::path path;
    Capture(const fs::path &base, const char *role) {
        for (auto &v : metrics)
            v.resize(metric_capacity);
        char stamp[40];
        time_t t = time(nullptr);
        tm wall{};
        localtime_r(&t, &wall);
        strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", &wall);
        path = base / (std::string(stamp) + "_cpp_" + role + "_" + std::to_string(getpid()));
        require(fs::create_directories(path), "Output directory already exists");
    }
    void event(const char *name, U64 a = 0, U64 b = 0, int c = 0, int d = 0,
               const char *detail = "") {
        auto &r = rows[total++ % capacity];
        r.t = ns();
        r.a = a;
        r.b = b;
        r.c = c;
        r.d = d;
        std::snprintf(r.name, sizeof r.name, "%s", name);
        std::snprintf(r.detail, sizeof r.detail, "%s", detail);
    }
    void metric(Metric m, double value) {
        metric_max[m] = std::max(metric_max[m], value);
        metrics[m][metric_total[m]++ % metric_capacity] = value;
    }
    void finish(const Config &c, bool standby, bool stop_ack, const std::string &error) {
        double elapsed = (ns() - start) / 1e9, used = cpu() - cpu_start;
        Object out{{"implementation", Json(std::string("C++17"))},
                   {"elapsed_s", jnum(elapsed)},
                   {"cpu_s", jnum(used)},
                   {"motors_enabled", Json(c.motors)},
                   {"standby_confirmed", Json(standby)},
                   {"stop_acknowledged", Json(stop_ack)},
                   {"error", error.empty() ? Json() : Json(error)},
                   {"config", Json(config_json(c))}};
        Object counters;
        for (int i = 0; i < COUNT_SIZE; i++)
            counters[count_names[i]] = jnum(counts[i]);
        counters["audit_overwritten"] = jnum(total > capacity ? total - capacity : 0);
        out["counters"] = Json(counters);
        Object ms;
        for (int m = 0; m < METRIC_COUNT; m++) {
            auto n = std::min<U64>(metric_total[m], metric_capacity);
            auto &v = metrics[m];
            v.resize(n);
            std::sort(v.begin(), v.end());
            Object d{{"count", jnum(n)},
                     {"total_count", jnum(metric_total[m])},
                     {"max_all", jnum(metric_max[m])}};
            if (n) {
                auto q = [&](double p) { return v[size_t(std::ceil(p * n)) - 1]; };
                d["median"] = jnum(q(.5));
                d["p99"] = jnum(q(.99));
                d["p999"] = jnum(q(.999));
                d["max"] = jnum(v.back());
            }
            ms[metric_names[m]] = Json(d);
        }
        out["metrics"] = Json(ms);
        std::ofstream f(path / "events.csv");
        f << "t_ns,event,a,b,c,d,detail\n";
        for (U64 i = total > capacity ? total - capacity : 0; i < total; i++) {
            const auto &r = rows[i % capacity];
            f << r.t << ',' << r.name << ',' << r.a << ',' << r.b << ',' << r.c << ',' << r.d << ','
              << r.detail << '\n';
        }
        require(bool(f), "Cannot write events");
        std::ofstream s(path / "summary.json");
        s << Json(out).serialize(true) << '\n';
        require(bool(s), "Cannot write summary");
        std::cout << "REPORT " << (path / "summary.json") << std::endl;
    }
};
inline void wait_io(int udp, int serial = -1, bool write_serial = false, U64 until = 0) {
    pollfd f[2] = {{udp, POLLIN, 0}, {serial, short(POLLIN | (write_serial ? POLLOUT : 0)), 0}};
    U64 now = ns();
    int timeout =
        until ? int(std::min<U64>(20, until > now ? (until - now + 999999) / 1000000 : 0)) : 2;
    int n = poll(f, serial < 0 ? 1 : 2, timeout);
    if (n < 0 && errno != EINTR)
        throw std::runtime_error("poll failed");
    if (n > 0)
        for (int i = 0; i < (serial < 0 ? 1 : 2); i++)
            if (f[i].revents & (POLLERR | POLLHUP | POLLNVAL))
                throw std::runtime_error("I/O disconnected");
}
} // namespace lf
