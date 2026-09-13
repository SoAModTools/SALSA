#include "Corpus.h"
#include "Integration.h"
#include "PackedStorage.h"
#include "PagedStorage.h"
#include <SpiceSCT/SctDocumentImporter.h>
#include <SpiceSCT/SctParser.h>
#include <SpiceSCT/SctInstructionFactory.h>
#include <SalsaCore/Authoring/SctProjectContainer.h>
#include <SalsaCore/Foundation/Hashing.h>
#include <fstream>
#include <iostream>
#include <unordered_map>
#include <set>
#include <bit>
#include <cmath>
#include <iomanip>

namespace salsa::evaluation::corpus {
namespace {
namespace sct=spice::sct;
std::filesystem::path utf8Path(std::string_view s){return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()),s.size()));}
uint64_t ns(Clock::time_point start) { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count(); }
std::string hash(std::span<const std::byte> bytes) { auto h=core::sha256(bytes); expect(h.ok(),"Cannot hash corpus input"); return h.value().toHex(); }
std::string hash(std::string_view text) { return hash(std::as_bytes(std::span(text.data(),text.size()))); }
struct Random { uint64_t state; uint64_t next(){ state+=0x9e3779b97f4a7c15ULL; auto v=state; v=(v^(v>>30))*0xbf58476d1ce4e5b9ULL; v=(v^(v>>27))*0x94d049bb133111ebULL; return v^(v>>31); } };
// Interpret only a single fixed literal, preserving its original representation.
std::optional<uint32_t> fixedBitIndex(const Pool& pool,uint32_t expression) {
    if(pool.nodes.at(expression).kind!=Kind::Expression) return {};
    const auto values=pool.child(expression); if(values.size()!=1) return {};
    const auto& v=pool.nodes[values.front()]; if(v.kind!=Kind::Value) return {};
    if(v.aux==static_cast<uint32_t>(sct::SctScptValueKind::DecimalLiteral)) {
        if(!pool.child(values.front()).empty() || (v.value&0x8000ff)!=0) return {};
        return static_cast<uint32_t>((v.value>>8)&0x7fff);
    }
    if(v.aux==static_cast<uint32_t>(sct::SctScptValueKind::FloatLiteral)) {
        const auto words=pool.child(values.front());
        if(words.size()!=1 || pool.nodes[words.front()].kind!=Kind::Word) return {};
        const double value=std::bit_cast<float>(static_cast<uint32_t>(pool.nodes[words.front()].value));
        if(std::isfinite(value) && value>=0 && value<=UINT32_MAX && std::floor(value)==value)
            return static_cast<uint32_t>(value);
    }
    return {}; // Computed indices and special inline sentinels are not fixed targets.
}
struct Site { InstructionId instruction; std::vector<uint32_t> path; };
struct Command {
    std::string kind; InstructionId instruction,target; std::vector<uint32_t> path; uint64_t value{};
};
struct Change {
    std::optional<StoredRecord> before,after; uint32_t section{};
    std::shared_ptr<const std::vector<InstructionId>> orderBefore,orderAfter;
    std::string nameBefore,nameAfter;
};
template<class Storage> class Host {
public:
    Model& m; Context& c; Storage store; InstructionDirectory directory; ProgramWork work; FactIndex index;
    std::vector<std::shared_ptr<const std::vector<InstructionId>>> orders;
    std::map<SectionId,uint32_t> sections; std::vector<Change> history; std::pmr::vector<Fact> factScratch;
    uint64_t factHighWater{},sourcesExamined{},orderCopied{};
    Host(Model& model,Context& context):m(model),c(context),store(c),directory(c.resource(Domain::Directory)),index(c.resource(Domain::Index),work,directory),factScratch(c.resource(Domain::Preparation)){factScratch.reserve(16);}
    void load() {
        std::vector<SectionLayout> layouts;
        for(uint32_t i=0;i<m.sections.size();++i) {
            auto& section=m.sections[i]; expect(sections.emplace(section.id,i).second,"Duplicate section ID");
            layouts.push_back({section.id,section.order.size()}); orders.push_back(std::make_shared<const std::vector<InstructionId>>(std::move(section.order)));
            for(auto id:*orders.back()) { auto& state=directory.ensure(id); expect(!state.live,"Duplicate instruction in order"); state.live=true; state.section=i; }
        }
        auto result=store.load(m.records,layouts); if(!result) throw std::runtime_error(result.diagnostics().front().message);
        size_t count=0; for(const auto& ids:orders) count+=ids->size(); expect(count==m.records.size(),"Incomplete corpus order");
        for(const auto& row:m.records) expect(directory.find(row.id) && m.sections[directory.find(row.id)->section].id==row.section,"Corpus owner mismatch");
        index.reserve(m.records.size()*3);
        for(const auto& row:m.records) refresh(row.id);
        std::pmr::vector<StoredRecord> empty(c.resource(Domain::Preparation)); m.records.swap(empty); sourcesExamined=0; work={};
    }
    template<class Fn> void walk(uint32_t root,Fn&& fn) const {
        fn(root); for(auto child:m.pool.child(root)) walk(child,fn);
    }
    void refresh(InstructionId id) {
        auto& state=directory.ensure(id); auto& facts=factScratch;facts.clear();
        if(const auto* row=store.lookup(id)) {
            ++sourcesExamined; uint64_t ordinal=0; const auto root=m.pool.nodes.at(row->firstOperand);
            walk(row->firstOperand,[&](uint32_t at){ const auto& n=m.pool.nodes[at]; ++ordinal;
                auto emit=[&](FactKind kind,Bank bank,uint64_t key,uint64_t value=0){
                    Fact f{{},id,OperandId{id.value},kind,ordinal,bank,key,value};
                    index.source(state.head,[&](const Fact& old){if(sameRole(old,f)) f.id=old.id;});
                    if(!f.id.value) f.id=FactId{++factHighWater}; facts.push_back(f);
                };
                if(n.kind==Kind::InstructionRef) { expect(directory.find(InstructionId{n.value}) && directory.find(InstructionId{n.value})->live,"Dangling corpus target"); emit(FactKind::Call,Bank::Bit,n.value); }
                else if(n.kind==Kind::Unresolved || n.kind==Kind::Gap) emit(FactKind::Unknown,Bank::Bit,0);
                else if(n.kind==Kind::Value) {
                    const auto kind=static_cast<sct::SctScptValueKind>(n.aux); std::optional<Bank> bank;
                    if(kind==sct::SctScptValueKind::BitVariable) bank=Bank::Bit;
                    if(kind==sct::SctScptValueKind::ByteVariable) bank=Bank::Byte;
                    if(kind==sct::SctScptValueKind::FloatVariable) bank=Bank::Float;
                    if(kind==sct::SctScptValueKind::IntegerVariable || kind==sct::SctScptValueKind::IntegerVariableLow16Comparison || kind==sct::SctScptValueKind::FloatBackedIntegerVariable) bank=Bank::Int;
                    if(bank) emit(FactKind::Read,*bank,n.value&(*bank==Bank::Bit?0x1fffffff:*bank==Bank::Int?0x00ffffff:0x0fffffff));
                }
            });
            // Exact, bounded effect recognition. Other opcodes remain fully stored.
            if(root.value==17 || root.value==18) {
                for(auto p:m.pool.child(row->firstOperand)) if(m.pool.nodes[p].kind==Kind::Parameter && m.pool.nodes[p].value==0) {
                    auto expression=m.pool.child(p); if(expression.size()!=1) continue;
                    if(auto key=fixedBitIndex(m.pool,expression[0])) {
                        Fact f{{},id,OperandId{id.value},FactKind::Write,0,Bank::Bit,*key,root.value==17};
                        index.source(state.head,[&](const Fact& old){if(sameRole(old,f)) f.id=old.id;}); if(!f.id.value) f.id=FactId{++factHighWater}; facts.push_back(f);
                    }
                }
            }
        }
        index.prepareBuckets(facts); index.reserve(facts.size()); index.apply(state.head,facts);
    }
    void publish(const Change& change,bool forward) {
        auto before=forward?change.before:change.after,after=forward?change.after:change.before;
        if(before || after) {
            StorageChange mutation{before,after}; auto prepared=store.prepare(std::span(&mutation,1));
            if(!prepared) throw std::runtime_error(prepared.diagnostics().front().message);
            store.apply(std::move(prepared).takeValue());
            auto id=after?after->id:before->id; auto& state=directory.ensure(id); state.live=after.has_value(); state.section=change.section; refresh(id);
        }
        if(change.orderBefore) orders[change.section]=forward?change.orderAfter:change.orderBefore;
        if(!change.nameBefore.empty()) m.sections[change.section].name=forward?change.nameAfter:change.nameBefore;
        ++m.revision;
    }
    void apply(const Command& command) {
        auto* row=store.lookup(command.instruction); expect(row!=nullptr,"Missing corpus edit source");
        Change change; change.section=sections.at(row->section);
        if(command.kind=="rename") {
            const auto& name=m.sections[change.section].name; expect(name!="init" && name!="loop","Cannot rename required root");
            change.nameBefore=name; change.nameAfter="ir-survey-"+std::to_string(command.value);
            while(std::ranges::any_of(m.sections,[&](const auto& s){return s.script==m.sections[change.section].script && s.name==change.nameAfter;})) change.nameAfter+="-";
        } else if(command.kind=="delete-inserted") {
            bool incoming=false; index.query({FactKind::Call,Bank::Bit,row->id.value},[&](const Fact&){incoming=true;}); expect(!incoming,"Cannot delete referenced instruction");
            change.before=*row; change.orderBefore=orders[change.section]; auto order=std::make_shared<std::vector<InstructionId>>(*change.orderBefore);
            std::erase(*order,row->id); orderCopied+=order->size(); change.orderAfter=order;
        } else {
            const auto original=*row; StoredRecord modified=original; modified.firstOperand=m.pool.clone(original.firstOperand,m.pool);
            if(command.kind=="insert") {
                modified.id=InstructionId{++m.highWater}; change.orderBefore=orders[change.section]; auto order=std::make_shared<std::vector<InstructionId>>(*change.orderBefore);
                auto position=std::ranges::find(*order,original.id); expect(position!=order->end(),"Missing insertion position"); order->insert(position,modified.id);
                orderCopied+=order->size(); change.orderAfter=order;
            } else {
                change.before=original; auto at=m.pool.atPath(modified.firstOperand,command.path); auto& n=m.pool.nodes[at];
                if(command.kind=="bit-write") { expect(n.kind==Kind::Instruction && (n.value==17 || n.value==18),"Invalid bit write edit"); n.value=n.value==17?18:17; }
                else if(command.kind=="bit-read") { expect(n.kind==Kind::Value && n.aux==static_cast<uint32_t>(sct::SctScptValueKind::BitVariable),"Invalid expression edit"); auto replacement=sct::SctExpressionFactory::bitVariable(static_cast<uint32_t>(command.value));expect(replacement.expression.has_value(),"Unrepresentable bit index");n.value=std::get<sct::SctScptValueOperation>(std::get<sct::SctTypedScptProgram>(replacement.expression->body).operations.front()).encodingWord; }
                else if(command.kind=="retarget") { expect(n.kind==Kind::InstructionRef,"Invalid target edit"); const auto* target=store.lookup(command.target); expect(target && m.sections[sections.at(target->section)].script==m.sections[change.section].script,"Cross-script native target"); n.value=command.target.value; }
                else throw std::runtime_error("Unknown corpus edit kind");
            }
            change.after=modified;
        }
        publish(change,true); history.push_back(std::move(change));
    }
    uint64_t query(Bank bank,uint64_t key) const {
        uint64_t digest=0; for(auto kind:{FactKind::Read,FactKind::Write}) index.query({kind,bank,key},[&](const Fact& f){digest+=f.source.value*131+f.key+f.value+static_cast<uint64_t>(kind);}); return digest;
    }
};

// Explicit binary checkpoint, shared by all candidates. No JSON/native documents
// in the reload path. Save compacts immutable payload roots, independently of history.
struct Binary {
    std::vector<std::byte> bytes; size_t cursor{};
    void put(uint64_t n,unsigned width=8){expect(width==8 || n<(uint64_t{1}<<(width*8)),"Checkpoint integer overflow");for(unsigned i=0;i<width;++i) bytes.push_back(std::byte((n>>(i*8))&255));}
    uint64_t get(unsigned width=8){expect(cursor<=bytes.size() && bytes.size()-cursor>=width,"Truncated corpus checkpoint"); uint64_t n=0;for(unsigned i=0;i<width;++i)n|=uint64_t(std::to_integer<uint8_t>(bytes[cursor++]))<<(i*8);return n;}
    void text(std::string_view s){put(s.size());auto b=std::as_bytes(std::span(s.data(),s.size()));bytes.insert(bytes.end(),b.begin(),b.end());}
    std::string text(){auto n=get();expect(n<=bytes.size()-cursor,"Invalid checkpoint string");std::string s(reinterpret_cast<const char*>(bytes.data()+cursor),static_cast<size_t>(n));cursor+=static_cast<size_t>(n);return s;}
    size_t count(){auto n=get();expect(n<=bytes.size()/8,"Invalid checkpoint count");return static_cast<size_t>(n);}
};
using CheckpointTables=std::map<std::string,Binary>;
constexpr std::string_view TableNames[]={"metadata","nodes","children","text"};
uint64_t checkpointSize(const CheckpointTables& tables){uint64_t size=0;for(const auto& [name,b]:tables)size+=b.bytes.size();return size;}
std::string checkpointHash(const CheckpointTables& tables){std::string identity;for(const auto& [name,b]:tables)identity+=name+":"+std::to_string(b.bytes.size())+":"+hash(b.bytes)+"\n";return hash(identity);}
std::string chunkName(std::string_view table,size_t index){std::ostringstream out;out<<"corpus-v2/"<<table<<"/"<<std::setw(6)<<std::setfill('0')<<index;return out.str();}
// Bound each uncompressed record independently; production container limits stay unchanged.
std::vector<std::byte> packCheckpoint(CheckpointTables tables,size_t chunkBytes=64u<<20) {
    expect(chunkBytes>0 && chunkBytes<=core::SctProjectContainer::MaximumRecordBytes,"Invalid checkpoint chunk size");
    core::SctProjectRecords records;
    for(auto& [name,b]:tables){size_t part=0;for(size_t offset=0;offset<b.bytes.size();offset+=chunkBytes){const auto length=std::min(chunkBytes,b.bytes.size()-offset);records[chunkName(name,part++)]={b.bytes.begin()+offset,b.bytes.begin()+offset+length};}
        if(!part)records[chunkName(name,0)]={};std::vector<std::byte>().swap(b.bytes);}
    auto result=core::SctProjectContainer::encode(records);if(!result)throw std::runtime_error(result.diagnostics().front().message);return std::move(result).takeValue();
}
CheckpointTables unpackCheckpoint(std::span<const std::byte> bytes) {
    auto decoded=core::SctProjectContainer::decode(bytes);if(!decoded)throw std::runtime_error(decoded.diagnostics().front().message);
    auto records=std::move(decoded).takeValue();CheckpointTables tables;
    for(auto name:TableNames){auto& b=tables[std::string(name)];size_t part=0;for(;;++part){auto it=records.find(chunkName(name,part));if(it==records.end())break;b.bytes.insert(b.bytes.end(),it->second.begin(),it->second.end());records.erase(it);}expect(part!=0,"Missing corpus checkpoint table");}
    expect(records.empty(),"Unknown or noncontiguous corpus checkpoint records");return tables;
}
template<class Storage> CheckpointTables save(const Host<Storage>& h) {
    Context context; Model compact(context); auto& m=h.m; CheckpointTables tables;
    auto& b=tables["metadata"];b.put(0x32524943534c4153ULL); b.put(m.highWater); b.put(m.revision);
    b.put(m.scripts.size()); for(const auto& s:m.scripts) {b.put(s.id.value);b.text(s.name);b.text(s.source);}
    b.put(m.sections.size());
    for(size_t i=0;i<m.sections.size();++i) {
        const auto& s=m.sections[i];b.put(s.id.value);b.put(s.script.value);b.text(s.name);
        b.put(s.content==NoSlot?NoSlot:m.pool.clone(s.content,compact.pool),4); b.put(h.orders[i]->size());
        for(auto id:*h.orders[i]) {const auto& r=*h.store.lookup(id);b.put(id.value);b.put(m.pool.clone(r.firstOperand,compact.pool),4);}
    }
    b.put(m.texts.size());for(const auto& t:m.texts){b.put(t.id);b.put(t.script.value);b.put(m.pool.clone(t.root,compact.pool),4);b.put(t.kind,1);}
    auto& nodes=tables["nodes"];nodes.bytes.reserve(compact.pool.nodes.size()*21);
    for(const auto& n:compact.pool.nodes){nodes.put(static_cast<uint64_t>(n.kind),1);nodes.put(n.value);nodes.put(n.aux,4);nodes.put(n.first,4);nodes.put(n.count,4);}
    auto& children=tables["children"];children.bytes.reserve(compact.pool.children.size()*4);for(auto child:compact.pool.children)children.put(child,4);
    auto text=std::as_bytes(std::span(compact.pool.text));tables["text"].bytes.assign(text.begin(),text.end());return tables;
}
void restore(CheckpointTables& tables,Model& m) {
    auto& b=tables.at("metadata");expect(b.get()==0x32524943534c4153ULL,"Unsupported corpus checkpoint");m.highWater=b.get();m.revision=b.get();
    for(size_t n=b.count();n;--n){Script s;s.id=ScriptId{b.get()};s.name=b.text();s.source=b.text();m.scripts.push_back(std::move(s));}
    for(size_t n=b.count();n;--n){SectionInfo s;s.id=SectionId{b.get()};s.script=ScriptId{b.get()};s.name=b.text();s.content=static_cast<uint32_t>(b.get(4));
        for(size_t k=b.count();k;--k){InstructionId id{b.get()};const auto root=static_cast<uint32_t>(b.get(4));s.order.push_back(id);m.records.push_back({id,s.id,Op::Unknown,root,0});}m.sections.push_back(std::move(s));}
    for(size_t n=b.count();n;--n){TextInfo t;t.id=b.get();t.script=ScriptId{b.get()};t.root=static_cast<uint32_t>(b.get(4));t.kind=static_cast<uint32_t>(b.get(1));expect(t.kind<=1,"Invalid text record");m.texts.push_back(t);}
    expect(b.cursor==b.bytes.size(),"Trailing checkpoint metadata");
    auto& nodes=tables.at("nodes");auto& children=tables.at("children");expect(nodes.bytes.size()%21==0 && children.bytes.size()%4==0,"Truncated checkpoint table");
    slotCount(nodes.bytes.size()/21);slotCount(children.bytes.size()/4);m.pool.nodes.reserve(nodes.bytes.size()/21);m.pool.children.reserve(children.bytes.size()/4);
    while(children.cursor<children.bytes.size())m.pool.children.push_back(static_cast<uint32_t>(children.get(4)));
    while(nodes.cursor<nodes.bytes.size()){Node n;const auto kind=nodes.get(1);expect(kind<=static_cast<uint64_t>(Kind::Marker),"Invalid checkpoint node kind");n.kind=static_cast<Kind>(kind);n.value=nodes.get();n.aux=static_cast<uint32_t>(nodes.get(4));n.first=static_cast<uint32_t>(nodes.get(4));n.count=static_cast<uint32_t>(nodes.get(4));m.pool.nodes.push_back(n);const auto index=static_cast<uint32_t>(m.pool.nodes.size()-1);for(auto child:m.pool.child(index))expect(child<index,"Cyclic/dangling checkpoint node");}
    const auto& text=tables.at("text").bytes;m.pool.text.assign(reinterpret_cast<const char*>(text.data()),reinterpret_cast<const char*>(text.data())+text.size());
    for(const auto& s:m.sections)expect(s.content==NoSlot || s.content<m.pool.nodes.size(),"Invalid section payload");
    for(const auto& r:m.records)expect(r.firstOperand<m.pool.nodes.size(),"Invalid instruction payload");
    for(const auto& t:m.texts)expect(t.root<m.pool.nodes.size(),"Invalid text payload");
}

template<class Storage> Json experiment(Model& m,Context& c,Json& report,uint64_t seed,uint64_t editCount,Controls controls,Clock::time_point loadStart) {
    const auto stop=[&](){if(controls.stopRequested())throw std::runtime_error("Corpus operation cancelled");};
    Host<Storage> h(m,c);auto start=Clock::now();h.load();report["index_and_storage_ns"]=std::to_string(ns(start));report["load_to_ready_ns"]=std::to_string(ns(loadStart));report["ready_memory"]=processMemory();
    report["flat_storage"]={{"nodes",m.pool.nodes.size()},{"node_capacity",m.pool.nodes.capacity()},{"node_bytes",sizeof(Node)},{"child_indices",m.pool.children.size()},{"child_capacity",m.pool.children.capacity()},{"text_bytes",m.pool.text.size()},{"text_capacity",m.pool.text.capacity()}};
    std::map<std::string,uint64_t> opcodes;uint64_t readFacts=0,writeFacts=0,references=0,unknownFacts=0;
    h.store.forEachPhysical([&](const StoredRecord& r){++opcodes[std::to_string(m.pool.nodes[r.firstOperand].value)];h.index.source(h.directory.find(r.id)->head,[&](const Fact& f){readFacts+=f.kind==FactKind::Read;writeFacts+=f.kind==FactKind::Write;references+=f.kind==FactKind::Call;unknownFacts+=f.kind==FactKind::Unknown;});});
    report["unrecognized_bit_write_targets"]=opcodes["17"]+opcodes["18"]-writeFacts;report["opcodes"]=opcodes;report["direct_index"]={{"reads",readFacts},{"fixed_bit_writes",writeFacts},{"instruction_references",references},{"unknown",unknownFacts}};
    std::vector<InstructionId> writes,renameable;std::vector<Site> reads,targets;std::map<ScriptId,std::vector<InstructionId>> entries;
    for(size_t s=0;s<m.sections.size();++s){const auto& section=m.sections[s];if(h.orders[s]->empty())continue;entries[section.script].push_back(h.orders[s]->front());if(!section.name.empty() && section.name!="init" && section.name!="loop")renameable.push_back(h.orders[s]->front());
        for(auto id:*h.orders[s]){const auto& r=*h.store.lookup(id);const auto root=m.pool.nodes[r.firstOperand];
            if((root.value==17 || root.value==18) && !(root.aux&2)) {bool fixed=false;h.index.source(h.directory.find(id)->head,[&](const Fact& f){fixed|=f.kind==FactKind::Write;});if(fixed)writes.push_back(id);}
            std::vector<uint32_t> path;auto visit=[&](auto&& self,uint32_t node)->void{const auto& n=m.pool.nodes[node];if(n.kind==Kind::InstructionRef)targets.push_back({id,path});if(n.kind==Kind::Value && n.aux==static_cast<uint32_t>(sct::SctScptValueKind::BitVariable))reads.push_back({id,path});
                uint32_t ordinal=0;for(auto child:m.pool.child(node)){path.push_back(ordinal++);self(self,child);path.pop_back();}};visit(visit,r.firstOperand);
        }
    }
    report["eligible"]={{"bit_writes",writes.size()},{"bit_reads",reads.size()},{"target_operands",targets.size()},{"renameable_sections",renameable.size()}};
    std::map<uint64_t,size_t> readUses,targetUses;
    for(const auto& site:reads) ++readUses[m.pool.nodes[m.pool.atPath(h.store.lookup(site.instruction)->firstOperand,site.path)].value&0x1fffffff];
    for(const auto& site:targets) ++targetUses[m.pool.nodes[m.pool.atPath(h.store.lookup(site.instruction)->firstOperand,site.path)].value];
    std::vector<Site> hotReads,hotTargets;
    const auto hottest=[](const auto& counts){uint64_t key=0;size_t maximum=0;for(auto [k,n]:counts)if(n>maximum){key=k;maximum=n;}return key;};
    const auto hotBit=hottest(readUses),hotTarget=hottest(targetUses);
    for(const auto& site:reads)if((m.pool.nodes[m.pool.atPath(h.store.lookup(site.instruction)->firstOperand,site.path)].value&0x1fffffff)==hotBit)hotReads.push_back(site);
    for(const auto& site:targets)if(m.pool.nodes[m.pool.atPath(h.store.lookup(site.instruction)->firstOperand,site.path)].value==hotTarget)hotTargets.push_back(site);
    report["pressure_sites"]={{"hot_bit",hotBit},{"hot_bit_initial_uses",readUses[hotBit]},{"hot_target",hotTarget},{"hot_target_initial_uses",targetUses[hotTarget]}};
    Random rng{seed};Json trace=Json::array(),samples=Json::array();uint64_t queryDigest=0;std::vector<InstructionId> inserted;
    // Every fourth choice stresses a longest section; remaining choices sample
    // eligible sites uniformly. The seed, source ordering and schedule are portable.
    size_t longest=0;for(size_t s=1;s<h.orders.size();++s)if(h.orders[s]->size()>h.orders[longest]->size())longest=s;
    auto choose=[&](const auto& values,uint64_t iteration)->const auto& {expect(!values.empty(),"No eligible corpus site");auto index=rng.next()%values.size();if(iteration%4==0)for(size_t n=0;n<values.size();++n){const auto& v=values[(index+n)%values.size()];InstructionId id;if constexpr(std::is_same_v<std::decay_t<decltype(v)>,InstructionId>)id=v;else id=v.instruction;if(h.directory.find(id)->section==longest){index=(index+n)%values.size();break;}}return values[index];};
    report["edit_catalog_memory"]=processMemory();
    uint64_t skipped=0,batches=0;std::map<std::string,uint64_t> skipReasons;
    for(uint64_t i=0;i<editCount;++i){stop();Command command;auto kind=i%6;
        if(kind==0 && !writes.empty()){command.kind="bit-write";command.instruction=choose(writes,i);}
        else if(kind==1 && !reads.empty()){command.kind="bit-read";auto site=choose((i/6)%2==0 && !hotReads.empty()?hotReads:reads,i);command.instruction=site.instruction;command.path=site.path;command.value=(i/6)%2==0?rng.next()%32768:hotBit;}
        else if(kind==2 && !targets.empty()){command.kind="retarget";auto site=choose(i%4==2 && !hotTargets.empty()?hotTargets:targets,i);command.instruction=site.instruction;command.path=site.path;const auto& section=m.sections[h.directory.find(site.instruction)->section];const auto& candidates=entries.at(section.script);command.target=candidates[rng.next()%candidates.size()];}
        else if(kind==3 && !writes.empty()){command.kind="insert";command.instruction=choose(writes,i);}
        else if(kind==4 && !inserted.empty()){command.kind="delete-inserted";command.instruction=inserted.back();inserted.pop_back();}
        else if(kind==5 && !renameable.empty()){command.kind="rename";command.instruction=choose(renameable,i);command.value=i;}
        else{++skipped;++skipReasons[std::to_string(kind)];continue;}
        Json description={{"ordinal",std::to_string(i)},{"kind",command.kind},{"instruction",std::to_string(command.instruction.value)},{"path",command.path},{"value",std::to_string(command.value)},{"target",std::to_string(command.target.value)}};
        const auto examined=h.sourcesExamined,copied=h.orderCopied,batchStart=h.history.size(),revision=m.revision;
        start=Clock::now();h.apply(command);
        // Bounded two-source batches on one isolated owner. A failed batch ends
        // the run and discards the host; no partially successful result is emitted.
        if(i%11==0 && command.kind=="bit-write" && writes.size()>1){
            auto id=writes[rng.next()%writes.size()];if(id==command.instruction)id=writes[(std::ranges::find(writes,id)-writes.begin()+1)%writes.size()];
            Command extra;extra.kind="bit-write";extra.instruction=id;h.apply(extra);description["second_bit_write"]=std::to_string(id.value);
        }
        m.revision=revision+1;++batches;const auto editNs=ns(start);
        if(command.kind=="insert")inserted.push_back(h.history.back().after->id);
        uint64_t undoNs=0,redoNs=0;if(i%7==0){start=Clock::now();for(size_t j=h.history.size();j>batchStart;--j)h.publish(h.history[j-1],false);m.revision=revision+2;undoNs=ns(start);start=Clock::now();for(size_t j=batchStart;j<h.history.size();++j)h.publish(h.history[j],true);m.revision=revision+3;redoNs=ns(start);}
        start=Clock::now();queryDigest^=h.query(Bank::Bit,i%2==0?hotBit:(command.kind=="bit-read"?command.value:rng.next()%32768));const auto queryNs=ns(start);
        trace.push_back(description);samples.push_back({{"ordinal",std::to_string(i)},{"edit_ns",std::to_string(editNs)},{"undo_ns",std::to_string(undoNs)},{"redo_ns",std::to_string(redoNs)},{"query_ns",std::to_string(queryNs)},{"sources_examined",std::to_string(h.sourcesExamined-examined)},{"order_ids_copied",std::to_string(h.orderCopied-copied)}});
    }
    report["after_edits_memory"]=processMemory();report["history_entries"]=batches;report["history_changes"]=h.history.size();report["trace_sha256"]=hash(trace.dump());report["trace"]=std::move(trace);report["samples"]=std::move(samples);report["skipped_edits"]=skipped;report["skip_reasons"]=skipReasons;report["query_digest"]=std::to_string(queryDigest);
    std::vector<InstructionId>().swap(writes);std::vector<InstructionId>().swap(renameable);std::vector<InstructionId>().swap(inserted);
    std::vector<Site>().swap(reads);std::vector<Site>().swap(targets);std::vector<Site>().swap(hotReads);std::vector<Site>().swap(hotTargets);entries.clear();readUses.clear();targetUses.clear();
    report["after_edit_catalog_release_memory"]=processMemory();
    start=Clock::now();std::vector<Change>().swap(h.history);report["history_release_ns"]=std::to_string(ns(start));report["history_released_memory"]=processMemory();
    report["retention_policy"]="history/order handles released; immutable payload pool retains dead nodes until host destruction; save compacts reachable payloads";
    stop();start=Clock::now();auto binary=save(h);report["checkpoint_capture_encode_ns"]=std::to_string(ns(start));report["semantic_bytes"]=std::to_string(checkpointSize(binary));const auto expected=checkpointHash(binary);report["content_sha256"]=expected;
    start=Clock::now();auto packed=packCheckpoint(std::move(binary));report["compress_ns"]=std::to_string(ns(start));report["container_bytes"]=std::to_string(packed.size());
    auto read=fileRoundTrip(packed,report);std::vector<std::byte>().swap(packed);stop();start=Clock::now();auto decoded=unpackCheckpoint(read);std::vector<std::byte>().swap(read);
    Context reloadContext;Model restored(reloadContext);restore(decoded,restored);decoded.clear();Host<Storage> reopened(restored,reloadContext);reopened.load();report["reload_to_ready_ns"]=std::to_string(ns(start));report["reloaded_memory"]=processMemory();
    expect(checkpointHash(save(reopened))==expected,"Corpus binary reload changed program content");
    report["storage"]={{"rows",h.store.metrics().live},{"capacity",h.store.metrics().capacity},{"growth_rows_moved",h.store.metrics().growthRowsMoved},{"erase_rows_moved",h.store.metrics().eraseRowsMoved}};
    for(size_t i=0;i<c.memory.size();++i){const auto& t=c.memory[i].totals;report["pmr"][DomainNames[i]]={{"current_bytes",t.current},{"peak_bytes",t.peak},{"allocations",t.allocations}};}
    report["edit_coverage_complete"]=skipped==0;report["disposition"]=skipped?"unsupported-required":"passed";report["complete"]=skipped==0;return report;
}
} // namespace
Json run(const Json& manifest,std::string_view candidate,uint64_t seed,uint64_t edits,Controls controls) {
    Json report={{"format","jahorta.salsa.ir-corpus-evaluation"},{"version",1},{"complete",false},{"disposition","operation-failed"},{"build",buildIdentity()},{"candidate",candidate},{"seed",std::to_string(seed)},{"requested_edits",std::to_string(edits)},{"files",Json::array()}};
    try {
        expect(report["build"]["configuration"]=="Release","Corpus evaluation requires Release");
        expect(manifest.at("format")=="jahorta.salsa.ir-corpus-manifest" && manifest.at("version")==1 && manifest.at("dataset")=="gc_us","Expected explicit GameCube US manifest");
        const auto root=std::filesystem::canonical(utf8Path(manifest.at("root").get<std::string>()));
        const auto& files=manifest.at("assets");expect(files.is_array() && !files.empty() && files.size()==manifest.at("expected_scripts").get<size_t>(),"Empty/incomplete corpus inventory");
        const auto convention=manifest.at("text_convention").get<std::string>();const sct::SctKnownTextConventionDescriptor* text=nullptr;for(const auto& t:sct::sctKnownTextConventions())if(t.stableName==convention)text=&t;expect(text!=nullptr,"Unsupported text convention");
        Context context;Model model(context);Counts totals;uint64_t readNs=0,hashNs=0,parseNs=0,importNs=0,normalizeNs=0,failures=0;std::set<std::string> keys;std::string inventory;
        const auto loadStart=Clock::now();report["baseline_memory"]=processMemory();
        for(const auto& asset:files){if(controls.stopRequested())throw std::runtime_error("Corpus operation cancelled");const auto key=asset.get<std::string>();Json file={{"asset",key},{"status","failed"}};
            try {
                const auto relative=utf8Path(key);expect(!relative.empty() && relative.is_relative() && !relative.has_root_name(),"Invalid corpus asset key");for(const auto& part:relative)expect(part!=".." && part!=".","Unsafe corpus asset key");expect(keys.insert(key).second,"Duplicate corpus asset");
                const auto path=std::filesystem::canonical(root/relative);auto within=path.lexically_relative(root);expect(!within.empty() && *within.begin()!="..","Corpus asset escapes root");
                auto start=Clock::now();std::ifstream in(path,std::ios::binary|std::ios::ate);expect(bool(in),"Cannot open corpus asset");auto size=in.tellg();expect(size>0 && size<=64*1024*1024,"Invalid corpus script size");std::vector<uint8_t> bytes(static_cast<size_t>(size));in.seekg(0);in.read(reinterpret_cast<char*>(bytes.data()),size);expect(bool(in),"Incomplete corpus read");readNs+=ns(start);start=Clock::now();const auto sourceHash=hash(std::as_bytes(std::span(bytes)));hashNs+=ns(start);inventory+=key+":"+sourceHash+"\n";
                start=Clock::now();auto parsed=sct::SctParser{}.parse(bytes,{},sct::SctParseOptions{true});parseNs+=ns(start);start=Clock::now();sct::SctDocumentImportOptions options;options.declaredSourcePlatform=sct::SctPlatform::GameCube;options.sourceTextEncoding=text->encoding;auto imported=sct::SctDocumentImporter::import(parsed,options);importNs+=ns(start);file["diagnostic_count"]=imported.diagnostics.size();file["diagnostics"]=Json::array();for(size_t di=0;di<std::min<size_t>(16,imported.diagnostics.size());++di){const auto& d=imported.diagnostics[di];file["diagnostics"].push_back({{"code",static_cast<int>(d.code)},{"severity",static_cast<int>(d.severity)},{"message",d.message}});}expect(imported.document.has_value(),"SPICE could not produce a document");
                start=Clock::now();auto counts=appendDocument(model,*imported.document,Script{{},key,"gc_us:"+sourceHash});normalizeNs+=ns(start);
                totals.instructions+=counts.instructions;totals.parameters+=counts.parameters;totals.expressions+=counts.expressions;totals.texts+=counts.texts;totals.sourceOpaqueBytes+=counts.sourceOpaqueBytes;totals.sourceOpaqueItems+=counts.sourceOpaqueItems;totals.executionGaps+=counts.executionGaps;totals.unresolved+=counts.unresolved;
                file.update({{"status","loaded"},{"sha256",sourceHash},{"bytes",static_cast<uint64_t>(size)},{"instructions",counts.instructions},{"execution_gaps",counts.executionGaps},{"unresolved",counts.unresolved}});
            }catch(const std::exception& e){++failures;file["error"]=e.what();}
            report["files"].push_back(std::move(file));std::cerr<<"Corpus scripts "<<report["files"].size()<<"/"<<files.size()<<"\n";
        }
        report.update({{"dataset","gc_us"},{"text_convention",convention},{"inventory_sha256",hash(inventory)},{"expected_scripts",files.size()},{"loaded_scripts",files.size()-failures},{"failed_scripts",failures},{"instructions",totals.instructions},{"parameters",totals.parameters},{"expressions",totals.expressions},{"texts",totals.texts},{"execution_gaps",totals.executionGaps},{"unresolved",totals.unresolved},{"source_opaque_bytes_excluded",totals.sourceOpaqueBytes},{"source_opaque_items_excluded",totals.sourceOpaqueItems},{"source_hash_ns",std::to_string(hashNs)},{"native_file_read_ns",std::to_string(readNs)},{"native_parse_ns",std::to_string(parseNs)},{"native_import_ns",std::to_string(importNs)},{"normalization_ns",std::to_string(normalizeNs)},{"after_import_memory",processMemory()}});
        report["host"]="generic-native-tables-v2; shared storage candidates and FactIndex; not the narrow IR6 ProgramHost semantic adapter";
        report["coverage"]="All parsed opcode/parameter/SCPT/text structures stored; opaque source bytes excluded. Direct typed variable reads, fixed bit writes and explicit instruction references indexed. No inferred module guards, physical fallthrough, complete side-effect model or runtime path claims.";
        if(failures || totals.executionGaps){report["disposition"]="unsupported-required";return report;}
        if(candidate=="dense")return experiment<DenseStorage>(model,context,report,seed,edits,controls,loadStart);
        if(candidate=="paged")return experiment<PagedStorage>(model,context,report,seed,edits,controls,loadStart);
        if(candidate=="section-local")return experiment<SectionStorage>(model,context,report,seed,edits,controls,loadStart);
        throw std::runtime_error("Unsupported corpus storage candidate");
    }catch(const std::bad_alloc&){report["disposition"]="resource-limited";report["error"]="Corpus worker allocation failed";return report;}catch(const std::exception& e){report["disposition"]=controls.stopRequested()?"cancelled":"operation-failed";report["error"]=e.what();return report;}
}
void smoke() {
    sct::SctDocument document;
    const auto stringId=document.allocateStringId();const auto footerId=document.allocateSupplementaryTextId();
    const auto a=document.allocateInstructionId(),b=document.allocateInstructionId(),c=document.allocateInstructionId();
    sct::SctDocumentInstruction first;first.id=a;first.opcode=17;first.fixedParameters={{0,sct::SctExpressionFactory::encodedDecimalLiteral(17)}};
    auto bit=sct::SctExpressionFactory::bitVariable(17);expect(bit.expression.has_value(),"Cannot construct corpus smoke expression");
    sct::SctDocumentInstruction branch;branch.id=b;branch.opcode=0;branch.fixedParameters={{0,*bit.expression},{1,sct::SctInstructionReference{c}}};
    sct::SctDocumentInstruction broad;broad.id=c;broad.opcode=155;broad.skipRefresh=true;broad.scheduledExpression=sct::SctExpressionFactory::encodedDecimalLiteral(2);
    broad.fixedParameters={{0,sct::SctStringReference{stringId}},{1,sct::SctSupplementaryTextReference{footerId}},{2,sct::SctEncodedWordValue{0xfedcba98}},{3,sct::SctTerminatedWordSequenceValue{{12,34}}}};
    auto highBit=sct::SctExpressionFactory::bitVariable(0x10000011);expect(highBit.expression.has_value(),"Cannot create high bit index");broad.fixedParameters.push_back({7,*highBit.expression});
    broad.repeatedParameterGroups={{{{4,*bit.expression},{5,sct::SctInstructionReference{a}}}},{{{4,sct::SctEncodedWordValue{42}}}}};
    auto second=first;second.id=document.allocateInstructionId();second.opcode=18;second.fixedParameters={{0,sct::SctExpressionFactory::floatLiteral(17.0f)}};
    document.sections.push_back({document.allocateSectionId(),"init",sct::SctScriptSectionContent{{first,second}}});
    document.sections.push_back({document.allocateSectionId(),"loop",sct::SctScriptSectionContent{{branch}}});
    document.sections.push_back({document.allocateSectionId(),"helper",sct::SctScriptSectionContent{{broad}}});
    sct::SctMessage message;message.headerUtf8="header";message.body.elements={sct::SctTextChunk{"Hello"},sct::SctInlineCommand{sct::SctMessageCommandCode::C,sct::SctByteListCommandArgument{{1,2}}},sct::SctInlineCommand{sct::SctMessageCommandCode::Wc,sct::SctDecimalCommandArgument{7}}};
    document.sections.push_back({document.allocateSectionId(),"strings",sct::SctStringSectionContent{{stringId,message,sct::SctTextKind::SctString}}});
    document.sections.push_back({document.allocateSectionId(),"group",sct::SctStringGroupMarkerSectionContent{}});
    document.supplementaryText.push_back({footerId,sct::SctTextKind::PlainString,sct::SctPlainText{std::string("a\0b",3)}});
    {Context context;Model model(context);appendDocument(model,document,Script{{},"wide-index.sct","synthetic-source"});Host<PagedStorage> host(model,context);host.load();expect(host.query(Bank::Bit,0x10000011)!=0,"Bit index lost its 29th bit");
        size_t writes=0;host.index.query({FactKind::Write,Bank::Bit,17},[&](const Fact&){++writes;});expect(writes==2,"Float/decimal fixed writes were not both indexed");
        auto checkpoint=save(host);const auto expectedHash=checkpointHash(checkpoint);auto packed=packCheckpoint(std::move(checkpoint),64);auto restored=unpackCheckpoint(packed);Context restoredContext;Model restoredModel(restoredContext);restore(restored,restoredModel);Host<PagedStorage> reopened(restoredModel,restoredContext);reopened.load();expect(checkpointHash(save(reopened))==expectedHash,"Chunked checkpoint changed content");}
    Json expected;
    const auto exercise=[&]<class Storage>(std::string_view candidate){Context context;Model model(context);auto counts=appendDocument(model,document,Script{{},"synthetic.sct","synthetic-source"});expect(counts.instructions==4 && counts.texts==2 && !counts.executionGaps,"Broad corpus normalization lost content");
        Json initial;auto report=experiment<Storage>(model,context,initial,19,30,{},Clock::now());expect(report["disposition"]=="passed" && report["skipped_edits"]==0,"Corpus editing smoke failed");
        if(expected.is_null())expected=report;else for(auto key:{"content_sha256","trace_sha256","query_digest"})expect(report[key]==expected[key],"Corpus candidate results differ");
    };
    exercise.operator()<DenseStorage>("dense");exercise.operator()<PagedStorage>("paged");exercise.operator()<SectionStorage>("section-local");
    Context context;Model gap(context);auto opaque=document;std::get<sct::SctScriptSectionContent>(opaque.sections[0].content).instructions[0].fixedParameters.push_back({9,sct::SctOpaqueParameterValue{{123}}});
    expect(appendDocument(gap,opaque,Script{{},"gap.sct","synthetic-source"}).executionGaps==1,"Opaque execution gap disappeared");
    // Exercise actual file parsing using two tiny generated native scripts, outside any repository.
    const auto folder=std::filesystem::temp_directory_path()/("salsa-corpus-smoke-"+std::to_string(Clock::now().time_since_epoch().count()));expect(std::filesystem::create_directory(folder),"Cannot create corpus smoke folder");
    struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code e;std::filesystem::remove_all(path,e);}}cleanup{folder};
    auto native=makeNativeFixture(32,1,false);auto output=exportNative(native.fixture);Json assets=Json::array();
    for(size_t i=0;i<output.files.size();++i){auto key="script"+std::to_string(i)+".sct";std::ofstream out(folder/key,std::ios::binary);const auto& bytes=output.files[i].bytes;out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());expect(bool(out),"Cannot write synthetic corpus input");assets.push_back(key);}
    const auto path=folder.u8string();Json manifest={{"format","jahorta.salsa.ir-corpus-manifest"},{"version",1},{"dataset","gc_us"},{"root",std::string(reinterpret_cast<const char*>(path.data()),path.size())},{"assets",assets},{"expected_scripts",assets.size()},{"text_convention","shift-jis-byte-7f"}};
    auto report=run(manifest,"paged",1,0,{});expect(report["disposition"]=="passed",report.dump().c_str());
    manifest["assets"].push_back("missing.sct");manifest["expected_scripts"]=manifest["assets"].size();report=run(manifest,"paged",1,0,{});expect(report["disposition"]=="unsupported-required" && !report["complete"].get<bool>(),"Partial corpus was accepted");
}
} // namespace salsa::evaluation::corpus
