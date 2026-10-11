// C++17, fixed-size UDP four-timestamp diagnostic. No robot/UART/GPIO access.
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <chrono>
#include <csignal>
#include <ctime>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <net/if.h>
#include <arpa/inet.h>
#ifdef __linux__
#include <linux/net_tstamp.h>
#include <linux/errqueue.h>
#else
#include <net/if_dl.h>
#endif
using I= int64_t; using U=uint64_t;
static volatile sig_atomic_t running=1;
void stop(int){running=0;}
I clockns(clockid_t c){timespec t{};if(clock_gettime(c,&t))throw std::runtime_error("clock_gettime");return I(t.tv_sec)*1000000000+t.tv_nsec;}
I now(){return clockns(CLOCK_MONOTONIC);}
void need(bool ok,const std::string&s){if(!ok)throw std::runtime_error(s+": "+strerror(errno));}
struct Map{I real=0,mono=0,error=INT64_MAX;};
Map mapping(){Map best;for(int i=0;i<5;i++){I a=now(),r=clockns(CLOCK_REALTIME),b=now();if((b-a)/2<best.error)best={r,a+(b-a)/2,(b-a+1)/2};}return best;}
I convert(I real,const Map&m){return real-m.real+m.mono;}
I map_error(I raw,const Map&m){return m.error+1000+I(std::ceil(std::abs(double(raw-m.real))*.002));}
// No native structs on wire; big-endian words avoid padding, alignment and ARMv6 ABI differences.
struct Packet{std::array<unsigned char,192>b{};U get(size_t i)const{U v=0;for(size_t k=0;k<8;k++)v=(v<<8)|b[i*8+k];return v;}void put(size_t i,U v){for(int k=7;k>=0;k--){b[i*8+k]=v&255;v>>=8;}}};
constexpr U MAGIC=0x434c4f434b504431ULL;
sockaddr_in addr(const std::string&ip,int port){sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(port);need(inet_pton(AF_INET,ip.c_str(),&a.sin_addr)==1,"address");return a;}
struct Rx{Packet p; sockaddr_in peer{}; I raw=0,mono=0,app=0,error=0;Map map;unsigned iface=0;bool stamp=false;};
struct Socket{
 int fd=-1;unsigned iface=0;bool txstamp=false;uint32_t txid=0;
 Socket(const std::string&ip,int port,const std::string&device,bool tx):iface(if_nametoindex(device.c_str())){
  need(iface!=0,"interface "+device);fd=socket(AF_INET,SOCK_DGRAM,0);need(fd>=0,"socket");int one=1;
#ifdef __linux__
  need(!setsockopt(fd,SOL_SOCKET,SO_BINDTODEVICE,device.c_str(),device.size()+1),"bind device");
  need(!setsockopt(fd,IPPROTO_IP,IP_PKTINFO,&one,sizeof one),"IP_PKTINFO");
  int flags=SOF_TIMESTAMPING_RX_SOFTWARE|SOF_TIMESTAMPING_SOFTWARE;
  if(tx)flags|=SOF_TIMESTAMPING_TX_SOFTWARE|SOF_TIMESTAMPING_OPT_ID|SOF_TIMESTAMPING_OPT_TSONLY;
  need(!setsockopt(fd,SOL_SOCKET,SO_TIMESTAMPING_NEW,&flags,sizeof flags),"SO_TIMESTAMPING_NEW");txstamp=tx;
#else
  (void)tx;need(!setsockopt(fd,IPPROTO_IP,IP_BOUND_IF,&iface,sizeof iface),"IP_BOUND_IF");
  need(!setsockopt(fd,IPPROTO_IP,IP_RECVIF,&one,sizeof one),"IP_RECVIF");
  need(!setsockopt(fd,SOL_SOCKET,SO_TIMESTAMP,&one,sizeof one),"SO_TIMESTAMP");
#endif
  auto a=addr(ip,port);need(!bind(fd,(sockaddr*)&a,sizeof a),"bind "+ip);need(fcntl(fd,F_SETFL,O_NONBLOCK)==0,"nonblocking");
 }
 ~Socket(){if(fd>=0)close(fd);} Socket(const Socket&)=delete;
 bool receive(Rx&r){r=Rx{};alignas(cmsghdr) char control[512]{};iovec v{r.p.b.data(),r.p.b.size()};msghdr m{};m.msg_name=&r.peer;m.msg_namelen=sizeof r.peer;m.msg_iov=&v;m.msg_iovlen=1;m.msg_control=control;m.msg_controllen=sizeof control;
  auto n=recvmsg(fd,&m,0);r.app=now();if(n<0){if(errno==EAGAIN||errno==EINTR)return false;throw std::runtime_error("recvmsg");}
  if(n!=I(r.p.b.size())||(m.msg_flags&(MSG_TRUNC|MSG_CTRUNC)))return false;
  for(cmsghdr*c=CMSG_FIRSTHDR(&m);c;c=CMSG_NXTHDR(&m,c)){
#ifdef __linux__
   if(c->cmsg_level==SOL_SOCKET&&c->cmsg_type==SO_TIMESTAMPING_NEW){I x[6]{};if(c->cmsg_len>=CMSG_LEN(sizeof x)){memcpy(x,CMSG_DATA(c),sizeof x);r.raw=x[0]*1000000000+x[1];r.stamp=r.raw>0;}}
   if(c->cmsg_level==IPPROTO_IP&&c->cmsg_type==IP_PKTINFO){in_pktinfo x{};memcpy(&x,CMSG_DATA(c),sizeof x);r.iface=x.ipi_ifindex;}
#else
   if(c->cmsg_level==SOL_SOCKET&&c->cmsg_type==SCM_TIMESTAMP){timeval x{};memcpy(&x,CMSG_DATA(c),sizeof x);r.raw=I(x.tv_sec)*1000000000+I(x.tv_usec)*1000;r.stamp=r.raw>0;}
   if(c->cmsg_level==IPPROTO_IP&&c->cmsg_type==IP_RECVIF){sockaddr_dl x{};memcpy(&x,CMSG_DATA(c),std::min(sizeof x,size_t(c->cmsg_len-CMSG_LEN(0))));r.iface=x.sdl_index;}
#endif
  }
  r.map=mapping();if(r.stamp){r.mono=convert(r.raw,r.map);r.error=map_error(r.raw,r.map);}return true;
 }
 uint32_t send(const Packet&p,const sockaddr_in&a){auto id=txid++;need(sendto(fd,p.b.data(),p.b.size(),0,(const sockaddr*)&a,sizeof a)==I(p.b.size()),"sendto");return id;}
 bool tx_time(uint32_t expected,I&raw,Map&map){
#ifdef __linux__
  I deadline=now()+20000000;
  while(running&&now()<deadline){alignas(cmsghdr) char cbuf[512]{};char data[256]{};iovec v{data,sizeof data};msghdr m{};m.msg_iov=&v;m.msg_iovlen=1;m.msg_control=cbuf;m.msg_controllen=sizeof cbuf;
   if(recvmsg(fd,&m,MSG_ERRQUEUE|MSG_DONTWAIT)<0){pollfd f{fd,POLLERR,0};poll(&f,1,1);continue;}
   bool match=false;raw=0;
   for(cmsghdr*c=CMSG_FIRSTHDR(&m);c;c=CMSG_NXTHDR(&m,c)){
    if(c->cmsg_level==SOL_SOCKET&&c->cmsg_type==SO_TIMESTAMPING_NEW){I x[6]{};if(c->cmsg_len>=CMSG_LEN(sizeof x)){memcpy(x,CMSG_DATA(c),sizeof x);raw=x[0]*1000000000+x[1];}}
    if(c->cmsg_level==IPPROTO_IP&&c->cmsg_type==IP_RECVERR){sock_extended_err e{};memcpy(&e,CMSG_DATA(c),sizeof e);match=e.ee_origin==SO_EE_ORIGIN_TIMESTAMPING&&e.ee_data==expected;}
   }if(match&&raw){map=mapping();return true;}
  }
#else
  (void)expected;(void)raw;(void)map;
#endif
  return false;
 }
};
U nonce(){U v=0;int f=open("/dev/urandom",O_RDONLY);need(f>=0&&read(f,&v,sizeof v)==sizeof v,"nonce");close(f);return v;}
std::map<std::string,std::string> args(int n,char**v){std::map<std::string,std::string>a;for(int i=2;i<n;i++){std::string k=v[i];need(i+1<n,"missing option value");a[k]=v[++i];}return a;}
std::string opt(const std::map<std::string,std::string>&a,const std::string&k,const std::string&d){auto i=a.find(k);return i==a.end()?d:i->second;}
void server(const std::map<std::string,std::string>&a){
 auto ip=opt(a,"--bind","10.0.0.120"),dev=opt(a,"--interface","eth0"),peer=opt(a,"--peer","10.0.0.2");int port=std::stoi(opt(a,"--port","53120"));double seconds=std::stod(opt(a,"--seconds","180"));bool tx=opt(a,"--tx","software")=="software",load=opt(a,"--load","0")=="1";
 Socket s(ip,port,dev,tx);U instance=nonce();I end=now()+I(seconds*1e9);std::atomic<bool> busy{load};std::thread worker;if(load)worker=std::thread([&]{volatile U x=1;while(busy.load(std::memory_order_relaxed)){for(int i=0;i<10000;i++)x=x*6364136223846793005ULL+1;}});
 U received=0,bad=0,tx_ok=0,tx_missing=0;auto allowed=addr(peer,0);
 std::cout<<"READY "<<ip<<":"<<port<<" device="<<dev<<" ifindex="<<s.iface<<" rx=kernel_realtime_mapped_to_monotonic tx="<<(tx?"kernel_software_followup":"application")<<" instance="<<instance<<std::endl;
 try{while(running&&now()<end){pollfd f{s.fd,POLLIN,0};if(poll(&f,1,100)<=0)continue;Rx r;if(!s.receive(r))continue;
  if(r.peer.sin_addr.s_addr!=allowed.sin_addr.s_addr||r.p.get(0)!=MAGIC||r.p.get(1)!=1||r.p.get(2)!=0||!r.stamp||r.iface!=s.iface){++bad;continue;}
  Packet p=r.p;p.put(2,1);p.put(6,r.mono);p.put(8,r.raw);p.put(9,r.app);p.put(10,r.map.real);p.put(11,r.map.mono);p.put(12,r.error);p.put(13,r.iface);p.put(14,tx?2:1);p.put(19,load);p.put(20,instance);p.put(7,now());auto id=s.send(p,r.peer);++received;
  if(tx){I raw=0;Map m;bool ok=s.tx_time(id,raw,m);p.put(2,2);p.put(14,ok?2:3);if(ok){++tx_ok;p.put(7,convert(raw,m));p.put(15,raw);p.put(16,m.real);p.put(17,m.mono);p.put(18,map_error(raw,m));}else ++tx_missing;s.send(p,r.peer);}
 }}catch(...){busy=false;if(worker.joinable())worker.join();throw;}
 busy=false;if(worker.joinable())worker.join();std::cout<<"SUMMARY received="<<received<<" rejected="<<bad<<" tx_kernel="<<tx_ok<<" tx_missing="<<tx_missing<<std::endl;
}
void client(const std::map<std::string,std::string>&a){
 auto bindip=opt(a,"--bind","10.0.0.2"),dev=opt(a,"--interface","en5"),eth=opt(a,"--ethernet","10.0.0.120"),wifi=opt(a,"--wifi","10.0.0.20"),file=opt(a,"--csv","probes.csv");
 int pairs=std::stoi(opt(a,"--pairs","1200")),interval=std::stoi(opt(a,"--interval-ms","25"));need(pairs>0&&pairs<=100000&&interval>=1,"invalid count/interval");Socket s(bindip,0,dev,false);U session=nonce();I begin=now();
 std::ofstream out(file);need(bool(out),"open CSV");out<<"id,path,status,session,server_instance,d1_ns,p2_ns,p3_ns,d4_ns,pi_rx_realtime_ns,desktop_rx_realtime_ns,pi_rx_app_ns,desktop_rx_app_ns,pi_rx_map_real_ns,pi_rx_map_mono_ns,pi_rx_error_ns,pi_tx_realtime_ns,pi_tx_map_real_ns,pi_tx_map_mono_ns,pi_tx_error_ns,desktop_rx_map_real_ns,desktop_rx_map_mono_ns,desktop_rx_error_ns,pi_ifindex,desktop_ifindex,pi_tx_source,pi_load,offset_lower_ns,offset_upper_ns,rtt_ns\n";
 U success=0,fail=0;I reference=0;int sign=0;
 for(int k=0;k<pairs*2&&running;k++){
  // Reverse pair order on alternate pairs to avoid always sampling one path first.
  bool wired=((k%2)^((k/2)%2))==0;auto dst=addr(wired?eth:wifi,wired?53120:53121);U id=k+1;
  I scheduled=begin+I(k)*interval*1000000;if(now()<scheduled)std::this_thread::sleep_for(std::chrono::nanoseconds(scheduled-now()));
  Packet req;req.put(0,MAGIC);req.put(1,1);req.put(2,0);req.put(3,session);req.put(4,id);I d1=now();req.put(5,d1);s.send(req,dst);
  I deadline=now()+150000000;Rx original;Packet answer;bool got=false,follow=false;std::string status="timeout";
  while(running&&now()<deadline){pollfd f{s.fd,POLLIN,0};if(poll(&f,1,2)<=0)continue;Rx r;if(!s.receive(r))continue;
   if(r.peer.sin_addr.s_addr!=dst.sin_addr.s_addr||r.peer.sin_port!=dst.sin_port||r.p.get(0)!=MAGIC||r.p.get(1)!=1||r.p.get(3)!=session||r.p.get(4)!=id||r.p.get(5)!=U(d1))continue;
   if(r.p.get(2)==1){original=r;got=true;if(r.p.get(14)==1){answer=r.p;follow=true;}}
   if(r.p.get(2)==2){answer=r.p;follow=true;}
   if(got&&follow){status="ok";break;}
  }
  I lo=0,hi=0,rtt=0;if(status=="ok"){
   if(!original.stamp||original.iface!=s.iface)status="missing_timestamp_or_wrong_interface";
   else if(answer.get(14)==3)status="missing_pi_tx_timestamp";
   else if(original.p.get(20)!=answer.get(20))status="server_changed";
   else{I p2=answer.get(6),p3=answer.get(7),d4=original.mono;rtt=(d4-d1)-(p3-p2);lo=d1-p2;hi=d4-p3;
    if(p3<p2||d4<d1||rtt<-(original.error+I(answer.get(12))+I(answer.get(18))))status="inconsistent_timestamps";
   }
  }
  if(status=="ok"){++success;if(!sign){reference=lo+(hi-lo)/2;sign=reference>=0?1:-1;std::cout<<"OFFSET SIGN "<<(sign==1?"Desktop - Pi":"Pi - Desktop")<<"; reference_ns="<<(sign*reference)<<"\n";}if(success%200==0)std::cout<<"PROGRESS "<<id<<"/"<<pairs*2<<" "<<(wired?"ethernet":"wifi")<<" offset_ms="<<std::fixed<<double(sign*(lo+(hi-lo)/2))/1e6<<" rtt_ms="<<double(rtt)/1e6<<std::endl;}else ++fail;
  out<<id<<','<<(wired?"ethernet":"wifi")<<','<<status<<','<<session<<','<<answer.get(20)<<','<<d1<<','<<answer.get(6)<<','<<answer.get(7)<<','<<original.mono<<','<<answer.get(8)<<','<<original.raw<<','<<answer.get(9)<<','<<original.app<<','<<answer.get(10)<<','<<answer.get(11)<<','<<answer.get(12)<<','<<answer.get(15)<<','<<answer.get(16)<<','<<answer.get(17)<<','<<answer.get(18)<<','<<original.map.real<<','<<original.map.mono<<','<<original.error<<','<<answer.get(13)<<','<<original.iface<<','<<answer.get(14)<<','<<answer.get(19)<<','<<lo<<','<<hi<<','<<rtt<<'\n';
 }
 out.close();std::cout<<"DONE valid="<<success<<" failed="<<fail<<" seconds="<<double(now()-begin)/1e9<<" csv="<<file<<std::endl;need(success>0,"no valid probes");
}
int main(int n,char**v){signal(SIGINT,stop);signal(SIGTERM,stop);try{need(n>=2,"usage: clock_probe server|client [--option value]");auto a=args(n,v);if(std::string(v[1])=="server")server(a);else if(std::string(v[1])=="client")client(a);else throw std::runtime_error("unknown mode");return 0;}catch(const std::exception&e){std::cerr<<"ERROR "<<e.what()<<'\n';return 1;}}
