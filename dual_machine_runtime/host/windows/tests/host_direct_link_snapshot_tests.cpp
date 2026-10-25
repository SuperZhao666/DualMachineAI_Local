// Actual production serialization/parser, isolated in-memory payloads only.
#include "../src/host_direct_link_provisioner.cpp"
#include <cstdlib>
#include <iostream>
namespace {
void require(bool value,const char* text){if(!value){std::cerr<<text<<'\n';std::exit(1);}}
std::string signed_payload(const std::string& payload){return payload+"sha256="+vfdual::sha256_hex(payload)+'\n';}
}
int main(){
    vfdual::HostDirectLinkChangeSnapshot original;
    original.adapter_id="test-adapter";original.interface_index=9;
    original.owned_address_backend=vfdual::RestoreAddressBackendHint::transient_ip_helper;
    original.preserved_ipv4.push_back({"10.57.49.60",24,1,1,4,true});
    original.preserved_ipv4.push_back({"10.57.49.61",24,1,1,4,std::nullopt});
    const auto payload=vfdual::snapshot_payload(original);
    vfdual::HostDirectLinkChangeSnapshot decoded;std::string detail;
    require(vfdual::parse_snapshot_contents(signed_payload(payload),decoded,detail),"v3 roundtrip");
    require(decoded.preserved_ipv4.size()==2 && decoded.owned_address_backend==original.owned_address_backend,"guard/backend roundtrip");
    vfdual::HostNetworkAdapterProfile live;live.ipv4_addresses=original.preserved_ipv4;
    require(vfdual::host_direct_link_preserved_ipv4_matches(decoded.preserved_ipv4,live),"real retained addresses match");
    live.ipv4_addresses.pop_back();
    require(!vfdual::host_direct_link_preserved_ipv4_matches(decoded.preserved_ipv4,live),"DHCP alone cannot prove lost alias restored");
    for(unsigned version:{1U,2U}){
        auto legacy=payload;legacy.replace(0,9,version==1?"version=1":"version=2");
        const auto guard=legacy.find("preserved_ipv4_count=");legacy.erase(guard);
        if(version==1){const auto a=legacy.find("owned_address_backend=");legacy.erase(a,legacy.find('\n',a)-a+1U);}
        require(vfdual::parse_snapshot_contents(signed_payload(legacy),decoded,detail),"legacy v1/v2 readable");
        require(decoded.preserved_ipv4.empty(),"legacy has no fabricated address history");
    }
    for(unsigned mode{};mode<6;++mode){
        auto bad=payload;
        if(mode==0)bad.replace(bad.find("10.57.49.60"),11,"not-an-ip");
        if(mode==1)bad.replace(bad.find("preserved_ipv4_count=2"),22,"preserved_ipv4_count=65");
        if(mode==2){const auto a=bad.find("preserved_ipv4_1=");bad.erase(a,bad.find('\n',a)-a+1U);}
        if(mode==3)bad+="unknown=field\n";
        if(mode==4)bad.replace(bad.find(";24;"),4,";99;");
        if(mode==5)bad.replace(bad.find(";1;1;1"),6,";1;1;9");
        require(!vfdual::parse_snapshot_contents(signed_payload(bad),decoded,detail),"bad guarded snapshot rejected");
    }
    auto bad_hash=signed_payload(payload);bad_hash[0]='X';
    require(!vfdual::parse_snapshot_contents(bad_hash,decoded,detail),"hash tamper rejected");
    std::cout<<"PASS production direct-link snapshot v3 guards, v1/v2 compatibility, missing aliases and malformed payloads\n";
}
