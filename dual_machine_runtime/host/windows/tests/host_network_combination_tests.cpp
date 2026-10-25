#include "vfdual/host_direct_link_provisioner.hpp"
#include "vfdual/host_firewall_provisioner.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <tuple>
#include <vector>
using namespace vfdual;
namespace {
void check(bool value,const char* text,int line){if(!value){std::cerr<<line<<": "<<text<<'\n';std::exit(EXIT_FAILURE);}}
#define CHECK(x) check(bool(x),#x,__LINE__)
auto key(const HostFirewallRuleSnapshot& s){return std::tie(s.name,s.description,s.grouping,s.application_path,
    s.interface_name,s.interface_types,s.local_address,s.remote_addresses,s.local_port,s.remote_port,s.protocol,
    s.inbound,s.enabled,s.allow,s.edge_traversal,s.service_name,s.local_app_package_id,s.local_user_owner,
    s.local_user_authorized_list,s.remote_user_authorized_list,s.remote_machine_authorized_list,s.secure_flags,
    s.protocol_number,s.edge_traversal_options,s.profiles,s.all_profiles);}
bool same_set(const std::vector<HostFirewallRuleSnapshot>& a,const std::vector<HostFirewallRuleSnapshot>& b){
    if(a.size()!=b.size())return false;std::vector<bool> used(b.size());
    for(const auto& s:a){bool found=false;for(std::size_t i{};i<b.size();++i)if(!used[i] && key(s)==key(b[i])){used[i]=true;found=true;break;}if(!found)return false;}return true;
}
HostFirewallRuleSnapshot snapshot(const HostFirewallRulePolicy& p){
    HostFirewallRuleSnapshot s;static_cast<HostFirewallRulePolicy&>(s)=p;
    s.protocol_number=17;s.profiles=0x7fffffff;s.all_profiles=true;return s;
}
struct MemoryRules final:HostFirewallRuleStore {
    std::vector<HostFirewallRuleSnapshot> rules;
    unsigned audits{},collects{},prepares{},removes{},adds{};
    unsigned fail_add{},fail_remove{},fail_prepare{},fail_audit{};
    bool partial_add{},unreadable{};
    bool audit_entries(std::vector<HostFirewallAuditedRule>& out,std::string& detail)override{
        if(++audits==fail_audit){detail="injected_audit_failure";return false;}
        for(const auto& s:rules)out.push_back({{s.name,s.application_path,s.description,s.grouping,s.inbound,s.enabled,s.allow},!unreadable,s});return true;
    }
    bool enumerate_named(std::wstring_view name,std::vector<HostFirewallStoredRule>& out,std::string&)override{
        ++collects;for(const auto& s:rules)if(s.name==name)out.push_back({!unreadable,s});return true;
    }
    bool prepare(const HostFirewallRuleSnapshot&,std::string& detail)override{
        if(++prepares==fail_prepare){detail="injected_prepare_failure";return false;}return true;
    }
    bool remove(std::wstring_view name,std::string& detail)override{
        if(++removes==fail_remove){detail="injected_remove_failure";return false;}
        const auto i=std::find_if(rules.begin(),rules.end(),[&](const auto& s){return s.name==name;});
        if(i==rules.end()){detail="missing_rule";return false;}rules.erase(i);return true;
    }
    bool add(const HostFirewallRuleSnapshot& s,std::string& detail)override{
        const bool fail=++adds==fail_add;
        if(!fail || partial_add)rules.push_back(s);
        if(fail){detail="injected_add_failure";return false;}return true;
    }
};
std::vector<HostFirewallRuleSnapshot> legacy_rules(const std::vector<HostFirewallRulePolicy>& policy){
    std::vector<HostFirewallRuleSnapshot> result;
    for(std::size_t i{};i<policy.size();++i){
        auto s=snapshot(policy[i]);s.name=L"VisionForge"+s.name.substr(2);
        s.description=i<3?L"VisionForge Host isolated CAT6 transport":L"VisionForge Host wireless LAN UDP transport";
        s.grouping=L"VisionForge Host Transport";result.push_back(s);
    }return result;
}
void firewall_combination(){
    const std::wstring exe=L"C:\\VF\\VFHost-Local.exe";
    const auto desired=build_host_firewall_policy(exe);
    const auto legacy=legacy_rules(desired);
    for(unsigned mode{};mode<3;++mode){
        MemoryRules store;store.rules=legacy;
        if(mode==1){store.rules.push_back(legacy[1]);store.rules.push_back(snapshot(desired[1]));}
        if(mode==2){store.rules.clear();for(const auto& p:desired)store.rules.push_back(snapshot(p));}
        const auto result=reconcile_host_firewall_rule_store(exe,store,true);
        CHECK(host_firewall_is_ready(result.status));CHECK(store.rules.size()==desired.size());
        CHECK(store.audits>=1 && store.collects>=desired.size()*2);
        for(std::size_t i{};i<desired.size();++i){std::vector<HostFirewallRuleSnapshot> matching;
            for(const auto& s:store.rules)if(s.name==desired[i].name)matching.push_back(s);
            CHECK(host_firewall_rule_set_is_current(desired[i],matching));}
        const auto removed=store.removes,added=store.adds;
        CHECK(reconcile_host_firewall_rule_store(exe,store,true).status==HostFirewallStatus::ready);
        CHECK(removed==store.removes && added==store.adds);
        if(mode==2)CHECK(store.removes==0 && store.adds==0);
    }
    // Every mutation targets a complete owned fingerprint; a registered name
    // and current EXE path alone never grant write ownership.
    for(unsigned bad{};bad<10;++bad){
        MemoryRules store;store.rules=legacy;auto& s=store.rules[1];
        switch(bad){
            case 0:++s.local_port;break;case 1:s.remote_addresses="*";break;
            case 2:s.description=L"imposter";break;case 3:s.grouping=L"imposter";break;
            case 4:s.application_path=L"C:\\Other\\evil.exe";break;
            case 5:s.service_name=L"different-service";break;case 6:s.local_user_owner=L"different-owner";break;
            case 7:s.name=L"Unknown wide current-executable allow";s.local_port=0;break;
            case 8:s.secure_flags=1;break;case 9:store.unreadable=true;break;
        }
        const auto before=store.rules;
        CHECK(reconcile_host_firewall_rule_store(exe,store,true).status==HostFirewallStatus::failed);
        CHECK(store.removes==0 && store.adds==0 && same_set(before,store.rules));
    }
    for(unsigned fault{};fault<5;++fault){
        MemoryRules store;store.rules=legacy;store.rules.push_back(legacy[1]);const auto before=store.rules;
        if(fault==0)store.fail_add=3;
        if(fault==1){store.fail_add=3;store.partial_add=true;}
        if(fault==2)store.fail_remove=3;
        if(fault==3)store.fail_prepare=3;
        if(fault==4)store.fail_audit=2; // final audit, after actual replacements
        const auto result=reconcile_host_firewall_rule_store(exe,store,true);
        CHECK(result.status==HostFirewallStatus::failed);CHECK(same_set(before,store.rules));
        if(fault!=3)CHECK(result.detail.find("rollback=complete")!=std::string::npos);
        else CHECK(store.removes==0 && store.adds==0);
    }
    MemoryRules limited;limited.rules=legacy;
    CHECK(reconcile_host_firewall_rule_store(exe,limited,false).status==HostFirewallStatus::requires_elevation);
    CHECK(limited.removes==0 && limited.adds==0 && limited.prepares==0);
    std::cout<<"PASS firewall enumeration/audit/collect/reconcile: current, legacy, duplicates, ownership negatives, rollback\n";
}
HostNetworkAdapterProfile alias_adapter(){
    HostNetworkAdapterProfile a;a.adapter_id="test-adapter";a.connection_name=L"Ethernet";a.interface_index=9;
    a.transport=HostAdapterTransport::ethernet;a.hardware_interface=a.operational=a.dhcp_enabled=a.has_usable_ipv4=true;
    a.device_identity={true,L"USB\\VID_TEST",{L"USB\\VID_TEST"},L"test-usb-driver",L"test-usb.inf"};
    a.ipv4_addresses.push_back({"10.57.49.60",24,1,1,4,true});return a;
}
void address_combination(){
    for(unsigned scenario{};scenario<10;++scenario){
        auto live=alias_adapter();const auto original=live;
        bool static_dns=scenario==4,prefer_persistent=true;
        if(scenario==5)live.ipv4_addresses[0].address="192.168.8.5";
        if(scenario==6)live.has_default_gateway=true;
        if(scenario==7 || scenario==9){live.ipv4_addresses.clear();live.has_usable_ipv4=false;}
        if(scenario==9)prefer_persistent=false;
        const auto selected=choose_host_direct_link_plan({live});
        if(scenario==5 || scenario==6)CHECK(!selected.downstream_adapter);
        else CHECK(selected.downstream_adapter);
        const auto plan=plan_host_direct_link_address_backend(live,true,static_dns,prefer_persistent);
        unsigned records{},alias_calls{},persistent_calls{},dns_calls{},sharing_calls{},policy_calls{},rollbacks{};
        HostDirectLinkAddressBackend recorded=HostDirectLinkAddressBackend::none;
        bool owned_alias=false;int policy{};
        const auto execution=execute_host_direct_link_address_plan(plan,{
            .record_backend=[&](auto backend,std::string&){++records;recorded=backend;return true;},
            .prepare_private_sharing=[&](std::string&){++sharing_calls;return true;},
            .create_transient=[&](std::string&){++alias_calls;
                if(scenario==1 || scenario==9)return HostDirectLinkTransientStatus::failed;
                if(scenario==2)return HostDirectLinkTransientStatus::ownership_conflict;
                if(scenario==8)return HostDirectLinkTransientStatus::validation_failed;
                owned_alias=true;return HostDirectLinkTransientStatus::created;},
            .configure_persistent=[&](std::string&){++persistent_calls;live.dhcp_enabled=false;live.ipv4_addresses.clear();return true;},
            .configure_dns=[&](std::string&){++dns_calls;return true;},
            .configure_interface_policy=[&](std::string&){++policy_calls;policy=1;return scenario!=3;},
            .verify=[&](std::string&){return scenario==7 || scenario==9 || host_direct_link_preserved_ipv4_matches(original.ipv4_addresses,live);},
            .rollback=[&](auto applied,std::string&){++rollbacks;if(applied==HostDirectLinkAddressBackend::transient_ip_helper)owned_alias=false;policy=0;return true;},
        });
        if(scenario<4 || scenario==8){
            CHECK(plan.preserve_existing_ipv4 && !plan.allow_persistent_fallback);
            CHECK(plan.backend==HostDirectLinkAddressBackend::transient_ip_helper && recorded==plan.backend);
            CHECK(alias_calls==1 && persistent_calls==0 && dns_calls==0 && sharing_calls==0);
            CHECK(live.dhcp_enabled && host_direct_link_preserved_ipv4_matches(original.ipv4_addresses,live));
            if(scenario==0){CHECK(execution.transaction.succeeded && owned_alias);owned_alias=false;policy=0;}
            else CHECK(!execution.transaction.succeeded && rollbacks==1 && !owned_alias && policy==0);
        }else if(scenario<7){CHECK(!execution.transaction.succeeded && records==0 && alias_calls==0 && persistent_calls==0 && policy_calls==0);}
        else CHECK(execution.transaction.succeeded && persistent_calls==1 && alias_calls==(scenario==9?1U:0U) && recorded==HostDirectLinkAddressBackend::persistent_netsh);
    }
    auto live=alias_adapter();const auto expected=live.ipv4_addresses;
    live.ipv4_addresses.clear();CHECK(!host_direct_link_preserved_ipv4_matches(expected,live));
    live=alias_adapter();live.ipv4_addresses[0].skip_as_source=false;CHECK(!host_direct_link_preserved_ipv4_matches(expected,live));
    std::cout<<"PASS direct-link candidate/plan/backend record/mutations/rollback: protected alias wins, no unsafe fallback\n";
}
}
int main(){firewall_combination();address_combination();}
