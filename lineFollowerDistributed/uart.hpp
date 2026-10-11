#pragma once
#include "policy.hpp"
#include "stream.hpp"
#include <sys/file.h>
#include <sys/ioctl.h>
#include <termios.h>
namespace lf {
enum Job { NO_JOB, VERIFY, ADC_JOB, MOTOR_JOB, STOP_JOB, SAMPLE_JOB, CLOCK_JOB, CAL_CLOCK_JOB, LED_JOB };
inline std::array<int, 3> parse_adc(const char *s) {
    int a, b, c;
    char extra;
    require(std::sscanf(s, "ADC3 %d %d %d %c", &a, &b, &c, &extra) == 3 && a >= 0 && a <= 1023 &&
                b >= 0 && b <= 1023 && c >= 0 && c <= 1023,
            "Invalid ADC3 reply");
    return {a, b, c};
}
// Receipt means the complete command line reached the Teensy parser, before driveMotors.
inline U64 parse_motor_time(const char *s) {
    require(!std::strncmp(s, "OK T ", 5) && s[5] >= '0' && s[5] <= '9',
            "Invalid Teensy motor timestamp");
    errno = 0;
    char *end = nullptr;
    auto value = std::strtoull(s + 5, &end, 10);
    require(!errno && end && *end == '\0' && value > 0, "Invalid Teensy motor timestamp");
    return U64(value);
}
inline std::array<U64, 2> parse_time_sync(const char *s) {
    require(std::strlen(s) == 50 && !std::strncmp(s, "TIMESYNC ", 9) && s[29] == ' ',
            "Invalid Teensy calibration reply");
    std::array<U64, 2> result{};
    for (int field = 0; field < 2; ++field) {
        U64 value = 0;
        for (int i = 0; i < 20; ++i) {
            char c = s[9 + field * 21 + i];
            require(c >= '0' && c <= '9', "Invalid calibration timestamp digit");
            require(value <= (UINT64_MAX - U64(c - '0')) / 10, "Calibration timestamp overflow");
            value = value * 10 + U64(c - '0');
        }
        require(value > 0 && value <= UINT64_MAX / 1000, "Invalid calibration timestamp");
        result[field] = value;
    }
    require(result[1] >= result[0], "Reversed calibration timestamps");
    return result;
}
struct Reply {
    Job job = NO_JOB;
    U64 started = 0, completed = 0;
    Target target{};
    std::array<char, 512> text{};
};
struct UART {
    FD fd;
    U64 timeout, started = 0;
    Job job = NO_JOB;
    Target target{};
    std::array<char, 128> tx{};
    std::array<char, 2048> rx{};
    ClockMap clock;
    bool motor_timestamps = false, time_sync = false, job_led = false;
    size_t tx_size = 0, tx_pos = 0, rx_size = 0;
    UART(const std::string &path, int ms)
        : fd(open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK)), timeout(U64(ms) * 1000000) {
        require(fd.value >= 0, "Cannot open UART");
        require(flock(fd.value, LOCK_EX | LOCK_NB) == 0, "UART already in use");
        termios t{};
        require(tcgetattr(fd.value, &t) == 0, "Cannot get UART settings");
        cfmakeraw(&t);
        cfsetispeed(&t, B115200);
        cfsetospeed(&t, B115200);
        t.c_cflag = (t.c_cflag & ~(PARENB | CSTOPB | CSIZE | CRTSCTS)) | CS8 | CLOCAL | CREAD;
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;
        require(tcsetattr(fd.value, TCSANOW, &t) == 0, "Cannot set UART settings");
        tcflush(fd.value, TCIOFLUSH);
    }
    void start(const char *line, Job j, U64 now, Target t = {}) {
        require(job == NO_JOB, "Overlapping UART transaction");
        auto n = std::snprintf(tx.data(), tx.size(), "%s\n", line);
        require(n > 0 && size_t(n) < tx.size(), "UART line too long");
        job = j;
        started = now;
        target = t;
        tx_size = size_t(n);
        tx_pos = 0;
    }
    bool pump(U64 now, Reply &reply) {
        if (job != NO_JOB && now - started >= timeout)
            throw std::runtime_error("UART transaction timed out");
        if (tx_pos < tx_size && job == MOTOR_JOB && now >= target.expiry)
            throw std::runtime_error("Motor command expired before UART write");
        if (tx_pos < tx_size) {
            auto n = write(fd.value, tx.data() + tx_pos, tx_size - tx_pos);
            if (n > 0)
                tx_pos += size_t(n);
            else if (n < 0 && errno != EAGAIN && errno != EINTR)
                throw std::runtime_error("UART write failed");
        }
        auto end = std::find(rx.begin(), rx.begin() + rx_size, '\n');
        if (end == rx.begin() + rx_size) {
            auto n = read(fd.value, rx.data() + rx_size, rx.size() - rx_size);
            if (n > 0)
                rx_size += size_t(n);
            else if (n < 0 && errno != EAGAIN && errno != EINTR)
                throw std::runtime_error("UART read failed");
            end = std::find(rx.begin(), rx.begin() + rx_size, '\n');
            require(end != rx.begin() + rx_size || rx_size < rx.size(), "UART line too large");
            if (end == rx.begin() + rx_size)
                return false;
        }
        size_t consumed = size_t(end - rx.begin()) + 1, len = consumed - 1;
        if (len && rx[len - 1] == '\r')
            --len;
        require(len < reply.text.size(), "UART reply too large");
        reply = Reply{};
        reply.completed = ns();
        std::copy_n(rx.begin(), len, reply.text.begin());
        std::memmove(rx.data(), rx.data() + consumed, rx_size - consumed);
        rx_size -= consumed;
        if (len >= 2 && reply.text[0] == 'S' && reply.text[1] == ' ') {
            reply.job = SAMPLE_JOB;
        } else {
            require(job != NO_JOB && tx_pos == tx_size, "Unexpected UART reply");
            reply.job = job;
            reply.started = started;
            reply.target = target;
            job = NO_JOB;
            tx_size = tx_pos = 0;
        }
        return true;
    }
    std::string exchange(const char *line) {
        start(line, VERIFY, ns());
        Reply r;
        while (true) {
            if (pump(ns(), r) && r.job != SAMPLE_JOB)
                break;
            pollfd f{fd.value, short(POLLIN | (tx_pos < tx_size ? POLLOUT : 0)), 0};
            poll(&f, 1, 1);
        }
        return r.text.data();
    }
    void verify() {
        require(exchange("STREAM 0") == "OK", "Stream reset failed; revision 10 required");
        require(exchange("STOPALL") == "OK", "Initial stop failed");
        auto v = exchange("VERSION");
        motor_timestamps = v.find("MOTOR_TIME") != std::string::npos;
        time_sync = v.find("TIME_SYNC") != std::string::npos;
        job_led = v.find("JOB_LED") != std::string::npos;
        for (const char *s : {"HALO_TEENSY/1.0;", "board=Teensy 4.1", "UART1=115200,8N1",
                              "ADC3=25,26,27", "ADC_BITS=10", "MOTOR_AB", "STREAM1", "CLOCK"})
            require(v.find(s) != std::string::npos, "Unsupported firmware");
        require(exchange("STATUSAB") == "MOTORS left=0; right=0; standby=1",
                "Initial standby not confirmed");
        refresh_clock();
    }
    void refresh_clock() {
        // The map expires 2 s after this reply. Refresh it again before a stream.
        start("CLOCK", CLOCK_JOB, ns());
        Reply r;
        while (true) {
            if (pump(ns(), r) && r.job != SAMPLE_JOB)
                break;
            pollfd f{fd.value, POLLIN, 0};
            poll(&f, 1, 1);
        }
        clock.update(r.text.data(), r.started, r.completed);
    }
    bool emergency_stop() {
        job = NO_JOB;
        tcflush(fd.value, TCIOFLUSH);
        const char *msg = "\nSTOPALL\nSTREAM 0\nSTATUSAB\n";
        size_t pos = 0, len = strlen(msg), used = 0;
        char line[512]{};
        U64 end = ns() + 300000000;
        while (ns() < end) {
            if (pos < len) {
                auto n = write(fd.value, msg + pos, len - pos);
                if (n > 0)
                    pos += size_t(n);
            }
            char buffer[256];
            auto n = read(fd.value, buffer, sizeof buffer);
            for (ssize_t i = 0; i < n; i++) {
                if (buffer[i] == '\n') {
                    line[used] = 0;
                    if (!strcmp(line, "MOTORS left=0; right=0; standby=1"))
                        return true;
                    used = 0;
                } else if (buffer[i] != '\r') {
                    if (used + 1 < sizeof line)
                        line[used++] = buffer[i];
                    else
                        used = 0;
                }
            }
            pollfd f{fd.value, short(POLLIN | (pos < len ? POLLOUT : 0)), 0};
            poll(&f, 1, 2);
        }
        return false;
    }
};
} // namespace lf
