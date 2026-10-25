#include "ReadyProbeSession.hpp"
#include <cstdio>
#include <cstdlib>
#include <vector>
using vfdual_android::ReadyProbeSession;
namespace {
void require(bool ok,const char* message) {if(!ok){std::fprintf(stderr,"FAIL %s\n",message);std::exit(1);}}
}
int main() {
  constexpr std::uint32_t local=0xc0a8012aU,peer=0xc0a80116U,other=0xc0a80117U;
  ReadyProbeSession state;
  auto generation=state.begin(local);
  std::array<char,vfdual_android::kReadyProbeBytes> probe{},ack{};
  constexpr std::string_view text="VF_CAT6_PROBE_V1|Valid_42-token|";
  std::copy(text.begin(),text.end(),probe.begin());
  auto receive=[&](std::ptrdiff_t size,int error,std::span<const char> data,
                   std::uint32_t source,std::uint64_t at=100'000U){
    return state.receive(generation,size,error,data,source,at,ack).outcome;
  };
  for(int stale_errno:{0,EIO,EAGAIN,EINTR,EBADF}) {
    require(receive(0,stale_errno,{},peer)==ReadyProbeSession::Outcome::ignored,"empty UDP stays running");
    require(state.requester(100'000)==0,"empty cannot publish");
  }
  std::array<char,1200> invalid{};
  require(receive(1200,0,invalid,peer)==ReadyProbeSession::Outcome::ignored,"malformed rejected");
  require(state.requester(100'000)==0,"invalid sender cannot become discovered Host");
  for(auto bad:{0U,0x7f000001U,0xe0000001U,0xa9fe0101U,0xffffffffU,local})
    require(receive(1200,0,probe,bad)==ReadyProbeSession::Outcome::ignored,"invalid or self source rejected");
  for(std::size_t length:{1199U,1201U,65507U}) {
    std::vector<char> oversized(probe.begin(),probe.end());oversized.resize(length);
    require(receive(static_cast<std::ptrdiff_t>(length),0,oversized,peer)==ReadyProbeSession::Outcome::ignored,
        "exact probe size required including oversized valid prefix");
  }
  require(receive(1200,EIO,probe,peer)==ReadyProbeSession::Outcome::valid_probe,"valid probe after empty/invalid");
  require(std::string_view(ack.data()).starts_with("VF_CAT6_PROBE_ACK_V1|Valid_42-token|"),"ack token preserved");
  require(state.requester(100'000)==peer,"only valid probe publishes");
  require(receive(1200,0,invalid,other,101'000)==ReadyProbeSession::Outcome::ignored,"other invalid cannot replace");
  require(state.requester(101'000)==peer,"retains valid discovered Host");
  require(state.requester(99'999)==0,"backward time cannot expose discovery");
  require(state.requester(2'100'001)==0,"stale discovery not selectable");
  require(receive(-1,EAGAIN,{},other)==ReadyProbeSession::Outcome::ignored,"timeout normal");
  require(receive(-1,EINTR,{},other)==ReadyProbeSession::Outcome::ignored,"interrupted normal");
  require(receive(-1,EBADF,{},other)==ReadyProbeSession::Outcome::fatal,"real socket fault preserved");
  const auto old=generation;state.end(generation);generation=state.begin(local);
  require(state.requester(100'000)==0,"new session clears discovery");
  require(state.receive(old,1200,0,probe,peer,100'000,ack).outcome==ReadyProbeSession::Outcome::ignored,"old worker cannot publish");
  require(state.receive(old,-1,EBADF,{},peer,100'000,ack).outcome==ReadyProbeSession::Outcome::ignored,"old failure cannot revoke new session");
  state.end(old);
  require(receive(1200,0,probe,other)==ReadyProbeSession::Outcome::valid_probe,"old stop does not clear current session");
  require(state.requester(100'000)==other,"fresh current discovery");
  state.end(generation);require(state.requester(100'000)==0,"stopped agent exposes no requester");
  std::puts("PASS production Ready receive transaction: errno, protocol, source, epoch and freshness");
}
