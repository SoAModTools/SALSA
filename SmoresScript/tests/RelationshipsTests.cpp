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
    std::cout << "SmoresScript contracts passed\n"; return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; } }
