#pragma once
#include "integrated_clock.hpp"
namespace lf {
// Only call between jobs, with motors stopped and before HELLO/ARM.
// A drain clears queued datagrams, not packets still in flight; session IDs
// and command/sample freshness remain mandatory after this barrier.
inline U64 drain_job_udp(UDP &udp) {
    const U64 deadline=ns()+100000000ULL;
    U64 discarded=0;
    while(ns()<deadline && discarded<4096) {
        std::array<uint8_t,2048> bytes{};sockaddr_in from{};
        auto n=udp.receive(bytes.data(),bytes.size(),from);
        if(n>=0){++discarded;continue;}
        if(errno==EINTR)continue;
        return discarded; // receive only returns EAGAIN/EWOULDBLOCK here.
    }
    throw std::runtime_error("Job queue drain did not reach empty; refusing startup");
}
inline void flush_job_queues(UDP &udp,Capture &log,U64 session,const char *phase) {
    const U64 discarded=drain_job_udp(udp);
    log.event("job_queue_flush",session,discarded,0,0,phase);
    atomic_json(log.path/(std::string("queue_flush_")+phase+".json"),Json(Object{
        {"schema",jnum(1)},{"job_session",Json(std::to_string(session))},
        {"phase",Json(std::string(phase))},{"complete",Json(true)},
        {"udp_datagrams_discarded",jnum(discarded)},{"monotonic_ns",jnum(ns())},
        {"in_flight_protection",Json(std::string("session ID, sequence and sample freshness checks"))}}));
    std::cout<<"QUEUE RESET "<<phase<<": discarded "<<discarded<<" queued UDP packets"<<std::endl;
}
}
