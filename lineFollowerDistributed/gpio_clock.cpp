// Standalone Pi/Teensy shared-edge diagnostic. No controller dependency.
// g++ -O2 -std=c++17 -Wall -Wextra -Wpedantic -pthread measure_pi_teensy_clock.cpp -o measure_pi_teensy_clock
#include <linux/gpio.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <sys/random.h>
#include <termios.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
namespace fs=std::filesystem;
using U=uint64_t;
volatile sig_atomic_t interrupted=0;
void signal_stop(int){interrupted=1;}
void need(bool ok,const std::string &why){if(!ok)throw std::runtime_error(why);}
U now(){timespec t{};need(!clock_gettime(CLOCK_MONOTONIC,&t),"clock_gettime");return U(t.tv_sec)*1000000000+t.tv_nsec;}
std::string quote(const std::string&s){
 std::ostringstream o;o<<'"';
 for(unsigned char c:s){if(c=='"'||c=='\\')o<<'\\'<<c;else if(c<32)o<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<unsigned(c)<<std::dec;else o<<c;}o<<'"';return o.str();
}
U number(const std::string&s,int base=10){
 need(!s.empty(),"Empty integer");for(unsigned char c:s)need(base==16?bool(std::isxdigit(c)):bool(std::isdigit(c)),"Invalid integer: "+s);
 size_t n;auto v=std::stoull(s,&n,base);need(n==s.size(),"Trailing integer text");return v;
}
std::string hex(U x){std::ostringstream s;s<<std::hex<<std::setw(16)<<std::setfill('0')<<x;return s.str();}
struct FD{int n=-1;~FD(){if(n>=0)close(n);}FD()=default;FD(const FD&)=delete;FD&operator=(const FD&)=delete;};
struct Serial {
 FD fd;std::string buffered;
 explicit Serial(const std::string&path){
  fd.n=open(path.c_str(),O_RDWR|O_NOCTTY|O_NONBLOCK|O_CLOEXEC);need(fd.n>=0,"Open UART: "+std::string(strerror(errno)));
  need(!flock(fd.n,LOCK_EX|LOCK_NB),"UART locked");need(!ioctl(fd.n,TIOCEXCL),"Cannot reserve UART");
  termios t{};need(!tcgetattr(fd.n,&t),"Read UART settings");cfmakeraw(&t);cfsetispeed(&t,B115200);cfsetospeed(&t,B115200);
  t.c_cflag|=CLOCAL|CREAD;t.c_cflag&=~(CSTOPB|CRTSCTS);t.c_cc[VMIN]=0;t.c_cc[VTIME]=0;
  need(!tcsetattr(fd.n,TCSANOW,&t),"Set UART settings");tcflush(fd.n,TCIOFLUSH);
 }
 ~Serial(){if(fd.n>=0)ioctl(fd.n,TIOCNXCL);}
 void send(const std::string&line){
  std::string s=line+'\n';size_t i=0;U end=now()+500000000;
  while(i<s.size()){auto n=write(fd.n,s.data()+i,s.size()-i);if(n>0)i+=size_t(n);else need(n>=0||errno==EAGAIN||errno==EINTR,"UART write failed");
   if(i<s.size()){need(now()<end,"UART write timeout");pollfd p{fd.n,POLLOUT,0};poll(&p,1,5);}}
 }
 std::vector<std::string> lines(){
  std::vector<std::string>v;char b[512];
  for(;;){auto n=read(fd.n,b,sizeof b);if(n>0)buffered.append(b,size_t(n));else{need(n>=0||errno==EAGAIN||errno==EINTR,"UART read failed");break;}need(buffered.size()<8192,"UART input overflow");}
  size_t i;while((i=buffered.find('\n'))!=std::string::npos){std::string s=buffered.substr(0,i);buffered.erase(0,i+1);if(!s.empty()&&s.back()=='\r')s.pop_back();v.push_back(s);}return v;
 }
 std::string exchange(const std::string&s){
  send(s);U end=now()+1000000000;
  while(now()<end){auto v=lines();if(!v.empty()){need(v.size()==1,"Extra UART reply");return v[0];}pollfd p{fd.n,POLLIN,0};poll(&p,1,10);}
  throw std::runtime_error("UART timeout: "+s);
 }
 bool stop(){
  try{tcflush(fd.n,TCIFLUSH);buffered.clear();bool a=exchange("EDGE STOP")=="OK";bool b=exchange("STOPALL")=="OK";return a&&b&&exchange("STATUSAB")=="MOTORS left=0; right=0; standby=1";}catch(...){return false;}
 }
};
struct Edge{U timestamp=0,read_at=0;unsigned seq=0,line_seq=0;bool high=false;};
struct GPIO {
 FD chip,line;unsigned offset;bool output;
 GPIO(const std::string&path,unsigned pin,bool out=false,bool both=false):offset(pin),output(out){
  chip.n=open(path.c_str(),O_RDONLY|O_CLOEXEC);need(chip.n>=0,"Open GPIO chip: "+std::string(strerror(errno)));
  gpio_v2_line_info info{};info.offset=pin;need(!ioctl(chip.n,GPIO_V2_GET_LINEINFO_IOCTL,&info),"Read GPIO line info");
  need(std::string(info.name)=="GPIO"+std::to_string(pin),"Wrong GPIO chip/line");
  gpio_v2_line_request r{};r.offsets[0]=pin;r.num_lines=1;r.event_buffer_size=256;
  std::strncpy(r.consumer,"pi-teensy-clock",sizeof(r.consumer)-1);
  // Default event clock is CLOCK_MONOTONIC; no debounce.
  r.config.flags=out?GPIO_V2_LINE_FLAG_OUTPUT:GPIO_V2_LINE_FLAG_INPUT|GPIO_V2_LINE_FLAG_EDGE_RISING|GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN;
  if(both)r.config.flags|=GPIO_V2_LINE_FLAG_EDGE_FALLING;
  need(!ioctl(chip.n,GPIO_V2_GET_LINE_IOCTL,&r),"Request GPIO"+std::to_string(pin)+": "+strerror(errno));line.n=r.fd;
  int flags=fcntl(line.n,F_GETFL);need(flags>=0&&!fcntl(line.n,F_SETFL,flags|O_NONBLOCK),"GPIO nonblocking");if(out)set(false);
 }
 ~GPIO(){if(output&&line.n>=0){gpio_v2_line_values v{};v.mask=1;ioctl(line.n,GPIO_V2_LINE_SET_VALUES_IOCTL,&v);}}
 void set(bool high){gpio_v2_line_values v{};v.mask=1;v.bits=high;need(!ioctl(line.n,GPIO_V2_LINE_SET_VALUES_IOCTL,&v),"Set Pi LED");}
 bool high(){gpio_v2_line_values v{};v.mask=1;need(!ioctl(line.n,GPIO_V2_LINE_GET_VALUES_IOCTL,&v),"Read GPIO");return v.bits&1;}
 std::vector<Edge> events(){
  std::vector<Edge>out;gpio_v2_line_event ev[16];
  for(;;){ssize_t n=read(line.n,ev,sizeof ev);U received=now();
   if(n<0&&(errno==EAGAIN||errno==EINTR))break;
   need(n>0&&n%sizeof(ev[0])==0,"Malformed GPIO event");
   for(size_t i=0;i<size_t(n)/sizeof(ev[0]);++i){need(ev[i].offset==offset&&(ev[i].id==GPIO_V2_LINE_EVENT_RISING_EDGE||ev[i].id==GPIO_V2_LINE_EVENT_FALLING_EDGE),"Unexpected GPIO event");out.push_back({ev[i].timestamp_ns,received,ev[i].seqno,ev[i].line_seqno,ev[i].id==GPIO_V2_LINE_EVENT_RISING_EDGE});}}
  return out;
 }
};
struct Pulse{U before=0,after=0;};
Pulse parse_reply(const std::string&s,U session,U seq){
 std::istringstream in(s);std::vector<std::string>t;std::string x;while(in>>x)t.push_back(x);
 need(t.size()==6&&t[0]=="EDGE","Invalid pulse reply: "+s);need(number(t[1],16)==session&&number(t[2])==seq,"Pulse session/sequence mismatch");
 Pulse p{number(t[3]),number(t[4])};need(p.before>0&&p.after>=p.before&&p.after-p.before<=1000&&p.after<=U(INT64_MAX)/1000,"Invalid Teensy timestamp bracket");
 need(t[5]=="1","Teensy pin20 readback not HIGH");return p;
}
struct Row{
 U seq=0,sent=0,reply_at=0;std::vector<Edge>edges;std::vector<std::string>replies;Pulse pulse;bool valid=false;std::string error;
 long double offset()const{return static_cast<long double>(edges.at(0).timestamp)/1000-(static_cast<long double>(pulse.before)+pulse.after)/2;}
};
void validate(Row&r,unsigned expected,U previous){
 need(r.edges.size()==1,"Missing/multiple GPIO edges; refusing shifted pairing");auto e=r.edges[0];
 need(e.high&&e.seq==expected&&e.line_seq==expected,"GPIO sequence gap/overflow");
 need(e.timestamp>=r.sent&&e.timestamp<=e.read_at,"GPIO timestamp outside request / wrong clock");
 need(r.pulse.before>previous,"Teensy clock reset or reordered pulse");need(r.replies.size()==1,"Missing/duplicate UART record");
}
long double percentile(std::vector<long double>v,long double q){
 need(!v.empty(),"No samples");std::sort(v.begin(),v.end());return v[std::max<size_t>(1,size_t(std::ceil(q*v.size())))-1];
}
struct Stats{size_t n=0;U reference=0;long double fit=0,ppm=0,baseline=0;std::vector<long double>offsets,changes,brackets,wakeup,residuals;};
Stats analyze(const std::vector<Row>&rows){
 Stats s;std::vector<const Row*>v;for(auto&r:rows)if(r.valid)v.push_back(&r);s.n=v.size();if(v.empty())return s;s.reference=v[v.size()/2]->edges[0].timestamp;s.baseline=v.front()->offset();
 long double mx=0,my=0;
 for(auto r:v){mx+=(static_cast<long double>(r->edges[0].timestamp)-s.reference)/1e9L;my+=r->offset();s.offsets.push_back(r->offset());s.changes.push_back(r->offset()-s.baseline);s.brackets.push_back(r->pulse.after-r->pulse.before);s.wakeup.push_back((r->edges[0].read_at-r->edges[0].timestamp)/1000.L);}
 mx/=v.size();my/=v.size();long double xx=0,xy=0;
 for(auto r:v){auto x=(static_cast<long double>(r->edges[0].timestamp)-s.reference)/1e9L-mx;xx+=x*x;xy+=x*(r->offset()-my);}
 s.ppm=xx?xy/xx:0;s.fit=my-s.ppm*mx;
 for(auto r:v){auto x=(static_cast<long double>(r->edges[0].timestamp)-s.reference)/1e9L;s.residuals.push_back(r->offset()-(s.fit+s.ppm*x));}return s;
}
void stats_json(std::ostream&o,const std::vector<long double>&v){
 if(v.empty()){o<<"null";return;}auto m=std::minmax_element(v.begin(),v.end());
 o<<"{\"min\":"<<*m.first<<",\"q1\":"<<percentile(v,.25)<<",\"median\":"<<percentile(v,.5)<<",\"q3\":"<<percentile(v,.75)<<",\"p99\":"<<percentile(v,.99)<<",\"max\":"<<*m.second<<",\"range\":"<<*m.second-*m.first<<'}';
}
void atomic_write(const fs::path&p,const std::string&s){
 fs::create_directories(p.parent_path());fs::path t=p.string()+".tmp."+std::to_string(getpid());{std::ofstream f(t);f<<s;f.close();need(bool(f),"Cannot write "+t.string());}fs::rename(t,p);
}
struct Load{
 std::atomic<bool> stop{false};std::thread t;
 void start(){t=std::thread([this]{volatile U x=1;while(!stop.load(std::memory_order_relaxed))for(int i=0;i<10000;++i)x=x*1664525+1013904223;});}
 ~Load(){stop=true;if(t.joinable())t.join();}
};
struct Flash{Edge edge;U pi_led_write_before=0,pi_led_write_after=0;};
void startup_blink(Serial&uart,const std::string&chip,U session,unsigned blink_count,std::vector<Flash>&flashes){
 GPIO led(chip,24,true);GPIO signal(chip,23,false,true);
 need(!signal.high()&&signal.events().empty(),"Unexpected GPIO23 activity before startup flashes");
 uart.send("EDGE BLINK "+hex(session)+" "+std::to_string(blink_count));U deadline=now()+U(blink_count+2)*1000000000;bool reply=false;
 while(now()<deadline&&!interrupted){
  for(auto&e:signal.events()){
   flashes.push_back({e,now(),0});led.set(e.high);flashes.back().pi_led_write_after=now();
   size_t k=flashes.size();need(k<=2*blink_count&&e.seq==k&&e.line_seq==k&&e.high==(k%2==1),"Startup flash edge loss/extra edge");
  }
  for(auto&s:uart.lines()){need(!reply&&s=="EDGE BLINKED "+hex(session),"Unexpected startup flash reply: "+s);reply=true;}
  if(reply&&flashes.size()==2*blink_count){need(!signal.high(),"Timing pin did not return LOW");return;}
  pollfd p[2]={{signal.line.n,POLLIN,0},{uart.fd.n,POLLIN,0}};poll(p,2,2);
 }
 throw std::runtime_error(interrupted?"Interrupted during LED flashes":"Startup flashes missing: check Teensy20 -> 5k -> Pi GPIO23");
}
void self_test(){
 int n=0;auto check=[&](bool x){++n;need(x,"Self-test failed");};auto bad=[&](auto fn){bool failed=false;try{fn();}catch(...){failed=true;}check(failed);};
 auto p=parse_reply("EDGE 0000000000000001 2 4294967295 4294967296 1",1,2);check(p.after==4294967296ULL);check(p.before==4294967295ULL);
 for(const char*s:{"EDGE 2 2 100 101 1","EDGE 1 3 100 101 1","EDGE 1 2 101 100 1","EDGE 1 2 100 101 0","EDGE 1 2 -1 101 1","EDGE 1 2 100 101 1 extra"})bad([&]{parse_reply(s,1,2);});
 Row r;r.sent=1000;r.edges={{2000,3000,1,1,true}};r.pulse={100,101};r.replies={"reply"};validate(r,1,99);++n;
 bad([&]{validate(r,2,99);});bad([&]{validate(r,1,101);});r.edges.push_back(r.edges[0]);bad([&]{validate(r,1,99);});r.edges.clear();bad([&]{validate(r,1,99);});
 std::vector<Row>rows;
 for(U i=0;i<100;++i){Row a;a.valid=true;a.edges={{100000000000ULL+i*1000000000ULL,0,0,0,true}};a.pulse.before=100000000+i*1000000+500+2*i;a.pulse.after=a.pulse.before;rows.push_back(a);}
 auto s=analyze(rows);check(s.baseline==-500 && s.changes.front()==0 && s.changes.back()==-198);check(std::abs(s.ppm+2)<1e-9);check(std::abs(s.fit+600)<1e-9);check(std::abs(percentile(s.residuals,.99))<1e-9);
 for(auto&a:rows)a.edges[0].timestamp+=50000;
 auto biased=analyze(rows);check(std::abs(biased.fit+550)<1e-9);check(quote("a\nb")=="\"a\\u000ab\"");
 std::cout<<"PASS "<<n<<" parser, pairing, wrap and offset/drift checks\n";
}
int main(int argc,char**argv){
 unsigned count=200,blink_count=2;double rate=20;bool cpu_load=false;std::string chip="/dev/gpiochip0",serial="/dev/serial0",label="idle";
 fs::path root=fs::canonical("/proc/self/exe").parent_path(),output=root/"out/pi_teensy_clock.latest.json";
 try{
  for(int i=1;i<argc;++i){
   std::string a=argv[i];if(a=="--self-test"){self_test();return 0;}
   if(a=="--help"){std::cout<<"Usage: ./measure_pi_teensy_clock [--samples 200] [--rate 20] [--blinks 0..3] [--cpu-load] [--label idle] [--output out/pi_teensy_clock.latest.json] [--chip /dev/gpiochip0] [--serial /dev/serial0]\nGPIO23/header16 INPUT <- 5 kohm <- Teensy20 OUTPUT; common ground.\nStartup: Teensy21 and Pi GPIO24/header18 LEDs flash twice, 0.5s on/off. Motors disabled.\n";return 0;}
   if(a=="--cpu-load"){cpu_load=true;continue;}need(i+1<argc,"Missing option value");std::string v=argv[++i];
   if(a=="--blinks"){auto k=number(v);need(k<=3,"Blinks must be 0..3");blink_count=k;}
   else if(a=="--samples"){auto k=number(v);need(k>=2&&k<=1000000,"Samples must be 2..1000000");count=k;}
   else if(a=="--rate"){size_t n;rate=std::stod(v,&n);need(n==v.size()&&std::isfinite(rate)&&rate>=1&&rate<=20,"Rate must be 1..20 Hz");}
   else if(a=="--chip")chip=v;else if(a=="--serial")serial=v;else if(a=="--label")label=v;
   else if(a=="--output")output=fs::path(v).is_absolute()?fs::path(v):root/v;else throw std::runtime_error("Unknown option: "+a);
  }
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 2;}
 for(int s:{SIGINT,SIGTERM,SIGHUP})signal(s,signal_stop);
 U started=now(),session=0;std::vector<Row>rows;std::vector<Flash>flashes;std::string error,firmware;bool stopped=false;std::unique_ptr<Serial>uart;
 try{
  need(getrandom(&session,sizeof session,0)==sizeof session&&session,"Random session ID failed");
  uart=std::make_unique<Serial>(serial);firmware=uart->exchange("VERSION");
  need(firmware.find("GPIO_EDGE_CLOCK")!=std::string::npos,"Install GPIO_EDGE_CLOCK Teensy firmware revision 14 or later");
  need(firmware.find("CLOCK_PIN=20")!=std::string::npos,"Install firmware revision 16: timing output is Teensy pin 20");
  need(uart->exchange("EDGE STOP")=="OK","Cannot stop old diagnostic");
  need(uart->exchange("STREAM 0")=="OK","Cannot stop ADC stream");
  need(uart->exchange("STOPALL")=="OK","Cannot disable motors");
  need(uart->exchange("EDGE BEGIN "+hex(session))=="EDGE READY "+hex(session),"Cannot start identified edge session");
  std::cout<<"Startup: "<<blink_count<<" synchronized LED flashes (0.5 s ON / 0.5 s OFF)."<<std::endl;
  if(blink_count)startup_blink(*uart,chip,session,blink_count,flashes);
  GPIO gpio(chip,23);need(!gpio.high()&&gpio.events().empty(),"Unexpected GPIO23 activity before samples");
  Load load;if(cpu_load)load.start();U previous=0;unsigned expected=1;
  std::cout<<"Pi GPIO23/header16 <- 5 kohm <- Teensy20; CLOCK_MONOTONIC\n"<<count<<" pulses, up to "<<rate<<" Hz; motors disabled; session "<<hex(session)<<std::endl;
  for(unsigned seq=1;seq<=count&&!interrupted;++seq){
   rows.emplace_back();Row&r=rows.back();r.seq=seq;
   try{
    auto stray=gpio.events();if(!stray.empty()){r.edges=stray;throw std::runtime_error("Edge outside request window; aborting pairing");}
    auto extra=uart->lines();if(!extra.empty()){r.replies=extra;throw std::runtime_error("UART record outside request window");}
    need(!gpio.high(),"GPIO23 stayed HIGH between pulses");
    r.sent=now();uart->send("EDGE "+hex(session)+" "+std::to_string(seq));U end=r.sent+std::max<U>(300000000,U(1e9/rate)+100000000),slot=r.sent+U(1e9/rate);
    while(!interrupted){
     auto edges=gpio.events();r.edges.insert(r.edges.end(),edges.begin(),edges.end());need(r.edges.size()<=1,"Duplicate GPIO edge; aborting pairing");
     for(auto&s:uart->lines()){r.replies.push_back(s);r.reply_at=now();need(r.replies.size()==1,"Duplicate UART reply; aborting pairing");r.pulse=parse_reply(s,session,seq);}
     U t=now();if(!r.edges.empty()&&!r.replies.empty()&&t>=slot)break;
     need(t<end,"Missing GPIO edge or UART record; check GPIO23--5k--Teensy20 wiring");
     pollfd f[2]={{gpio.line.n,POLLIN,0},{uart->fd.n,POLLIN,0}};int n=poll(f,2,2);need(n>=0||errno==EINTR,"poll failed");
     for(auto &p:f)need(!(p.revents&(POLLERR|POLLHUP|POLLNVAL)),"GPIO/UART disconnected");
    }
    need(!interrupted,"Interrupted");validate(r,expected,previous);r.valid=true;previous=r.pulse.after;++expected;
    if(seq==1)std::cout<<"Baseline Pi - Teensy (first pulse): "<<std::fixed<<std::setprecision(9)<<r.offset()/1e6L<<" s\nProgress shows change from this baseline (either sign is possible)."<<std::endl;
    if(seq%20==0||seq==count)std::cout<<"  "<<seq<<'/'<<count<<" change="<<std::fixed<<std::setprecision(3)<<std::showpos<<(r.offset()-rows.front().offset())<<std::noshowpos<<" us; bracket="<<r.pulse.after-r.pulse.before<<" us"<<std::endl;
   }catch(const std::exception&e){r.error=e.what();throw;}
  }
  need(!interrupted,"Interrupted");
 }catch(const std::exception&e){error=e.what();std::cerr<<"ERROR: "<<error<<std::endl;}
 if(uart)stopped=uart->stop();
 if(!stopped&&error.empty())error="Could not confirm final GPIO stop / motor standby";
 U finished=now();Stats s=analyze(rows);bool complete=error.empty()&&s.n==count&&stopped;
 try{
  std::time_t epoch=std::time(nullptr);std::tm tm{};localtime_r(&epoch,&tm);char date[32];std::strftime(date,sizeof date,"%Y%m%d_%H%M%S",&tm);
  fs::path archive=root/"out/raspberry_pi"/(std::string("pi_teensy_clock_")+date+"_"+std::to_string(getpid())+".json");fs::path csvpath=archive;csvpath.replace_extension(".csv");
  std::ostringstream o;o<<std::fixed<<std::setprecision(6);
  o<<"{\n\"schema\":2,\"complete\":"<<(complete?"true":"false")<<",\"error\":"<<(error.empty()?"null":quote(error))<<",\"label\":"<<quote(label)<<",\"cpu_load\":"<<(cpu_load?"true":"false")<<",\"requested_samples\":"<<count<<",\"valid_samples\":"<<s.n<<",\"requested_rate_hz\":"<<rate<<",\"elapsed_s\":"<<(finished-started)/1e9<<",\"stop_confirmed\":"<<(stopped?"true":"false")<<",\"session\":"<<quote(hex(session))<<",\n";
  o<<"\"firmware\":"<<quote(firmware)<<",\"pi_clock\":\"CLOCK_MONOTONIC\",\"pi_gpio\":23,\"pi_header_pin\":16,\"teensy_pin\":20,\"series_resistor_ohm\":5000,\"debounce_us\":0,\"timestamp_method\":\"Linux GPIO v2 kernel interrupt event; not userspace wakeup\",\n";
  o<<"\"offset_definition\":\"Pi monotonic microseconds minus Teensy microseconds at shared rising edge\",\"reference_pi_monotonic_ns\":"<<s.reference<<",\"nominal_offset_at_reference_us\":"<<(s.n?std::to_string(s.fit):"null")<<",\"fitted_drift_ppm\":"<<(s.n>1?std::to_string(s.ppm):"null")<<",\n";
  o<<"\"baseline_offset_us\":"<<(s.n?std::to_string(s.baseline):"null")<<",\"offset_change_from_first_us\":";stats_json(o,s.changes);o<<',';
  o<<"\"offset_us\":";stats_json(o,s.offsets);o<<",\"teensy_bracket_us\":";stats_json(o,s.brackets);o<<",\"userspace_delivery_delay_us\":";stats_json(o,s.wakeup);o<<",\"detrended_residual_us\":";stats_json(o,s.residuals);
  o<<",\n\"absolute_accuracy_bound_us\":null,\"assumed_unmeasured_latency_allowance_us\":100,\"latency_bias_correction_us\":0,\"sub_100us_accuracy_validated\":false,\"teensy_timestamp_resolution_us\":1,\"pi_interrupt_latency_calibrated\":false,";
  o<<"\"caveat\":\"Nominal estimate uses the requested assumption that unmeasured latency is below 100 us, with no bias correction. This is not a validated bound. Positive Pi interrupt latency biases Pi-minus-Teensy upward. Software write/readback brackets do not latch the physical edge; pad/wire/input delays and quantization remain. Drift fit includes changes in interrupt latency. UART arrival times are diagnostic only.\",\"archive_json\":"<<quote(archive.string())<<",\"raw_csv\":"<<quote(csvpath.string())<<",\n\"startup_flashes\":[";
  for(size_t i=0;i<flashes.size();++i){if(i)o<<',';auto&f=flashes[i];o<<"{\"high\":"<<(f.edge.high?"true":"false")<<",\"pi_gpio_edge_ns\":"<<f.edge.timestamp<<",\"pi_led_write_before_ns\":"<<f.pi_led_write_before<<",\"pi_led_write_after_ns\":"<<f.pi_led_write_after<<'}';}
  o<<"],\n\"records\":[\n";
  std::ostringstream csv;csv<<"session,sequence,valid,pi_request_send_ns,pi_reply_receive_ns,pi_gpio_edge_ns,pi_userspace_receive_ns,kernel_sequence,kernel_line_sequence,teensy_before_us,teensy_after_us,teensy_bracket_us,pi_minus_teensy_offset_us,offset_change_from_first_us,error\n"<<std::fixed<<std::setprecision(3);
  for(size_t i=0;i<rows.size();++i){
   const auto&r=rows[i];if(i)o<<",\n";
   o<<"{\"sequence\":"<<r.seq<<",\"valid\":"<<(r.valid?"true":"false")<<",\"error\":"<<(r.error.empty()?"null":quote(r.error))<<",\"request_send_ns\":"<<r.sent<<",\"reply_receive_ns\":"<<r.reply_at<<",\"teensy_before_us\":"<<r.pulse.before<<",\"teensy_after_us\":"<<r.pulse.after<<",\"gpio_events\":[";
   for(size_t j=0;j<r.edges.size();++j){auto&e=r.edges[j];if(j)o<<',';o<<"{\"timestamp_ns\":"<<e.timestamp<<",\"userspace_receive_ns\":"<<e.read_at<<",\"seqno\":"<<e.seq<<",\"line_seqno\":"<<e.line_seq<<'}';}
   o<<"],\"uart_records\":[";for(size_t j=0;j<r.replies.size();++j){if(j)o<<',';o<<quote(r.replies[j]);}o<<"]}";
   Edge e=r.edges.empty()?Edge{}:r.edges[0];
   csv<<hex(session)<<','<<r.seq<<','<<r.valid<<','<<r.sent<<','<<r.reply_at<<','<<e.timestamp<<','<<e.read_at<<','<<e.seq<<','<<e.line_seq<<','<<r.pulse.before<<','<<r.pulse.after<<','<<(r.pulse.after>=r.pulse.before?r.pulse.after-r.pulse.before:0)<<',';
   if(r.valid)csv<<r.offset();
   csv<<',';if(r.valid)csv<<r.offset()-s.baseline;
   csv<<','<<quote(r.error)<<'\n';
  }
  o<<"\n]}\n";atomic_write(archive,o.str());atomic_write(csvpath,csv.str());atomic_write(output,o.str());fs::path latest_csv=output;latest_csv.replace_extension(".csv");atomic_write(latest_csv,csv.str());
  if(s.n){
   auto range=*std::max_element(s.residuals.begin(),s.residuals.end())-*std::min_element(s.residuals.begin(),s.residuals.end());
   std::cout<<"\nPi - Teensy nominal offset at Pi t="<<std::fixed<<std::setprecision(6)<<s.reference/1e9L<<" s: "<<std::setprecision(9)<<s.fit/1e6L<<" s\nChange from first pulse Q1 / median / Q3: "<<std::setprecision(3)<<std::showpos<<percentile(s.changes,.25)<<" / "<<percentile(s.changes,.5)<<" / "<<percentile(s.changes,.75)<<std::noshowpos<<" us\nFitted drift (Pi - Teensy): "<<s.ppm<<" ppm; detrended residual range: "<<range<<" us\nTeensy bracket max: "<<*std::max_element(s.brackets.begin(),s.brackets.end())<<" us; userspace delivery median: "<<percentile(s.wakeup,.5)<<" us\n";
   if(range>100)std::cout<<"NOTE: measured detrended variation exceeds 100 us.\n";
  }
  std::cout<<"Valid edges: "<<s.n<<'/'<<count<<"; stop confirmed: "<<stopped<<"\nBias correction: 0; unmeasured latency assumed <100 us (not independently validated).\nJSON: "<<output<<"\nCSV: "<<latest_csv<<"\nArchive: "<<archive<<std::endl;
 }catch(const std::exception&e){std::cerr<<"Saving results failed: "<<e.what()<<'\n';return 1;}
 return complete?0:1;
}
