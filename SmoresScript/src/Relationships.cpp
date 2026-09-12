#include "smores/script/Relationships.h"
#include <algorithm>
#include <iomanip>
#include <sstream>
namespace smores::script {
std::string assetKey(std::string value) {
    for (auto& c : value) { if (c == '\\') c = '/'; if (c >= 'A' && c <= 'Z') c += 'a' - 'A'; }
    return value;
}
bool valid(const ResourceIdentity& r) {
    if (r.dataset.empty() || r.revision.empty() || r.document.empty() || r.asset.empty() || r.asset != assetKey(r.asset)
        || r.asset.front() == '/' || r.asset.find(':') != std::string::npos) return false;
    for (const unsigned char c : r.asset) if (c < 0x20 || c == 0x7f) return false;
    std::istringstream in(r.asset); std::string part;
    while (std::getline(in, part, '/')) if (part.empty() || part == "." || part == "..") return false;
    return r.asset.back() != '/';
}
Resource describe(const ResourceIdentity& identity, ResourceRole role, const spice::mld::MldDocument& doc) {
    Resource result{identity, role};
    for (std::size_t i = 0; i < doc.entries.size(); ++i) {
        const auto& e = doc.entries[i]; result.entries.push_back({identity, i, e.id, e.entryId, e.tableId, e.functionName, e.functionParameters});
    }
    return result;
}
Resolution resolve(const Entry& entry, const Catalog& catalog) {
    if (!valid(entry.resource) || !entry.localId) return {Status::Invalid, "Invalid source qualification."};
    std::vector<const Resource*> found;
    for (const auto& r : catalog.resources) if (r.identity.dataset == entry.resource.dataset && r.identity.asset == entry.resource.asset) found.push_back(&r);
    if (found.empty()) {
        if (std::ranges::any_of(catalog.resources, [&](const auto& r) { return r.identity.asset == entry.resource.asset; }))
            return {Status::Stale, "Resource is from a different dataset snapshot; explicitly reselect its entry."};
        return {Status::MissingInput, "Resource not inspected in this catalog; absence does not establish deletion or residency."};
    }
    if (found.size() != 1) return {Status::Ambiguous, "Multiple resource snapshots require explicit selection."};
    const auto& r = *found.front();
    if (r.identity != entry.resource) return {Status::Stale, "Resource revision or imported-document identity changed; explicitly reselect the entry."};
    if (entry.tableIndex >= r.entries.size() || r.entries[static_cast<std::size_t>(entry.tableIndex)] != entry)
        return {Status::Stale, "Entry was reordered or changed; explicitly reselect the entry."};
    return {Status::Exact, "Selected entry matches the captured resource snapshot. Residency is unknown."};
}
std::optional<std::string> handlerName(std::int32_t id) {
    if (id < 0 || id > 99999) return {};
    std::ostringstream out; out << 'M' << std::setw(5) << std::setfill('0') << id; return out.str();
}
std::vector<HandlerMatch> handlers(const Catalog& catalog, const spice::sct::SctDocument& doc) {
    std::vector<HandlerMatch> result;
    for (const auto& r : catalog.resources) for (const auto& e : r.entries) if (const auto name = handlerName(e.tableId))
        for (const auto& s : doc.sections) if (s.nameBytes == *name && std::holds_alternative<spice::sct::SctScriptSectionContent>(s.content))
            result.push_back({e, s.id, {EvidenceKind::Inference, "Selected-resource tblId matches the script section name; invocation and residency are not proven."}});
    return result;
}
std::vector<SignalProvider> signalProviders(const Catalog& catalog) {
    std::vector<SignalProvider> result;
    for (const auto& r : catalog.resources) for (const auto& e : r.entries)
        if (assetKey(e.function) == "eventhook" && e.parameters.size() > 8 && e.parameters[8] <= 0xffffff)
            result.push_back({e, e.parameters[8], {EvidenceKind::Inference, "Eventhook parameter 8 is a candidate BitVar index; this is not an engine dispatch decoder."}});
    return result;
}
std::vector<StateRead> reads(const spice::sct::SctCanonicalExpression& expression) {
    namespace sct = spice::sct; std::vector<StateRead> result;
    if (const auto* p = std::get_if<sct::SctTypedScptProgram>(&expression.body)) for (const auto& op : p->operations)
        if (const auto* v = std::get_if<sct::SctScptValueOperation>(&op)) switch (v->kind) {
            case sct::SctScptValueKind::BitVariable: case sct::SctScptValueKind::ByteVariable:
            case sct::SctScptValueKind::IntegerVariable: case sct::SctScptValueKind::IntegerVariableLow16Comparison:
            case sct::SctScptValueKind::FloatVariable: case sct::SctScptValueKind::FloatBackedIntegerVariable:
                result.push_back({v->kind, v->encodingWord & 0xffffff}); break;
            default: break;
        }
    return result;
}
std::vector<ResourceOperation> resourceOperations(const spice::sct::SctDocument& doc) {
    namespace sct = spice::sct; std::vector<ResourceOperation> result;
    for (std::size_t si = 0; si < doc.sections.size(); ++si) if (const auto* body = std::get_if<sct::SctScriptSectionContent>(&doc.sections[si].content))
        for (std::size_t ii = 0; ii < body->instructions.size(); ++ii) {
            const auto& i = body->instructions[ii]; if (i.opcode != 23 && i.opcode != 110 && i.opcode != 113) continue;
            std::optional<std::string> asset;
            for (const auto& p : i.fixedParameters) if (p.schemaIndex == 0) if (const auto* ref = std::get_if<sct::SctSupplementaryTextReference>(&p.value))
                for (const auto& t : doc.supplementaryText) if (t.id == ref->target) if (const auto* plain = std::get_if<sct::SctPlainText>(&t.value)) asset = plain->utf8;
            result.push_back({doc.sections[si].id, i.id, si, ii, i.opcode == 23 ? ResourceOperationKind::Load : i.opcode == 110 ? ResourceOperationKind::Release : ResourceOperationKind::Wait,
                asset, i.scheduledExpression.has_value(), i.skipRefresh});
        }
    return result;
}
}
