#pragma once
#include <cstring>
#include <stdexcept>
namespace lf {
// Length includes the LF added by UART::start. Leading zeros preserve the
// existing Teensy parseDuty semantics, including negative and zero targets.
inline void pad_motor_line(char *line, size_t capacity, size_t wire_bytes) {
    const size_t n=std::strlen(line);
    if(std::strncmp(line,"MOTORST ",8)!=0 || wire_bytes<n+1 || wire_bytes>64 || wire_bytes>capacity)
        throw std::runtime_error("Invalid motor padding length");
    char *digits=line+8;
    if(*digits=='-')++digits;
    if(*digits<'0'||*digits>'9')throw std::runtime_error("Invalid motor padding target");
    const size_t extra=wire_bytes-(n+1);
    std::memmove(digits+extra,digits,n-size_t(digits-line)+1);
    std::memset(digits,'0',extra);
}
}
