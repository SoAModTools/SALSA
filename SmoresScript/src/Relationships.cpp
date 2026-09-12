#include "smores/script/Relationships.h"
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <bit>
#include <cmath>
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
const std::array<TileFamily,42>& tileFamilies() {
    static const auto result = [] {
        std::array<TileFamily,42> rows;
        const std::map<unsigned,unsigned> variables{{4,67},{5,68},{6,69},{11,57},{12,58},{13,70},{16,71},
            {20,59},{21,60},{22,45},{23,46},{24,72},{25,73},{26,74},{30,61},{31,75},{32,7},{33,47},{34,62},
            {42,8},{43,9},{44,10},{45,11},{52,12},{53,13},{54,14},{55,15}};
        for (unsigned z=0;z<6;++z) for (unsigned x=0;x<7;++x) {
            const auto found = variables.find(z*10+x);
            rows[z*7+x] = {std::to_string(z)+std::to_string(x),found == variables.end() ? std::nullopt : std::optional{found->second}};
        }
        return rows;
    }();
    return result;
}
const std::array<Evidence,3>& tileRuleEvidence() {
    static const std::array<Evidence,3> evidence{{
        {EvidenceKind::Observation,"The 42 filename keys and 27 ByteVar selector mappings agree across examined GameCube and Dreamcast JP/US/EU executable profiles."},
        {EvidenceKind::Observation,"GC US loader: filename digits are (5 - gridRowZ, gridColumnX); ByteVar[6] selects the overworld SCT/base MLD."},
        {EvidenceKind::Observation,"GC US initialization captures tile filenames; ordinary streaming reuses captured names. This does not establish runtime reachability for authored assignments."}}};
    return evidence;
}
namespace {
std::string filename(std::string path) { path = assetKey(std::move(path)); return path.substr(path.find_last_of('/')+1); }
ResourceSelection selection(std::string stem, std::optional<std::uint8_t> value, std::string extension, const std::vector<ResourceIdentity>& assets) {
    ResourceSelection r;
    if (!value) return r;
    const auto suffix = static_cast<unsigned char>('a'+*value);
    // Keep the engine's byte addition; unsupported path bytes are not clamped.
    if (suffix < 32 || suffix > 126 || std::string_view("/\\:*?\"<>|").find(static_cast<char>(suffix)) != std::string_view::npos) {
        r.state = SelectionState::Unrepresentable; return r;
    }
    r.state = SelectionState::Known; r.filename = stem + static_cast<char>(suffix) + extension;
    for (const auto& a : assets) if (filename(a.asset) == assetKey(r.filename)) r.candidates.push_back(a);
    std::ranges::sort(r.candidates);
    r.availability = r.candidates.empty() ? Status::MissingInput : r.candidates.size() == 1 ? Status::Exact : Status::Ambiguous;
    return r;
}
}
std::optional<TileVariant> tileVariant(std::string asset) {
    const auto name = filename(std::move(asset));
    if (name.size()!=11 || !name.starts_with("fiel") || name.substr(7)!=".mld" || name[4]<'0' || name[4]>'5' || name[5]<'0' || name[5]>'6') return {};
    return TileVariant{name.substr(4,2),static_cast<unsigned char>(name[6])};
}
std::optional<std::pair<unsigned,unsigned>> tileGrid(std::string_view key) {
    if (key.size()!=2 || key[0]<'0' || key[0]>'5' || key[1]<'0' || key[1]>'6') return {};
    return std::pair{static_cast<unsigned>('5'-key[0]),static_cast<unsigned>(key[1]-'0')};
}
ResourceSelection selectTile(std::string_view key, const KnownBytes& values, const Catalog& catalog) {
    const auto& families = tileFamilies(); const auto row = std::ranges::find(families,key,&TileFamily::key);
    if (row == families.end()) return {SelectionState::Unrepresentable};
    std::optional<std::uint8_t> value = 0;
    if (row->byteVariable) { const auto found = values.find(*row->byteVariable); value = found == values.end() ? std::nullopt : found->second; }
    std::vector<ResourceIdentity> assets; for (const auto& r : catalog.resources) assets.push_back(r.identity);
    return selection("fiel"+std::string(key),value,".mld",assets);
}
ResourceSelection selectOverworld(bool script, const KnownBytes& values, const std::vector<ResourceIdentity>& assets) {
    const auto found = values.find(6);
    return selection(script ? "me099" : "a099",found == values.end() ? std::nullopt : found->second,script ? ".sct" : ".mld",assets);
}
bool isOverworldSelector(std::uint32_t index) {
    return index == 6 || std::ranges::any_of(tileFamilies(),[&](const auto& t) { return t.byteVariable == index; });
}
std::optional<ByteAssignment> byteAssignment(const spice::sct::SctDocumentInstruction& i) {
    namespace sct = spice::sct;
    if (i.opcode != 5) return {};
    const auto dest = std::ranges::find(i.fixedParameters,0u,&sct::SctDocumentParameter::schemaIndex);
    const auto expr = std::ranges::find(i.fixedParameters,1u,&sct::SctDocumentParameter::schemaIndex);
    if (dest == i.fixedParameters.end() || expr == i.fixedParameters.end()) return {};
    const auto* word = std::get_if<sct::SctEncodedWordValue>(&dest->value);
    const auto* expression = std::get_if<sct::SctCanonicalExpression>(&expr->value);
    if (!word || !expression || (word->value & 0xffff0000) != 0x10000000 || (word->value & 0xffff) > 0x7fff) return {};
    return ByteAssignment{i.id,word->value & 0xffff,*expression};
}
std::optional<std::uint8_t> evaluateByte(const spice::sct::SctCanonicalExpression& expression, const KnownBytes& values) {
    namespace sct = spice::sct;
    const auto* p = std::get_if<sct::SctTypedScptProgram>(&expression.body);
    if (!p || p->operations.size()!=1) return {};
    const auto* v = std::get_if<sct::SctScptValueOperation>(&p->operations.front()); if (!v) return {};
    if (v->kind == sct::SctScptValueKind::ByteVariable) { const auto found = values.find(v->encodingWord & 0x0fffffff); return found == values.end() ? std::nullopt : found->second; }
    double number;
    if (v->kind == sct::SctScptValueKind::DecimalLiteral) {
        auto n = static_cast<std::int32_t>(v->encodingWord & 0xffffff); if (n & 0x800000) n |= static_cast<std::int32_t>(0xff000000);
        number = n / 256.0;
    } else if (v->kind == sct::SctScptValueKind::FloatLiteral && v->payloadWords.size()==1) number = std::bit_cast<float>(v->payloadWords.front());
    else return {};
    if (!std::isfinite(number) || number < INT32_MIN || number > INT32_MAX) return {};
    return static_cast<std::uint8_t>(static_cast<std::int32_t>(number));
}
}
