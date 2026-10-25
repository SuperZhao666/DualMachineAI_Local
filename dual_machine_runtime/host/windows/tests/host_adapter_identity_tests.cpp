// The production enumeration fact conversion, GUID binding, chooser and
// mutation plan are exercised together. Live inventory is read-only.
#include "../src/host_direct_link_provisioner.cpp"
#include <cstdlib>
#include <iostream>
namespace {
void require(bool value,const char* text){if(!value){std::cerr<<text<<'\n';std::exit(1);}}
vfdual::HostNetworkDeviceIdentity physical(bool usb){
    return {true,usb?L"USB\\VID_0BDA&PID_8153\\test":L"PCI\\VEN_10EC&DEV_8168\\test",
        {usb?L"USB\\VID_0BDA&PID_8153":L"PCI\\VEN_10EC&DEV_8168"},L"rt640x64",L"oem-test.inf"};
}
vfdual::HostNetworkAdapterProfile convert(const vfdual::HostNetworkDeviceIdentity& identity,bool match=true){
    vfdual::HostNetworkAdapterProfile p;p.adapter_id="{TEST-GUID}";p.interface_index=6;
    p.connection_name=L"Renamed arbitrary adapter";p.operational=true;p.dhcp_enabled=true;p.has_link_local_ipv4=true;
    MIB_IF_ROW2 row{};row.Type=IF_TYPE_ETHERNET_CSMACD;row.MediaType=NdisMedium802_3;
    row.PhysicalMediumType=NdisPhysicalMediumUnspecified;
    row.InterfaceAndOperStatusFlags.HardwareInterface=TRUE;row.InterfaceAndOperStatusFlags.ConnectorPresent=TRUE;
    row.InterfaceAndOperStatusFlags.EndPointInterface=TRUE;
    const std::map<std::string,vfdual::HostNetworkDeviceIdentity> identities{{match?"test-guid":"different-guid",identity}};
    vfdual::apply_adapter_interface_identity(p,&row,IF_TYPE_ETHERNET_CSMACD,identities);return p;
}
void run_plan(const vfdual::HostNetworkAdapterProfile& p,bool allowed){
    const auto plan=vfdual::plan_host_direct_link_address_backend(p,true,false,true);
    unsigned writes{};
    const auto result=vfdual::execute_host_direct_link_address_plan(plan,{
        .record_backend=[&](auto,std::string&){++writes;return true;},
        .prepare_private_sharing=[](std::string&){return true;},
        .create_transient=[](std::string&){return vfdual::HostDirectLinkTransientStatus::created;},
        .configure_persistent=[&](std::string&){++writes;return true;},
        .configure_dns=[](std::string&){return true;},.configure_interface_policy=[](std::string&){return true;},
        .verify=[](std::string&){return true;},.rollback=[](auto,std::string&){return true;},
    });
    require(result.transaction.succeeded==allowed,"mutation admissibility");
    require(allowed?writes>0:writes==0,"unknown/loopback must fail before snapshot or mutations");
}
}
int main(){
    const vfdual::HostNetworkDeviceIdentity loop{true,L"ROOT\\NET\\0000",{L"*msloop"},L"kmloop",L"netloop.inf"};
    auto p=convert(loop);
    require(p.hardware_interface && p.transport==vfdual::HostAdapterTransport::ethernet,"actual misleading MIB facts preserved");
    require(vfdual::host_network_identity_is_software_loopback(p.device_identity),"exact PnP driver loopback identity");
    require(!vfdual::choose_host_direct_link_plan({p}).downstream_adapter,"known loopback cannot be downstream");run_plan(p,false);
    for(unsigned marker{};marker<3;++marker){
        auto id=physical(true);
        if(marker==0)id.hardware_ids={L"*MSLOOP"};
        if(marker==1)id.service_name=L"KMLOOP";
        if(marker==2)id.driver_inf=L"C:\\Windows\\INF\\NETLOOP.INF";
        auto candidate=convert(id);require(!vfdual::choose_host_direct_link_plan({candidate}).downstream_adapter,"exact marker independent of friendly name");run_plan(candidate,false);
    }
    for(bool usb:{false,true}){
        auto real=convert(physical(usb));real.connection_name=L"Npcap Loopback Adapter";
        require(vfdual::choose_host_direct_link_plan({real}).downstream_adapter.has_value(),"renamed PCI/USB with unspecified physical media survives");
        run_plan(real,true);
    }
    auto suffix=physical(true);suffix.hardware_ids={L"*msloop-extra"};suffix.service_name=L"kmloop-extra";suffix.driver_inf=L"netloop.inf.old";
    run_plan(convert(suffix),true); // precise identities, not broad substring denylisting
    for(unsigned unknown{};unknown<4;++unknown){
        auto id=physical(true);
        if(unknown==0)id.resolved=false;if(unknown==1)id.hardware_ids.clear();if(unknown==2)id.service_name.clear();
        auto unresolved=convert(id,unknown!=3);
        require(!vfdual::host_network_identity_is_software_loopback(unresolved.device_identity),"unknown is not labelled virtual");
        run_plan(unresolved,false);
        unresolved.has_isolated_private_ipv4=true;unresolved.has_usable_ipv4=true;
        unresolved.ipv4_addresses.push_back({"10.57.23.1",24,1,1,4,false});
        const auto ready=vfdual::choose_host_direct_link_plan({unresolved});
        require(ready.downstream_already_ready,"unknown existing address can still be read/probed");
        require(!vfdual::host_direct_link_adapter_mutation_is_admissible(unresolved),"readiness is not mutation authorization");
    }
    unsigned live_loopbacks{};
    const auto live=vfdual::enumerate_network_profiles();
    for(const auto& adapter:live)if(vfdual::host_network_identity_is_software_loopback(adapter.device_identity)){
        ++live_loopbacks;require(!vfdual::choose_host_direct_link_plan({adapter}).downstream_adapter,"production live enumeration excludes known loopback");
    }
    std::cout<<"PASS MIB/PnP GUID identity -> production chooser/plan/mutation gates; live software loopbacks excluded="<<live_loopbacks<<'\n';
}
