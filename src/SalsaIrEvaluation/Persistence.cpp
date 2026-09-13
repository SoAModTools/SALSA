#include "Integration.h"
#include "StorageSupport.h"
#include <SalsaCore/Authoring/SctProjectContainer.h>
#include <charconv>
#include <fstream>
#include <Windows.h>
#include <Psapi.h>

namespace salsa::evaluation {
namespace {
std::string number(uint64_t n) { return std::to_string(n); }
uint64_t integer(const Json& j,uint64_t maximum=UINT64_MAX) {
    const auto& s=j.get_ref<const std::string&>(); uint64_t n{};
    auto r=std::from_chars(s.data(),s.data()+s.size(),n);
    expect(!s.empty() && !(s.size()>1 && s[0]=='0') && r.ec==std::errc{} && r.ptr==s.data()+s.size() && n<=maximum,"Invalid persisted integer"); return n;
}
void row(const Json& j,size_t size) { expect(j.is_array() && j.size()==size,"Invalid persisted row shape"); }
void table(const Json& j) { expect(j.is_array() && j.size()<=1000000,"Invalid persisted table size"); }
template<class T> Json ids(const std::vector<T>& values) { Json out=Json::array(); for(auto id:values) out.push_back(number(id.value)); return out; }
template<class T> std::vector<T> readIds(const Json& j) { table(j); std::vector<T> out; out.reserve(j.size()); for(const auto& id:j) out.push_back(T{integer(id)}); return out; }
std::string text(const Json& j) { auto s=j.get<std::string>(); expect(s.size()<=1048576,"Persisted text limit exceeded"); return s; }
template<class T> T take(core::Result<T> result) { if(!result) throw std::runtime_error(result.diagnostics().front().message); return std::move(result).takeValue(); }
uint64_t elapsed(Clock::time_point t) { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-t).count(); }
}
void validateNativeSource(const NativeSource& n) {
    const auto hex=[](const std::string& v){ return v.size()==64 && std::ranges::all_of(v,[](char c){return (c>='0' && c<='9') || (c>='a' && c<='f');}); };
    expect(!n.dataset.empty() && !n.asset.empty() && n.asset.front()!='/' && n.asset.find_first_of("\\:")==std::string::npos && n.asset.find("..") == std::string::npos && hex(n.sha256) && hex(n.lineage),"Invalid native source qualification");
}
Json encodeProgram(const Fixture& f) {
    expect(f.checkpoint.has_value(),"Save requires a captured program checkpoint");
    Json j={{"format","jahorta.salsa.ir-program"},{"version",1},{"project",{number(f.project.high),number(f.project.low)}},
        {"revision",number(f.checkpoint->revision)},{"high_water",number(f.checkpoint->highWater)},
        {"derived_cache","rebuild"},{"history","not-saved"},{"scripts",Json::array()},{"sections",Json::array()},
        {"modules",Json::array()},{"expressions",Json::array()},{"evidence",f.evidence},{"reserved",ids(f.reserved)},
        {"records",Json::array()},{"identities",Json::array()}};
    for(const auto& s:f.scripts) {
        Json native=nullptr;
        if(s.native) { const auto& n=*s.native; native=Json::array({n.dataset,n.asset,n.sha256,n.lineage,n.dreamcast}); }
        j["scripts"].push_back({number(s.id.value),s.name,s.source,native});
    }
    for(const auto& s:f.sections) j["sections"].push_back({number(s.id.value),number(s.script.value),s.name,ids(s.order)});
    for(const auto& m:f.modules) j["modules"].push_back({number(m.id.value),number(m.script.value),m.name,ids(m.sections),number(m.entry.value),number(static_cast<uint8_t>(m.role))});
    for(const auto& e:f.expressions) j["expressions"].push_back({number(e.id.value),number(static_cast<uint8_t>(e.op)),number(static_cast<uint8_t>(e.bank)),number(e.variable),number(e.literal),number(e.left.value),number(e.right.value)});
    for(const auto& r:f.records) j["records"].push_back({number(r.id.value),number(r.section.value),number(static_cast<uint8_t>(r.op)),number(r.variable),number(r.value),number(static_cast<uint8_t>(r.bank)),number(r.expression.value),number(r.schedule.value),number(r.evidence),number(r.flags)});
    for(const auto& seed:f.checkpoint->identities) {
        Json facts=Json::array(); for(const auto& fact:seed.facts) facts.push_back({number(fact.id.value),number(fact.operand.value),number(fact.kind),number(fact.role)});
        j["identities"].push_back({number(seed.instruction.value),{number(seed.operands[0].value),number(seed.operands[1].value),number(seed.operands[2].value)},facts});
    }
    return j;
}
Fixture decodeProgram(const Json& j) {
    expect(j.is_object() && j.size()==15 && j.at("format")=="jahorta.salsa.ir-program" && j.at("version")==1,"Unsupported IR program schema");
    expect(j.at("derived_cache")=="rebuild" && j.at("history")=="not-saved","Unsupported cache/history persistence policy");
    Fixture f; row(j.at("project"),2); f.project={integer(j["project"][0]),integer(j["project"][1])};
    f.checkpoint=Checkpoint{integer(j.at("revision")),integer(j.at("high_water")),{}};
    for(const auto key:{"scripts","sections","modules","expressions","evidence","reserved","records","identities"}) table(j.at(key));
    for(const auto& a:j["scripts"]) {
        row(a,4); Script s{ScriptId{integer(a[0])},text(a[1]),text(a[2])};
        if(!a[3].is_null()) {
            const auto& n=a[3]; row(n,5); s.native=NativeSource{text(n[0]),text(n[1]),text(n[2]),text(n[3]),n[4].get<bool>()};
            validateNativeSource(*s.native);
        }
        f.scripts.push_back(std::move(s));
    }
    for(const auto& a:j["sections"]) { row(a,4); f.sections.push_back({SectionId{integer(a[0])},ScriptId{integer(a[1])},text(a[2]),readIds<InstructionId>(a[3])}); }
    for(const auto& a:j["modules"]) { row(a,6); f.modules.push_back({ModuleId{integer(a[0])},ScriptId{integer(a[1])},text(a[2]),readIds<SectionId>(a[3]),InstructionId{integer(a[4])},static_cast<ModuleRole>(integer(a[5],static_cast<uint8_t>(ModuleRole::FieldEntry)))}); }
    for(const auto& a:j["expressions"]) { row(a,7); f.expressions.push_back({ExprId{integer(a[0])},static_cast<ExprOp>(integer(a[1],static_cast<uint8_t>(ExprOp::Fixed))),static_cast<Bank>(integer(a[2],3)),static_cast<uint32_t>(integer(a[3],UINT32_MAX)),integer(a[4]),ExprId{integer(a[5])},ExprId{integer(a[6])}}); }
    for(const auto& a:j["evidence"]) f.evidence.push_back(text(a));
    f.reserved=readIds<InstructionId>(j["reserved"]); f.records.reserve(j["records"].size());
    for(const auto& a:j["records"]) {
        row(a,10); f.records.push_back({InstructionId{integer(a[0])},SectionId{integer(a[1])},static_cast<Op>(integer(a[2],static_cast<uint8_t>(Op::Sleep))),static_cast<uint32_t>(integer(a[3],UINT32_MAX)),integer(a[4]),static_cast<Bank>(integer(a[5],3)),ExprId{integer(a[6])},ExprId{integer(a[7])},static_cast<uint32_t>(integer(a[8],UINT32_MAX)),static_cast<uint32_t>(integer(a[9],UINT32_MAX))});
    }
    f.checkpoint->identities.reserve(j["identities"].size());
    for(const auto& a:j["identities"]) {
        row(a,3); row(a[1],3); table(a[2]); RecordIdentity seed{InstructionId{integer(a[0])},{OperandId{integer(a[1][0])},OperandId{integer(a[1][1])},OperandId{integer(a[1][2])}},{}};
        for(const auto& v:a[2]) { row(v,4); seed.facts.push_back({FactId{integer(v[0])},OperandId{integer(v[1])},static_cast<uint8_t>(integer(v[2],5)),integer(v[3])}); }
        f.checkpoint->identities.push_back(std::move(seed));
    }
    return f; // Normal host load performs structural validation while building the model/indexes.
}
std::vector<std::byte> packProgram(std::string_view json) {
    core::SctProjectRecords records; auto bytes=std::as_bytes(std::span(json.data(),json.size()));
    records["ir/program.json"]={bytes.begin(),bytes.end()}; return take(core::SctProjectContainer::encode(records));
}
std::string unpackProgram(std::span<const std::byte> container) {
    auto records=take(core::SctProjectContainer::decode(container));
    expect(records.size()==1 && records.contains("ir/program.json"),"Not an experimental IR container"); const auto& bytes=records.at("ir/program.json");
    return {reinterpret_cast<const char*>(bytes.data()),bytes.size()};
}
std::vector<std::byte> packBinaryProgram(std::vector<std::byte> bytes) {
    core::SctProjectRecords records; records["ir/program.bin"]=std::move(bytes); return take(core::SctProjectContainer::encode(records));
}
std::vector<std::byte> unpackBinaryProgram(std::span<const std::byte> container) {
    auto records=take(core::SctProjectContainer::decode(container));
    expect(records.size()==1 && records.contains("ir/program.bin"),"Not an experimental binary IR container");
    return std::move(records.at("ir/program.bin"));
}
std::vector<std::byte> fileRoundTrip(std::span<const std::byte> bytes,Json& measurements) {
    const auto folder=std::filesystem::temp_directory_path()/("salsa-ir-io-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(Clock::now().time_since_epoch().count()));
    expect(std::filesystem::create_directory(folder),"Cannot create temporary IR I/O directory");
    const auto path=folder/"project.salsair";
    struct Cleanup { std::filesystem::path file,folder; ~Cleanup(){ std::error_code ec; std::filesystem::remove(file,ec); std::filesystem::remove(folder,ec); } } cleanup{path,folder};
    auto start=Clock::now();
    { std::ofstream out(path,std::ios::binary); out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size())); out.close(); expect(bool(out),"Cannot write experimental container"); }
    measurements["file_write_ns"]=number(elapsed(start)); start=Clock::now();
    std::ifstream in(path,std::ios::binary|std::ios::ate); expect(bool(in),"Cannot read experimental container");
    const auto size=in.tellg(); expect(size>=0 && static_cast<uint64_t>(size)<=core::SctProjectContainer::MaximumContainerBytes,"Invalid container file size");
    std::vector<std::byte> result(static_cast<size_t>(size)); in.seekg(0);
    in.read(reinterpret_cast<char*>(result.data()),size); expect(bool(in),"Incomplete experimental container read");
    measurements["file_read_ns"]=number(elapsed(start)); return result;
}
Json processMemory() {
    PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb=sizeof(memory);
    if(!GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory))) return nullptr;
    return {{"private_bytes",number(memory.PrivateUsage)},{"working_set_bytes",number(memory.WorkingSetSize)},{"peak_working_set_bytes",number(memory.PeakWorkingSetSize)},
        {"peak_pagefile_bytes",number(memory.PeakPagefileUsage)},{"scope","Windows process-wide cumulative peaks, including fixture/native/JSON/transient state; not exact temporary object accounting"}};
}
} // namespace salsa::evaluation
