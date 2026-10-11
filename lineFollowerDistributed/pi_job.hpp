#pragma once
#include "integrated_clock.hpp"
#include <sys/wait.h>
namespace lf {
inline Json collect_gpio(const Config &c,const fs::path &job,const char *phase) {
    auto output=fs::absolute(job/(std::string("gpio_")+phase+".json"));
    auto exe=fs::absolute(c.root/"gpio_clock");
    auto serial=c.serial;auto label=std::string("job-")+phase;
    pid_t child=fork();require(child>=0,"Cannot fork GPIO calibration");
    if(!child){execl(exe.c_str(),exe.c_str(),"--samples","200","--blinks","3","--serial",serial.c_str(),"--label",label.c_str(),"--output",output.c_str(),nullptr);_exit(127);}
    int status=0;bool cancelled=false;U64 deadline=ns()+40000000000ULL,term=0;
    while(true){auto rc=waitpid(child,&status,WNOHANG);if(rc==child)break;require(rc>=0||errno==EINTR,"GPIO child wait failed");
      if((!running||ns()>deadline)&&!term){kill(child,SIGTERM);term=ns();cancelled=true;}
      if(term&&ns()-term>3000000000ULL)kill(child,SIGKILL);
      poll(nullptr,0,20);
    }
    require(!cancelled&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"GPIO calibration failed or interrupted");
    return json_read(output);
}
}
