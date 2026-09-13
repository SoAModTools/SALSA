#include "IntegrationRun.h"
#include "PackedStorage.h"
#include <SpiceSCT/SctDocumentExporter.h>
#include <SpiceSCT/SctDocumentImporter.h>
#include <SpiceSCT/SctInstructionFactory.h>
#include <SpiceSCT/SctParser.h>

namespace salsa::evaluation {
namespace {
template<class Action> void rejected(Action action,const char* message) { bool caught=false; try { action(); } catch(const std::exception&) { caught=true; } expect(caught,message); }
}
void integrationSmoke() {
    // Two small synthetic native files per platform; the normal runner also covers fresh output and persistence.
    for(const auto* fixture:{"native-gc","native-dc"}) {
        RunSpec spec; spec.candidate="dense"; spec.fixture=fixture; spec.count=32; spec.persistence="compressed";
        auto report=execute(spec);
        expect(report.at("disposition")=="passed",report.at("diagnostics").dump().c_str());
        expect(report.at("integration").at("native_output_mapping_count")=="33","Native identity mapping is incomplete");
    }
    auto native=makeNativeFixture(32,1,false); Context context; ProgramHost<DenseStorage> host(context);
    expect(host.load(native.fixture).ok(),"Native smoke load failed");
    const auto init=std::ranges::find(native.fixture.sections,std::string("init"),&Section::name);
    expect(!host.prepare(EditBatch{{},{},{{init->id,"init","renamed"}}},host.capture()),"Required init was renameable");
    auto record=*host.lookup(init->order.front()); auto changed=record; changed.value=0;
    auto edit=host.prepare({record,changed},host.capture()); expect(edit && host.commit(std::move(edit).takeValue()),"Required body edit failed");
    const auto reserved=host.allocateInstructionId(); auto saved=host.checkpoint();
    const auto json=encodeProgram(saved); const auto packed=packProgram(json.dump());
    auto loaded=decodeProgram(Json::parse(unpackProgram(packed))); Context restoredContext; ProgramHost<SectionStorage> restored(restoredContext);
    expect(restored.load(loaded).ok() && encodeProgram(restored.checkpoint())==json,"Cross-backend checkpoint changed identities");
    expect(restored.allocateInstructionId().value>reserved.value,"Reload recycled reserved identity");
    size_t fallthroughs=0;
    host.enumerate([&](const Record& r){if(r.op==Op::Jump || r.op==Op::Return) host.sourceFacts(r.id,[&](const Fact& fact){fallthroughs+=fact.kind==FactKind::Fallthrough;});});
    expect(!fallthroughs,"Jump/return gained a physical fallthrough edge");
    auto draft=saved; std::ranges::find(draft.sections,init->id,&Section::id)->name="missing-init";
    auto decodedDraft=decodeProgram(encodeProgram(draft)); Context draftContext; ProgramHost<DenseStorage> draftHost(draftContext);
    expect(draftHost.load(decodedDraft).ok(),"Missing-root draft could not load");
    rejected([&](){exportNative(decodedDraft);},"Missing-root native export succeeded");
    auto broken=json; broken["version"]=99; rejected([&](){decodeProgram(broken);},"Unsupported program schema accepted");
    auto bad=loaded; bad.checkpoint->highWater=1; Context badContext; ProgramHost<DenseStorage> badHost(badContext);
    expect(!badHost.load(bad),"Invalid checkpoint allocator accepted");
    auto truncated=packed; truncated.pop_back(); rejected([&](){unpackProgram(truncated);},"Truncated container accepted");
    // A structurally valid native opcode outside the declared subset must not disappear into a successful benchmark.
    auto output=exportNative(saved);
    namespace sct=spice::sct;
    auto parsed=sct::SctParser{}.parse(output.files.front().bytes);
    auto document=sct::SctDocumentImporter::import(parsed); expect(document.document.has_value(),"Unsupported fixture import failed");
    auto& first=std::get<sct::SctScriptSectionContent>(document.document->sections.front().content).instructions.front();
    first.opcode=20; first.fixedParameters={{0,sct::SctExpressionFactory::encodedDecimalLiteral(1)}};
    document.document->opaqueAttachments.clear();
    auto unsupported=sct::SctDocumentExporter::exportDocument(*document.document,sct::SctDocumentExportOptions{sct::SctPlatform::GameCube,sct::kSctShiftJisByte7FEncoding});
    expect(unsupported.success,"Unsupported opcode fixture could not export"); output.files.front().bytes=std::move(unsupported.bytes);
    bool unsupportedRejected=false; try { importNative(output.files,saved.project); } catch(const UnsupportedNative&) { unsupportedRejected=true; }
    expect(unsupportedRejected,"Unsupported native opcode was silently omitted");
    RunSpec story; story.candidate="paged"; story.fixture="story-connectivity"; story.count=32; story.persistence="compressed";
    auto report=execute(story); expect(report.at("disposition")=="passed",report.at("diagnostics").dump().c_str());
}
} // namespace salsa::evaluation
