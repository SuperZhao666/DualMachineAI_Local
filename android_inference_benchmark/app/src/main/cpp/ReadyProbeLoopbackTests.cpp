#include "ReadyProbeSession.hpp"
#ifdef _WIN32
#include <cstdio>
int main() { std::puts("SKIP POSIX Ready loopback; run same source on Android/ARM"); return 77; }
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
using vfdual_android::ReadyProbeSession;
namespace {
void check(bool ok,const char* message) {if(!ok){std::fprintf(stderr,"FAIL %s errno=%d\n",message,errno);std::exit(1);}}
}
int main() {
  ifaddrs* interfaces{};check(getifaddrs(&interfaces)==0,"read existing interface addresses");
  std::uint32_t source{};
  for(auto* it=interfaces;it;it=it->ifa_next) if(it->ifa_addr && (it->ifa_flags&IFF_UP) && it->ifa_addr->sa_family==AF_INET) {
    const auto value=ntohl(reinterpret_cast<sockaddr_in*>(it->ifa_addr)->sin_addr.s_addr);
    if(ReadyProbeSession::usable_peer(value)){source=value;break;}
  }
  freeifaddrs(interfaces);
  if(!source){std::puts("SKIP no usable existing IPv4 for kernel-local probe");return 77;}
  const int receiver=socket(AF_INET,SOCK_DGRAM,0),sender=socket(AF_INET,SOCK_DGRAM,0);
  check(receiver>=0&&sender>=0,"create isolated UDP sockets");
  sockaddr_in local{},from{};local.sin_family=from.sin_family=AF_INET;
  local.sin_addr.s_addr=htonl(INADDR_LOOPBACK);from.sin_addr.s_addr=htonl(source);
  check(bind(receiver,reinterpret_cast<sockaddr*>(&local),sizeof(local))==0,"bind loopback ephemeral port");
  check(bind(sender,reinterpret_cast<sockaddr*>(&from),sizeof(from))==0,"bind existing source address");
  socklen_t size=sizeof(local);check(getsockname(receiver,reinterpret_cast<sockaddr*>(&local),&size)==0,"read ephemeral port");
  timeval timeout{1,0};check(setsockopt(receiver,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))==0,"bounded receive");
  check(setsockopt(sender,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))==0,"bounded ack receive");
  ReadyProbeSession state;auto generation=state.begin(INADDR_LOOPBACK);
  std::array<char,1201> input{};std::array<char,1200> ack{};
  auto receive=[&](int initial_errno) {
    sockaddr_in peer{};socklen_t length=sizeof(peer);errno=initial_errno;
    const auto received=recvfrom(receiver,input.data(),input.size(),0,reinterpret_cast<sockaddr*>(&peer),&length);
    const int receive_errno=received<0?errno:0;
    return state.receive(generation,received,receive_errno,
        {input.data(),received>0?static_cast<std::size_t>(received):0U},ntohl(peer.sin_addr.s_addr),100'000,ack);
  };
  auto send=[&](const char* data,std::size_t bytes) {
    check(sendto(sender,data,bytes,0,reinterpret_cast<sockaddr*>(&local),sizeof(local))==static_cast<ssize_t>(bytes),"send only to own loopback socket");
  };
  for(int old_errno:{0,EIO,EAGAIN}) {
    send("",0);check(receive(old_errno).outcome==ReadyProbeSession::Outcome::ignored,"kernel zero UDP is not socket failure");
    check(state.requester(100'000)==0,"empty did not select Host");
  }
  input.fill('x');send(input.data(),1200);
  check(receive(EIO).outcome==ReadyProbeSession::Outcome::ignored,"invalid kernel UDP cannot select Host");
  std::array<char,1200> probe{};constexpr std::string_view text="VF_CAT6_PROBE_V1|kernel42|";
  std::copy(text.begin(),text.end(),probe.begin());
  std::vector<char> oversized(probe.begin(),probe.end());oversized.resize(2400);
  send(oversized.data(),oversized.size());
  check(receive(EIO).outcome==ReadyProbeSession::Outcome::ignored,"max+1 exposes oversized prefix");
  send(probe.data(),probe.size());
  check(receive(EIO).outcome==ReadyProbeSession::Outcome::valid_probe,"normal probe still works after invalid traffic");
  check(state.requester(100'000)==source,"actual validated socket peer selected");
  size=sizeof(from);check(getsockname(sender,reinterpret_cast<sockaddr*>(&from),&size)==0,"read reply destination");
  check(sendto(receiver,ack.data(),ack.size(),0,reinterpret_cast<sockaddr*>(&from),sizeof(from))==1200,"send local ACK");
  check(recv(sender,input.data(),input.size(),0)==1200,"receive complete local ACK");
  check(std::string_view(input.data()).starts_with("VF_CAT6_PROBE_ACK_V1|kernel42|"),"valid ACK follows empty UDP");
  close(receiver);errno=0;
  check(receive(0).outcome==ReadyProbeSession::Outcome::fatal,"actual closed socket remains fatal");
  state.end(generation);check(state.requester(100'000)==0,"explicit stop removes discovery");close(sender);
  std::puts("PASS Ready actual recvfrom/validation/publication/ACK on ephemeral kernel-local sockets; no App ports, UI, NPU or HID");
}
#endif
