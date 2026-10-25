// Compile the production COM adapter as well as its reconciliation engine.
// The only INetFwRules instance is this in-memory implementation. No system
// INetFwPolicy2 object is created and no Windows firewall rule is changed.
#include "../src/host_firewall_provisioner.cpp"
#include <cstdlib>
#include <iostream>

namespace {
using Microsoft::WRL::ComPtr;
void require(bool condition,const char* what){if(!condition){std::cerr<<what<<'\n';std::exit(EXIT_FAILURE);}}
class RuleEnumerator final:public IEnumVARIANT {
public:
    explicit RuleEnumerator(std::vector<ComPtr<INetFwRule>> rules,bool fail=false):rules_(std::move(rules)),fail_(fail){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out)override{
        if(!out)return E_POINTER;*out=nullptr;
        if(id==IID_IUnknown || id==IID_IEnumVARIANT)*out=static_cast<IEnumVARIANT*>(this);
        else return E_NOINTERFACE;AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef()override{return ++references_;}
    ULONG STDMETHODCALLTYPE Release()override{const auto n=--references_;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE Next(ULONG count,VARIANT* items,ULONG* fetched)override{
        if(fetched)*fetched=0;if(fail_)return E_FAIL;if(!items)return E_POINTER;
        ULONG n{};for(;n<count && index_<rules_.size();++n,++index_){
            VariantInit(&items[n]);items[n].vt=VT_DISPATCH;items[n].pdispVal=rules_[index_].Get();items[n].pdispVal->AddRef();
        }if(fetched)*fetched=n;return n==count?S_OK:S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Skip(ULONG count)override{index_=(std::min)(index_+count,rules_.size());return S_OK;}
    HRESULT STDMETHODCALLTYPE Reset()override{index_=0;return S_OK;}
    HRESULT STDMETHODCALLTYPE Clone(IEnumVARIANT**)override{return E_NOTIMPL;}
private:
    ULONG references_{1};std::size_t index_{};std::vector<ComPtr<INetFwRule>> rules_;bool fail_{};
};
class MemoryComRules final:public INetFwRules {
public:
    std::vector<ComPtr<INetFwRule>> rules;
    unsigned additions{},removals{},enumerations{},fail_add{},fail_remove{};
    bool fail_enumeration{};
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out)override{
        if(!out)return E_POINTER;*out=nullptr;
        if(id==IID_IUnknown || id==IID_IDispatch || id==__uuidof(INetFwRules))*out=static_cast<INetFwRules*>(this);
        else return E_NOINTERFACE;AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef()override{return ++references_;}
    ULONG STDMETHODCALLTYPE Release()override{const auto n=--references_;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT*)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT,LCID,ITypeInfo**)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID,LPOLESTR*,UINT,LCID,DISPID*)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE Invoke(DISPID,REFIID,LCID,WORD,DISPPARAMS*,VARIANT*,EXCEPINFO*,UINT*)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE get_Count(long* count)override{if(!count)return E_POINTER;*count=static_cast<long>(rules.size());return S_OK;}
    HRESULT STDMETHODCALLTYPE Add(INetFwRule* rule)override{
        if(++additions==fail_add)return E_ACCESSDENIED;if(!rule)return E_POINTER;rules.push_back(rule);return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Remove(BSTR name)override{
        if(++removals==fail_remove)return E_ACCESSDENIED;
        for(auto i=rules.begin();i!=rules.end();++i){BSTR current{};const auto hr=(*i)->get_Name(&current);
            const bool match=SUCCEEDED(hr) && current && _wcsicmp(current,name)==0;SysFreeString(current);
            if(match){rules.erase(i);return S_OK;}}
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
    HRESULT STDMETHODCALLTYPE Item(BSTR name,INetFwRule** out)override{
        if(!out)return E_POINTER;*out=nullptr;
        for(auto& rule:rules){BSTR current{};const auto hr=rule->get_Name(&current);
            const bool match=SUCCEEDED(hr) && current && _wcsicmp(current,name)==0;SysFreeString(current);
            if(match){*out=rule.Get();(*out)->AddRef();return S_OK;}}
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
    HRESULT STDMETHODCALLTYPE get__NewEnum(IUnknown** out)override{
        if(!out)return E_POINTER;++enumerations;*out=new RuleEnumerator(rules,fail_enumeration);return S_OK;
    }
private:ULONG references_{1};
};
std::vector<vfdual::HostFirewallRuleSnapshot> read_all(MemoryComRules& store){
    std::vector<vfdual::HostFirewallRuleSnapshot> snapshots;
    for(auto& base:store.rules){ComPtr<INetFwRule3> rule;vfdual::HostFirewallRuleSnapshot snapshot;
        require(SUCCEEDED(base.As(&rule)) && vfdual::read_rule_snapshot(rule.Get(),snapshot),"read isolated COM rule");snapshots.push_back(snapshot);}
    return snapshots;
}
void populate(MemoryComRules& store,const std::vector<vfdual::HostFirewallRuleSnapshot>& input){
    for(const auto& snapshot:input){ComPtr<INetFwRule3> rule;
        require(SUCCEEDED(vfdual::create_rule_from_snapshot(snapshot,rule)),"create isolated COM rule object");store.rules.push_back(rule);}
}
}
int main(){
    vfdual::ComApartment apartment;require(apartment.usable(),"COM initialization");
    const std::wstring exe=L"C:\\VF\\VFHost-Local.exe";
    const auto desired=vfdual::build_host_firewall_policy(exe);
    std::vector<vfdual::HostFirewallRuleSnapshot> legacy;
    for(std::size_t i{};i<desired.size();++i){auto s=vfdual::snapshot_from_policy(desired[i]);
        s.name=L"VisionForge"+s.name.substr(2);s.grouping=L"VisionForge Host Transport";
        s.description=i<3?L"VisionForge Host isolated CAT6 transport":L"VisionForge Host wireless LAN UDP transport";legacy.push_back(s);}
    for(unsigned mode{};mode<7;++mode){
        MemoryComRules store;auto initial=legacy;
        if(mode==1){initial.push_back(legacy[1]);initial.push_back(vfdual::snapshot_from_policy(desired[1]));}
        if(mode==2)++initial[1].local_port;
        if(mode==3)initial[1].remote_addresses="*";
        populate(store,initial);const auto before=read_all(store);
        if(mode==4)store.fail_add=3;if(mode==5)store.fail_remove=3;if(mode==6)store.fail_enumeration=true;
        vfdual::ComFirewallRuleStore adapter(&store,exe,desired);
        const auto result=vfdual::reconcile_host_firewall_rule_store(exe,adapter,true);
        const auto after=read_all(store);
        std::cout<<"COM scenario="<<mode<<" status="<<int(result.status)<<" initial="<<before.size()<<" final="<<after.size()<<" detail="<<result.detail<<'\n';
        if(mode<2){require(vfdual::host_firewall_is_ready(result.status),"legacy must reach production COM reconciliation");
            require(after.size()==5,"only five current rules remain");
            for(const auto& s:after){const auto p=std::find_if(desired.begin(),desired.end(),[&](const auto& p){return p.name==s.name;});
                require(p!=desired.end() && vfdual::host_firewall_rule_is_current(*p,s),"all COM roundtrip fields remain narrow/current");}
            const auto added=store.additions,removed=store.removals;
            require(vfdual::reconcile_host_firewall_rule_store(exe,adapter,true).status==vfdual::HostFirewallStatus::ready,"current COM idempotence");
            require(added==store.additions && removed==store.removals,"no idempotent writes");
        }else{require(result.status==vfdual::HostFirewallStatus::failed,"unsafe/fault COM store fails closed");
            require(vfdual::snapshot_multiset_equal(before,after),"original COM snapshot multiset restored");
            if(mode==2 || mode==3 || mode==6)require(store.additions==0 && store.removals==0,"preflight faults never modify store");}
    }
    std::cout<<"PASS production COM enumerate/audit/collect/reconcile with isolated INetFwRules: 7 scenarios, no system Policy access\n";
}
