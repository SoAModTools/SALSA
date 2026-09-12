#include "smores/script/Relationships.h"
#include <iostream>
#include <stdexcept>
using namespace smores::script;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int main() { try {
    ResourceIdentity id{"dataset", "field/a001a.mld", "revision", "document"};
    check(valid(id), "valid identity"); auto bad = id; bad.asset = "../outside"; check(!valid(bad), "relative traversal rejected");
    spice::mld::MldDocument doc; spice::mld::MldEntry e; e.id = {7}; e.entryId = 999; e.tableId = 4000; e.functionName = "eventhook"; e.functionParameters.resize(9); e.functionParameters[8] = 42; doc.entries.push_back(e);
    auto resource = describe(id, ResourceRole::Base, doc); Catalog catalog{{resource}}; const auto selected = resource.entries.front();
    check(selected.tableIndex == 0 && selected.entryId == 999 && selected.localId.value == 7 && selected.tableId == 4000, "distinct entry coordinates");
    check(resolve(selected, catalog).status == Status::Exact, "exact");
    check(resolve(selected, {}).status == Status::MissingInput, "partial is not orphaned");
    catalog.resources.push_back(resource); check(resolve(selected, catalog).status == Status::Ambiguous, "ambiguous"); catalog.resources.pop_back();
    catalog.resources[0].identity.revision = "new"; check(resolve(selected, catalog).status == Status::Stale, "revision stale"); catalog.resources[0] = resource;
    catalog.resources[0].entries[0].entryId++; check(resolve(selected, catalog).status == Status::Stale, "reorder/change stale"); catalog.resources[0] = resource;
    auto providers = signalProviders(catalog); check(providers.size() == 1 && providers[0].bit == 42 && providers[0].evidence.kind == EvidenceKind::Inference, "candidate evidence");
    spice::sct::SctDocument script; script.sections.push_back({spice::sct::SctSectionId{1}, "M04000", spice::sct::SctScriptSectionContent{}});
    check(handlers(catalog, script).size() == 1, "handler match");
    check(handlerName(4000) == "M04000" && !handlerName(-1), "dispatch formatting");
    check(assetKey("FIELD\\A.MLD") == "field/a.mld", "case identity");
    auto& code = std::get<spice::sct::SctScriptSectionContent>(script.sections[0].content);
    code.instructions = {{spice::sct::SctInstructionId{1},23}, {spice::sct::SctInstructionId{2},113}, {spice::sct::SctInstructionId{3},110}};
    const auto ops = resourceOperations(script); check(ops.size() == 3 && ops[0].kind == ResourceOperationKind::Load && ops[1].kind == ResourceOperationKind::Wait && ops[2].kind == ResourceOperationKind::Release && !ops[0].asset, "ordered unresolved operations");
    script.supplementaryText.push_back({spice::sct::SctSupplementaryTextId{1}, spice::sct::SctTextKind::PlainString, spice::sct::SctPlainText{"aux.mld"}});
    code.instructions.front().fixedParameters.push_back({0,spice::sct::SctSupplementaryTextReference{spice::sct::SctSupplementaryTextId{1}}});
    check(resourceOperations(script).front().asset == "aux.mld", "resolved resource target");
    check(tileFamilies().size()==42,"all tile cells");
    unsigned mapped=0; for (const auto& f:tileFamilies()) mapped+=f.byteVariable.has_value(); check(mapped==27,"27 selector rows");
    const std::map<std::string,unsigned> expected{{"04",67},{"05",68},{"06",69},{"11",57},{"12",58},{"13",70},{"16",71},
        {"20",59},{"21",60},{"22",45},{"23",46},{"24",72},{"25",73},{"26",74},{"30",61},{"31",75},{"32",7},{"33",47},{"34",62},
        {"42",8},{"43",9},{"44",10},{"45",11},{"52",12},{"53",13},{"54",14},{"55",15}};
    for (const auto& f:tileFamilies()) {
        const auto found=expected.find(f.key);
        check(f.byteVariable==(found==expected.end() ? std::nullopt : std::optional{found->second}),"research selector mapping");
        if(found==expected.end()) check(selectTile(f.key,{}).filename=="fiel"+f.key+"a.mld","all fixed variants");
    }
    check(tileRuleEvidence()[1].description.find("GC US")!=std::string::npos,"platform-qualified rule evidence");
    check(tileGrid("32")==std::optional{std::pair{2u,2u}},"Z reversal and X column");
    check(!tileGrid("67"),"invalid tile key");
    check(tileVariant("FIELD/FIEL32G.MLD")->key=="32","normalized family");
    check(selectTile("00",{}).filename=="fiel00a.mld","fixed a without state");
    check(selectTile("32",{}).state==SelectionState::Unknown,"unknown selector");
    KnownBytes values{{7,6},{45,4},{6,16}};
    check(selectTile("32",values).filename=="fiel32g.mld","tile selector");
    check(selectTile("22",values).filename=="fiel22e.mld","independent selector");
    check(selectOverworld(true,values).filename=="me099q.sct","overworld script");
    check(selectOverworld(false,values).filename=="a099q.mld","base MLD selector");
    values[7]=25; Resource custom{{"custom","mods/fiel32z.mld","r","d"},ResourceRole::StreamedTile,{}};
    check(selectTile("32",values,{{custom}}).availability==Status::Exact,"modded variant allowed");
    auto other=custom; other.identity.asset="other/fiel32z.mld";
    check(selectTile("32",values,{{custom,other}}).availability==Status::Ambiguous,"ambiguous asset match");
    values[7]=31; check(selectTile("32",values).state==SelectionState::Unrepresentable,"no clamped suffix");
    values[7]=0; check(selectTile("32",values,{{custom}}).filename=="fiel32a.mld","no fallback to existing z");
    for (const auto& f:tileFamilies()) if(f.byteVariable) { KnownBytes v{{*f.byteVariable,1}}; check(selectTile(f.key,v).filename=="fiel"+f.key+"b.mld","every selector maps to its own family"); }
    std::cout << "SmoresScript contracts passed\n"; return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; } }
