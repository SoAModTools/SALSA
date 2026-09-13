#include "Integration.h"
#include "StorageSupport.h"
#include <SpiceSCT/SctDocumentBuilder.h>
#include <SpiceSCT/SctDocumentExporter.h>
#include <SpiceSCT/SctDocumentImporter.h>
#include <SpiceSCT/SctInstructionFactory.h>
#include <SpiceSCT/SctParser.h>
#include <SpiceSCT/SctScptEncoding.h>
#include <map>
#include <unordered_map>

namespace salsa::evaluation {
namespace {
namespace sct=spice::sct;
void supported(bool value,const std::string& message) { if(!value) throw UnsupportedNative(message); }
uint64_t elapsed(Clock::time_point start) { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count(); }
std::string hex(std::span<const uint8_t> bytes) { std::string out; for(auto b:bytes) { out+="0123456789abcdef"[b>>4]; out+="0123456789abcdef"[b&15]; } return out; }
std::string messages(const std::vector<sct::SctDocumentDiagnostic>& diagnostics) { std::string out; for(const auto& d:diagnostics) out+=d.message+"; "; return out; }
const sct::SctDocumentParameterValue& parameter(const sct::SctDocumentInstruction& i,uint32_t n) {
    auto p=std::ranges::find(i.fixedParameters,n,&sct::SctDocumentParameter::schemaIndex);
    supported(p!=i.fixedParameters.end(),"Missing native parameter"); return p->value;
}
const sct::SctCanonicalExpression& expressionParameter(const sct::SctDocumentInstruction& i,uint32_t n) {
    auto* value=std::get_if<sct::SctCanonicalExpression>(&parameter(i,n)); supported(value!=nullptr,"Unsupported native expression parameter"); return *value;
}
struct Normalize {
    Fixture& fixture; uint64_t& next;
    ExprId add(Expression e) { e.id=ExprId{++next}; fixture.expressions.push_back(e); return e.id; }
    ExprId expression(const sct::SctCanonicalExpression& expression) {
        const auto* program=std::get_if<sct::SctTypedScptProgram>(&expression.body);
        supported(program && !program->operations.empty() && program->operations.size()<=128,"Opaque/large native expression is outside IR6 coverage");
        std::vector<ExprId> stack;
        for(const auto& op:program->operations) {
            if(const auto* v=std::get_if<sct::SctScptValueOperation>(&op)) {
                supported(sct::isSctScptOperationEncodingValid(op),"Invalid SCPT value encoding"); Expression e;
                switch(v->kind) {
                case sct::SctScptValueKind::DecimalLiteral:
                    supported((v->encodingWord&0x8000ff)==0,"IR6 covers only nonnegative integral decimal literals");
                    e.literal=(v->encodingWord>>8)&0x7fff; break;
                case sct::SctScptValueKind::InlineValue:
                    supported(sct::isSctScptInlineValue(v->encodingWord),"Unsupported inline value"); e.op=ExprOp::Fixed; e.literal=v->encodingWord; break;
                case sct::SctScptValueKind::ByteVariable:
                case sct::SctScptValueKind::BitVariable:
                    e.op=ExprOp::Variable; e.bank=v->kind==sct::SctScptValueKind::ByteVariable?Bank::Byte:Bank::Bit; e.variable=v->encodingWord&0x0fffffff; break;
                default: throw UnsupportedNative("Native expression value kind is outside IR6 coverage");
                }
                stack.push_back(add(e));
            } else if(const auto* binary=std::get_if<sct::SctScptBinaryOperation>(&op)) {
                supported(sct::isSctScptOperationEncodingValid(op) && binary->kind==sct::SctScptBinaryOperationKind::Arithmetic && sct::sctScptOperatorSymbol(binary->encodingWord)=="+" && stack.size()>=2,"Only conventional addition expressions are covered");
                auto right=stack.back(); stack.pop_back(); auto left=stack.back(); stack.pop_back();
                stack.push_back(add({{},ExprOp::Add,Bank::Bit,0,0,left,right}));
            } else throw UnsupportedNative("Native stack/inert expression operation is outside IR6 coverage");
        }
        supported(stack.size()==1,"Native expression must have one conventional result"); return stack.back();
    }
    uint64_t literal(const sct::SctCanonicalExpression& e) {
        const auto id=expression(e); const auto& node=fixture.expressions.back();
        supported(node.id==id && node.op==ExprOp::Literal,"Native bit destination must be a fixed integral index"); return node.literal;
    }
};
struct NativeView {
    std::unordered_map<uint64_t,const Record*> records;
    std::unordered_map<uint64_t,const Expression*> expressions;
    explicit NativeView(const Fixture& f) {
        records.reserve(f.records.size()); expressions.reserve(f.expressions.size());
        for(const auto& r:f.records) supported(records.emplace(r.id.value,&r).second,"Duplicate native output instruction ID");
        for(const auto& e:f.expressions) supported(expressions.emplace(e.id.value,&e).second,"Duplicate native output expression ID");
    }
    sct::SctCanonicalExpression expression(ExprId id,unsigned depth=0) const {
        supported(depth<24 && expressions.contains(id.value),"Missing/deep native output expression"); const auto& e=*expressions.at(id.value);
        if(e.op==ExprOp::Literal) { supported(e.literal<=32767,"Native decimal literal outside IR6 range"); return sct::SctExpressionFactory::encodedDecimalLiteral(static_cast<int16_t>(e.literal)); }
        if(e.op==ExprOp::Fixed) { supported(e.literal<=UINT32_MAX && sct::isSctScptInlineValue(static_cast<uint32_t>(e.literal)),"Invalid fixed native constant"); return sct::SctExpressionFactory::oneWordValue(static_cast<sct::SctExpressionOneWordValue>(e.literal)); }
        sct::SctExpressionBuildResult result;
        if(e.op==ExprOp::Variable) {
            supported(e.bank==Bank::Bit || e.bank==Bank::Byte,"Native expression bank outside IR6 coverage");
            result=e.bank==Bank::Bit?sct::SctExpressionFactory::bitVariable(e.variable):sct::SctExpressionFactory::byteVariable(e.variable);
        } else if(e.op==ExprOp::Add) result=sct::SctExpressionFactory::binaryOperator(sct::SctExpressionBinaryOperator::Add,expression(e.left,depth+1),expression(e.right,depth+1));
        else throw UnsupportedNative("Unknown native output expression");
        supported(result.expression.has_value(),messages(result.diagnostics)); return std::move(*result.expression);
    }
    sct::SctCanonicalExpression value(const Record& r) const {
        if(r.expression.value) return expression(r.expression);
        supported(r.value<=32767,"Native value outside IR6 literal range"); return sct::SctExpressionFactory::encodedDecimalLiteral(static_cast<int16_t>(r.value));
    }
};
}

NativeInput importNative(std::span<const NativeFile> files,ProjectId project) {
    NativeInput result; auto& f=result.fixture; f.project=project; uint64_t next=0,parseNs=0,importNs=0,normalizeNs=0,bytes=0,opaqueBytes=0,opaqueCount=0;
    for(const auto& file:files) {
        auto start=Clock::now(); auto parsed=sct::SctParser{}.parse(file.bytes,{},sct::SctParseOptions{true}); parseNs+=elapsed(start);
        start=Clock::now(); sct::SctDocumentImportOptions options; options.declaredSourcePlatform=file.dreamcast?sct::SctPlatform::Dreamcast:sct::SctPlatform::GameCube; options.sourceTextEncoding=sct::kSctShiftJisByte7FEncoding;
        auto imported=sct::SctDocumentImporter::import(parsed,options); importNs+=elapsed(start);
        expect(imported.document.has_value(),messages(imported.diagnostics).c_str()); start=Clock::now();
        const auto& d=*imported.document; const auto& receipt=imported.context.receipt(); bytes+=file.bytes.size();
        Script script{ScriptId{++next},file.asset,"sha256:"+hex(receipt.rawSourceSha256),NativeSource{"synthetic-ir6",file.asset,hex(receipt.rawSourceSha256),hex(receipt.lineage.sha256),file.dreamcast}};
        f.scripts.push_back(script);
        supported(d.supplementaryText.empty(),"Supplementary text is outside IR6 native coverage");
        for(const auto& a:d.opaqueAttachments) { opaqueBytes+=a.bytes.size(); ++opaqueCount; }
        std::unordered_map<uint64_t,InstructionId> instructions; std::vector<SectionId> sectionIds;
        for(const auto& s:d.sections) {
            const auto* code=std::get_if<sct::SctScriptSectionContent>(&s.content);
            supported(code!=nullptr,"Non-code native section is outside IR6 coverage");
            supported(code->instructions.empty() || (!code->instructions.back().scheduledExpression && (code->instructions.back().opcode==10 || code->instructions.back().opcode==12)),"Cross-section physical fallthrough is outside IR6 native coverage");
            Section section{SectionId{++next},script.id,s.nameBytes,{}}; sectionIds.push_back(section.id);
            for(const auto& i:code->instructions) { InstructionId id{++next}; instructions.emplace(i.id.value(),id); section.order.push_back(id); }
            f.sections.push_back(std::move(section));
        }
        Normalize normal{f,next};
        for(size_t si=0;si<d.sections.size();++si) for(const auto& i:std::get<sct::SctScriptSectionContent>(d.sections[si].content).instructions) {
            supported(i.repeatedParameterGroups.empty(),"Repeated native parameters are outside IR6 coverage");
            Record r{instructions.at(i.id.value()),sectionIds[si],Op::Unknown}; r.flags=i.skipRefresh?1:0;
            if(i.scheduledExpression) r.schedule=normal.expression(*i.scheduledExpression);
            size_t count=0;
            switch(i.opcode) {
            case 17: case 18:
                r.op=Op::Write; r.bank=Bank::Bit; r.variable=static_cast<uint32_t>(normal.literal(expressionParameter(i,0))); r.value=i.opcode==17; count=1; break;
            case 5: {
                r.op=Op::Write; r.bank=Bank::Byte; auto* dest=std::get_if<sct::SctEncodedWordValue>(&parameter(i,0));
                supported(dest && (dest->value&0xffff0000)==0x10000000 && (dest->value&0xffff)<=0x7fff,"Unsupported byte destination encoding"); r.variable=dest->value&0xffff;
                r.expression=normal.expression(expressionParameter(i,1)); count=2; break;
            }
            case 10: case 11: {
                r.op=i.opcode==10?Op::Jump:Op::Call; const auto* target=std::get_if<sct::SctInstructionReference>(&parameter(i,0));
                supported(target && instructions.contains(target->target.value()),"Unresolved native control target is outside IR6 export coverage"); r.value=instructions.at(target->target.value()).value; count=1; break;
            }
            case 12: r.op=Op::Return; break;
            case 15: r.op=Op::Frame; break;
            case 16: r.op=Op::Sleep; r.expression=normal.expression(expressionParameter(i,0)); count=1; break;
            default: throw UnsupportedNative("Unsupported native opcode "+std::to_string(i.opcode)+" in "+file.asset+"/"+d.sections[si].nameBytes);
            }
            supported(count==i.fixedParameters.size(),"Unexpected native parameter count");
            supported(!r.schedule.value || (r.op!=Op::Jump && r.op!=Op::Return),"Scheduled jump/return is outside IR6 coverage");
            f.records.push_back(r);
        }
        normalizeNs+=elapsed(start);
        result.measurements["after_native_normalization_memory"]=processMemory();
    }
    result.measurements.update({{"source_bytes",std::to_string(bytes)},{"parse_decompress_ns",std::to_string(parseNs)},{"spice_import_ns",std::to_string(importNs)},{"normalize_ns",std::to_string(normalizeNs)},
        {"source_only_opaque_bytes",std::to_string(opaqueBytes)},{"source_only_opaque_attachments",std::to_string(opaqueCount)},
        {"coverage","Opcodes 5,10,11,12,15,16,17,18; nonnegative integral decimal, fixed inline, bit/byte leaf and conventional addition expressions; no branch/switch, text, other banks or unknown execution content"}});
    return result;
}

NativeOutput exportNative(const Fixture& f) {
    NativeView view(f); NativeOutput result; result.project=f.project; result.revision=f.checkpoint?f.checkpoint->revision:0;
    for(const auto& script:f.scripts) {
        supported(script.native.has_value(),"Native output requires an imported source baseline");
        sct::SctDocumentBuilder builder; auto& d=builder.document();
        std::unordered_map<uint64_t,sct::SctInstructionId> ids; std::vector<const Section*> sections;
        unsigned init=0,loop=0;
        for(const auto& s:f.sections) if(s.script==script.id) {
            init+=s.name=="init"; loop+=s.name=="loop"; sections.push_back(&s);
            supported(!s.order.empty(),"Empty native section needs explicit repair before output");
            for(auto id:s.order) supported(ids.emplace(id.value,builder.allocateInstructionId()).second,"Duplicate native order ID");
            const auto* end=view.records.at(s.order.back().value);
            supported(!end->schedule.value && (end->op==Op::Return || end->op==Op::Jump),"Cross-section physical fallthrough is outside IR6 coverage; provide an explicit terminal");
        }
        supported(init==1 && loop==1,"Native export requires exactly one init and loop; draft needs explicit repair");
        const auto mappingStart=result.locations.size();
        for(uint32_t si=0;si<sections.size();++si) {
            const auto& section=*sections[si]; sct::SctScriptSectionContent content;
            for(uint32_t ii=0;ii<section.order.size();++ii) {
                const auto& r=*view.records.at(section.order[ii].value);
                supported(r.section==section.id && !r.evidence && r.flags<=1,"Unresolved/misowned native instruction or unsupported flags");
                supported(!r.schedule.value || (r.op!=Op::Jump && r.op!=Op::Return),"Scheduled jump/return is outside IR6 coverage");
                sct::SctDocumentInstruction i; i.id=ids.at(r.id.value); i.skipRefresh=(r.flags&1)!=0;
                if(r.schedule.value) i.scheduledExpression=view.expression(r.schedule);
                switch(r.op) {
                case Op::Write:
                    if(r.bank==Bank::Bit) {
                        supported(!r.expression.value && r.value<=1 && r.variable<=32767,"Unsupported native bit assignment");
                        i.opcode=r.value?17:18; i.fixedParameters={{0,sct::SctExpressionFactory::encodedDecimalLiteral(static_cast<int16_t>(r.variable))}};
                    } else {
                        supported(r.bank==Bank::Byte && r.variable<=0x7fff,"Native write bank/destination outside IR6 coverage");
                        i.opcode=5; i.fixedParameters={{0,sct::SctEncodedWordValue{0x10000000|r.variable}},{1,view.value(r)}};
                    }
                    break;
                case Op::Jump: case Op::Call:
                    supported(ids.contains(r.value),"Native control target must exist in the same script"); i.opcode=r.op==Op::Jump?10:11; i.fixedParameters={{0,sct::SctInstructionReference{ids.at(r.value)}}}; break;
                case Op::Return: i.opcode=12; break;
                case Op::Frame: i.opcode=15; break;
                case Op::Sleep: i.opcode=16; i.fixedParameters={{0,view.value(r)}}; break;
                default: throw UnsupportedNative("Authored instruction is outside native IR6 output coverage");
                }
                result.locations.push_back({script.id,section.id,r.id,i.id.value(),si,ii}); content.instructions.push_back(std::move(i));
            }
            d.sections.push_back({builder.allocateSectionId(),section.name,std::move(content)});
        }
        const auto dc=script.native->dreamcast;
        const sct::SctDocumentExportOptions options{dc?sct::SctPlatform::Dreamcast:sct::SctPlatform::GameCube,sct::kSctShiftJisByte7FEncoding,
            dc?sct::SctDocumentOutputByteOrder::LittleEndian:sct::SctDocumentOutputByteOrder::BigEndian,dc?sct::SctDocumentOutputWrapper::Aklz:sct::SctDocumentOutputWrapper::Raw};
        auto output=sct::SctDocumentExporter::exportDocument(d,options);
        expect(output.success && output.layout.has_value(),messages(output.diagnostics).c_str());
        std::unordered_map<uint64_t,sct::SctDocumentByteSpan> spans;
        for(const auto& i:output.layout->instructions) spans.emplace(i.id.value(),i.span);
        for(size_t n=mappingStart;n<result.locations.size();++n) { auto& m=result.locations[n]; auto span=spans.at(m.spiceInstruction); m.offset=span.offset; m.size=span.size; }
        result.files.push_back({script.native->asset,dc,std::move(output.bytes)});
    }
    return result;
}

NativeInput makeNativeFixture(uint64_t count,uint64_t seed,bool dreamcast) {
    supported(count>=32 && count<=1000000,"Native fixture count must be 32 through 1000000"); auto start=Clock::now();
    Fixture source; source.project={seed,6}; uint64_t next=0;
    for(unsigned scriptNumber=0;scriptNumber<2;++scriptNumber) {
        const auto n=scriptNumber?count-count/2:count/2; Script script{ScriptId{++next},"synthetic"+std::to_string(scriptNumber),"fixture-generator"};
        script.native=NativeSource{"synthetic-ir6",script.name+".sct",std::string(64,'0'),std::string(64,'0'),dreamcast}; source.scripts.push_back(script);
        Section init{SectionId{++next},script.id,"init",{}},loop{SectionId{++next},script.id,"loop",{}},scene{SectionId{++next},script.id,"scene",{}},helper{SectionId{++next},script.id,"helper",{}};
        auto expr=[&](Expression e){ e.id=ExprId{++next}; source.expressions.push_back(e); return e.id; };
        const auto byte=expr({{},ExprOp::Variable,Bank::Byte,6}); const auto one=expr({{},ExprOp::Literal,Bank::Bit,0,1});
        const auto sum=expr({{},ExprOp::Add,Bank::Bit,0,0,byte,one}); const auto fixed=expr({{},ExprOp::Fixed,Bank::Bit,0,0x7fffffffu});
        auto add=[&](Section& s,Op op)->Record& { InstructionId id{++next}; s.order.push_back(id); source.records.push_back({id,s.id,op}); return source.records.back(); };
        add(init,Op::Write).value=1; add(init,Op::Return);
        auto& assign=add(loop,Op::Write); assign.bank=Bank::Byte; assign.variable=87; assign.expression=byte;
        auto callIndex=source.records.size(); add(loop,Op::Call);
        auto& wait=add(loop,Op::Sleep); wait.expression=fixed;
        add(loop,Op::Jump).value=loop.order.front().value;
        for(uint64_t i=0;i<n-10;++i) { auto& r=add(scene,Op::Write); r.variable=static_cast<uint32_t>((seed+i)%512); r.value=i%2; r.flags=i%3==0; if(i==0) {r.bank=Bank::Byte;r.variable=7;r.value=0;r.expression=sum;} }
        auto helperCall=source.records.size(); add(scene,Op::Call).schedule=one; add(scene,Op::Return);
        add(helper,Op::Frame); add(helper,Op::Return);
        source.records[callIndex].value=scene.order.front().value; source.records[helperCall].value=helper.order.front().value;
        source.sections.insert(source.sections.end(),{init,loop,scene,helper});
    }
    auto native=exportNative(source); const auto generated=elapsed(start);
    auto result=importNative(native.files,source.project); result.measurements["synthetic_source_generation_ns"]=std::to_string(generated);
    auto& f=result.fixture; auto edit=std::ranges::find_if(f.records,[](const Record& r){return r.op==Op::Write && r.bank==Bank::Bit;});
    auto after=*edit; after.value=1-after.value;
    f.trace={{1,Query{QueryKind::Writes,after.variable}},{2,Edit{{*edit,after}}},{3,Undo{}},{4,Redo{}}};
    auto call=std::ranges::find_if(f.records,[](const Record& r){return r.op==Op::Call && !r.schedule.value;});
    auto retarget=*call;
    const auto owner=std::ranges::find(f.sections,call->section,&Section::id)->script;
    const auto helper=std::ranges::find_if(f.sections,[&](const Section& s){return s.script==owner && s.name=="helper";}); retarget.value=helper->order.front().value;
    f.trace.push_back({5,Edit{{*call,retarget}}}); f.trace.push_back({6,Query{QueryKind::Incoming,retarget.value}});
    const auto max=std::max_element(f.records.begin(),f.records.end(),[](const Record& a,const Record& b){return a.id<b.id;})->id.value;
    uint64_t high=max; for(const auto& e:f.expressions) high=std::max(high,e.id.value); for(const auto& s:f.sections) high=std::max(high,s.id.value);
    InstructionId inserted{high+1}; f.reserved.push_back(inserted);
    Record value{inserted,helper->id,Op::Write}; value.variable=121; value.value=1;
    f.trace.push_back({7,EditBatch{{{{},value}},{{inserted,helper->id,helper->order.back()}},{}}});
    f.trace.push_back({8,Undo{}}); f.trace.push_back({9,Redo{}});
    fingerprintFixture(f); return result;
}

std::string nativeMeaning(const Fixture& f) {
    NativeView view(f); std::unordered_map<uint64_t,std::array<uint64_t,3>> locations;
    for(size_t si=0;si<f.scripts.size();++si) { size_t section=0; for(const auto& s:f.sections) if(s.script==f.scripts[si].id) { for(size_t i=0;i<s.order.size();++i) locations.emplace(s.order[i].value,std::array<uint64_t,3>{si,section,i}); ++section; } }
    Json result=Json::array();
    for(const auto& script:f.scripts) {
        Json sections=Json::array();
        for(const auto& section:f.sections) if(section.script==script.id) {
            Json records=Json::array();
            for(auto id:section.order) { const auto& r=*view.records.at(id.value); Json value=r.value;
                if(isControl(r.op)) value=locations.at(r.value);
                else if(r.expression.value) value=sct::encodeSctCanonicalExpressionWords(view.expression(r.expression));
                else if(r.op==Op::Write && r.bank==Bank::Byte) value=sct::encodeSctCanonicalExpressionWords(view.value(r));
                records.push_back({static_cast<uint8_t>(r.op),static_cast<uint8_t>(r.bank),r.variable,value,r.flags,r.schedule.value?Json(sct::encodeSctCanonicalExpressionWords(view.expression(r.schedule))):Json(nullptr)});
            }
            sections.push_back({section.name,records});
        }
        result.push_back(sections);
    }
    return result.dump();
}
} // namespace salsa::evaluation
