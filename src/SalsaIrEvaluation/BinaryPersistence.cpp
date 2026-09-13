#include "Integration.h"
#include "StorageSupport.h"
#include <cstring>

namespace salsa::evaluation {
namespace {
using Bytes=std::vector<std::byte>;
enum Table:size_t { Scripts,Sections,Modules,Expressions,Records,Identities,Facts,Order,Members,Evidence,Reserved,Strings,TableCount };
constexpr std::array<size_t,TableCount> Strides{112,48,64,40,56,48,32,8,8,16,8,1};
constexpr size_t Header=64+TableCount*24;
constexpr std::string_view Magic="SALSAIRB";
uint64_t get(std::span<const std::byte> bytes,size_t at,size_t width=8) {
    expect(at<=bytes.size() && width<=bytes.size()-at,"Truncated binary value"); uint64_t value=0;
    for(size_t i=0;i<width;++i) value|=uint64_t(std::to_integer<uint8_t>(bytes[at+i]))<<(i*8); return value;
}
void put(Bytes& bytes,uint64_t value,size_t width=8) { for(size_t i=0;i<width;++i) bytes.push_back(std::byte((value>>(i*8))&255)); }
std::span<const std::byte> selection(std::span<const std::byte> table,uint64_t first,uint64_t count,size_t stride) {
    expect(first<=table.size()/stride && count<=table.size()/stride-first,"Binary selection outside its table");
    return table.subspan(static_cast<size_t>(first)*stride,static_cast<size_t>(count)*stride);
}
void zero(std::span<const std::byte> bytes,size_t at,size_t width) { expect(get(bytes,at,width)==0,"Unsupported binary row flags"); }
std::string stringAt(std::span<const std::byte> row,size_t at,std::span<const std::byte> strings) {
    const auto bytes=selection(strings,get(row,at),get(row,at+8),1); expect(bytes.size()<=1048576,"Persisted text limit exceeded");
    return {reinterpret_cast<const char*>(bytes.data()),bytes.size()};
}
InstructionId instruction(std::span<const std::byte> row,std::span<const std::byte>) { return {get(row,0)}; }
Expression expression(std::span<const std::byte> row,std::span<const std::byte>) {
    const auto op=get(row,36,1),bank=get(row,37,1); expect(op<=uint8_t(ExprOp::Fixed) && bank<=3,"Invalid binary expression tag"); zero(row,38,2);
    return {ExprId{get(row,0)},static_cast<ExprOp>(op),static_cast<Bank>(bank),static_cast<uint32_t>(get(row,32,4)),get(row,8),ExprId{get(row,16)},ExprId{get(row,24)}};
}
Record record(std::span<const std::byte> row,std::span<const std::byte>) {
    const auto op=get(row,52,1),bank=get(row,53,1); expect(op<=uint8_t(Op::Sleep) && bank<=3,"Invalid binary instruction tag"); zero(row,54,2);
    return {InstructionId{get(row,0)},SectionId{get(row,8)},static_cast<Op>(op),static_cast<uint32_t>(get(row,40,4)),get(row,16),static_cast<Bank>(bank),ExprId{get(row,24)},ExprId{get(row,32)},static_cast<uint32_t>(get(row,44,4)),static_cast<uint32_t>(get(row,48,4))};
}
FactIdentity fact(std::span<const std::byte> row,std::span<const std::byte>) {
    const auto kind=get(row,24,1); expect(kind<=5,"Invalid binary fact role"); zero(row,25,7);
    return {FactId{get(row,0)},OperandId{get(row,8)},static_cast<uint8_t>(kind),get(row,16)};
}
IdentityInput identity(std::span<const std::byte> row,std::span<const std::byte> facts) {
    return {InstructionId{get(row,0)},{OperandId{get(row,8)},OperandId{get(row,16)},OperandId{get(row,24)}},
        {selection(facts,get(row,32),get(row,40),32),32,fact}};
}
struct Writer {
    std::array<Bytes,TableCount> tables;
    void text(Bytes& row,std::string_view value) {
        auto& pool=tables[Strings]; put(row,pool.size()); put(row,value.size());
        const auto bytes=std::as_bytes(std::span(value.data(),value.size())); pool.insert(pool.end(),bytes.begin(),bytes.end());
    }
};
}
Bytes encodeBinaryProgram(const Fixture& f) {
    expect(f.checkpoint.has_value(),"Binary save requires a captured checkpoint"); Writer w;
    for(const auto& s:f.scripts) {
        auto& row=w.tables[Scripts]; put(row,s.id.value); w.text(row,s.name); w.text(row,s.source);
        put(row,s.native?(s.native->dreamcast?3:1):0);
        if(s.native) { const auto& n=*s.native; w.text(row,n.dataset); w.text(row,n.asset); w.text(row,n.sha256); w.text(row,n.lineage); }
        else for(size_t i=0;i<4;++i) w.text(row,{});
    }
    for(const auto& s:f.sections) {
        auto& row=w.tables[Sections]; put(row,s.id.value); put(row,s.script.value); w.text(row,s.name);
        put(row,w.tables[Order].size()/8); put(row,s.order.size()); for(auto id:s.order) put(w.tables[Order],id.value);
    }
    for(const auto& m:f.modules) {
        auto& row=w.tables[Modules]; put(row,m.id.value); put(row,m.script.value); w.text(row,m.name); put(row,m.entry.value); put(row,uint8_t(m.role));
        put(row,w.tables[Members].size()/8); put(row,m.sections.size()); for(auto id:m.sections) put(w.tables[Members],id.value);
    }
    for(const auto& e:f.expressions) {
        auto& row=w.tables[Expressions]; put(row,e.id.value); put(row,e.literal); put(row,e.left.value); put(row,e.right.value);
        put(row,e.variable,4); put(row,uint8_t(e.op),1); put(row,uint8_t(e.bank),1); put(row,0,2);
    }
    for(const auto& r:f.records) {
        auto& row=w.tables[Records]; put(row,r.id.value); put(row,r.section.value); put(row,r.value); put(row,r.expression.value); put(row,r.schedule.value);
        put(row,r.variable,4); put(row,r.evidence,4); put(row,r.flags,4); put(row,uint8_t(r.op),1); put(row,uint8_t(r.bank),1); put(row,0,2);
    }
    for(const auto& seed:f.checkpoint->identities) {
        auto& row=w.tables[Identities]; put(row,seed.instruction.value); for(auto id:seed.operands) put(row,id.value);
        put(row,w.tables[Facts].size()/32); put(row,seed.facts.size());
        for(const auto& fact:seed.facts) { auto& out=w.tables[Facts]; put(out,fact.id.value); put(out,fact.operand.value); put(out,fact.role); put(out,fact.kind,1); put(out,0,7); }
    }
    for(const auto& e:f.evidence) w.text(w.tables[Evidence],e);
    for(auto id:f.reserved) put(w.tables[Reserved],id.value);
    size_t size=Header; for(const auto& table:w.tables) size+=table.size();
    Bytes out; out.reserve(size); for(char c:Magic) out.push_back(std::byte(c));
    put(out,1,4); put(out,0x01020304,4); put(out,size); put(out,f.project.high); put(out,f.project.low);
    put(out,f.checkpoint->revision); put(out,f.checkpoint->highWater); put(out,TableCount,4); put(out,0,4);
    size_t offset=Header;
    for(size_t i=0;i<TableCount;++i) { expect(w.tables[i].size()%Strides[i]==0,"Binary writer row mismatch"); put(out,offset); put(out,w.tables[i].size()/Strides[i]); put(out,Strides[i]); offset+=w.tables[i].size(); }
    for(const auto& table:w.tables) out.insert(out.end(),table.begin(),table.end()); return out;
}
BinaryProgramInput viewBinaryProgram(std::span<const std::byte> bytes) {
    expect(bytes.size()>=Header && std::memcmp(bytes.data(),Magic.data(),Magic.size())==0 && get(bytes,8,4)==1 && get(bytes,12,4)==0x01020304,"Unsupported binary IR schema");
    expect(get(bytes,16)==bytes.size() && get(bytes,56,4)==TableCount && get(bytes,60,4)==0,"Invalid binary IR header");
    std::array<std::span<const std::byte>,TableCount> tables; size_t end=Header;
    for(size_t i=0;i<TableCount;++i) {
        const auto offset=get(bytes,64+i*24),count=get(bytes,72+i*24),stride=get(bytes,80+i*24);
        expect(offset==end && stride==Strides[i] && count<=(bytes.size()-end)/stride,"Invalid binary table directory");
        tables[i]=bytes.subspan(end,static_cast<size_t>(count)*stride); end+=tables[i].size();
    }
    expect(end==bytes.size(),"Trailing binary IR data");
    BinaryProgramInput f; f.project={get(bytes,24),get(bytes,32)};
    f.checkpoint=CheckpointInput{get(bytes,40),get(bytes,48),{tables[Identities],48,identity,tables[Facts]}};
    f.records={tables[Records],56,record}; f.expressions={tables[Expressions],40,expression}; f.reserved={tables[Reserved],8,instruction};
    // Only cold metadata is materialized here. Hot rows, authored order, and
    // identity roles remain bounded byte views consumed by normal host loading.
    f.scripts.reserve(tables[Scripts].size()/112);
    for(size_t i=0;i<tables[Scripts].size();i+=112) {
        const auto row=tables[Scripts].subspan(i,112); const auto flags=get(row,40);
        expect(flags==0 || flags==1 || flags==3,"Invalid binary native flags");
        Script s{ScriptId{get(row,0)},stringAt(row,8,tables[Strings]),stringAt(row,24,tables[Strings])};
        std::array<std::string,4> n; for(size_t j=0;j<4;++j) n[j]=stringAt(row,48+j*16,tables[Strings]);
        if(flags) { s.native=NativeSource{std::move(n[0]),std::move(n[1]),std::move(n[2]),std::move(n[3]),flags==3}; validateNativeSource(*s.native); }
        else expect(std::ranges::all_of(n,[](const auto& v){return v.empty();}),"Native metadata without source flag");
        f.scripts.push_back(std::move(s));
    }
    f.sections.reserve(tables[Sections].size()/48);
    for(size_t i=0;i<tables[Sections].size();i+=48) {
        const auto row=tables[Sections].subspan(i,48);
        f.sections.push_back({SectionId{get(row,0)},ScriptId{get(row,8)},stringAt(row,16,tables[Strings]),{selection(tables[Order],get(row,32),get(row,40),8),8,instruction}});
    }
    f.modules.reserve(tables[Modules].size()/64);
    for(size_t i=0;i<tables[Modules].size();i+=64) {
        const auto row=tables[Modules].subspan(i,64),members=selection(tables[Members],get(row,48),get(row,56),8);
        const auto role=get(row,40); expect(role<=uint8_t(ModuleRole::FieldEntry),"Invalid binary module role");
        Module m{ModuleId{get(row,0)},ScriptId{get(row,8)},stringAt(row,16,tables[Strings]),{},InstructionId{get(row,32)},static_cast<ModuleRole>(role)};
        m.sections.reserve(members.size()/8); for(size_t j=0;j<members.size();j+=8) m.sections.push_back(SectionId{get(members,j)}); f.modules.push_back(std::move(m));
    }
    f.evidence.reserve(tables[Evidence].size()/16);
    for(size_t i=0;i<tables[Evidence].size();i+=16) f.evidence.push_back(stringAt(tables[Evidence],i,tables[Strings]));
    return f;
}
} // namespace salsa::evaluation
