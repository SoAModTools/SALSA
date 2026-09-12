#pragma once
#include "SpiceMLD/MldDocument.h"
#include "SpiceSCT/SctDocument.h"
#include <string>
#include <optional>
#include <vector>
#include <array>
#include <map>

namespace smores::script {
enum class Status { Exact, Provisional, MissingInput, Ambiguous, Stale, Unsupported, Invalid };
enum class EvidenceKind { Observation, Inference, Assertion };
struct Evidence { EvidenceKind kind; std::string description; };
// No absolute paths. Document identity scopes SPICE IDs; revision identifies bytes.
struct ResourceIdentity {
    std::string dataset, asset, revision, document;
    auto operator<=>(const ResourceIdentity&) const = default;
};
enum class ResourceRole { Base, Auxiliary, Unclassified, StreamedTile };
struct Entry {
    ResourceIdentity resource;
    std::uint64_t tableIndex = 0;
    spice::mld::MldEntryId localId;
    std::uint32_t entryId = 0;
    std::int32_t tableId = 0;
    std::string function;
    std::vector<std::uint32_t> parameters;
    auto operator<=>(const Entry&) const = default;
};
struct Resource {
    ResourceIdentity identity;
    ResourceRole role = ResourceRole::Unclassified;
    std::vector<Entry> entries;
};
struct Catalog { std::vector<Resource> resources; bool complete = false; };
struct Resolution { Status status; std::string explanation; };
[[nodiscard]] std::string assetKey(std::string value);
[[nodiscard]] bool valid(const ResourceIdentity& identity);
[[nodiscard]] Resource describe(const ResourceIdentity&, ResourceRole, const spice::mld::MldDocument&);
[[nodiscard]] Resolution resolve(const Entry&, const Catalog&);
[[nodiscard]] std::optional<std::string> handlerName(std::int32_t tableId);
struct HandlerMatch { Entry entry; spice::sct::SctSectionId section; Evidence evidence; };
[[nodiscard]] std::vector<HandlerMatch> handlers(const Catalog&, const spice::sct::SctDocument&);
struct SignalProvider { Entry entry; std::uint32_t bit; Evidence evidence; };
[[nodiscard]] std::vector<SignalProvider> signalProviders(const Catalog&);
struct StateRead { spice::sct::SctScptValueKind kind; std::uint32_t index; };
[[nodiscard]] std::vector<StateRead> reads(const spice::sct::SctCanonicalExpression&);
enum class ResourceOperationKind { Load, Wait, Release };
struct ResourceOperation {
    spice::sct::SctSectionId section;
    spice::sct::SctInstructionId instruction;
    std::uint64_t sectionOrdinal, instructionOrdinal;
    ResourceOperationKind kind;
    std::optional<std::string> asset;
    bool scheduled, skipRefresh;
};
// Source order within each section, not an inferred global execution timeline.
[[nodiscard]] std::vector<ResourceOperation> resourceOperations(const spice::sct::SctDocument&);

struct TileFamily { std::string key; std::optional<std::uint32_t> byteVariable; };
[[nodiscard]] const std::array<TileFamily,42>& tileFamilies();
// Static observations qualify these portable defaults; availability and runtime
// reachability remain separate questions for the consuming project.
[[nodiscard]] const std::array<Evidence,3>& tileRuleEvidence();
struct TileVariant { std::string key; unsigned char suffix; };
[[nodiscard]] std::optional<TileVariant> tileVariant(std::string asset);
// GC US coordinate observation: filename digits are (5 - rowZ, columnX).
[[nodiscard]] std::optional<std::pair<unsigned,unsigned>> tileGrid(std::string_view key);
using KnownBytes = std::map<std::uint32_t,std::optional<std::uint8_t>>;
enum class SelectionState { Known, Unknown, Unrepresentable };
struct ResourceSelection {
    SelectionState state = SelectionState::Unknown;
    std::string filename;
    Status availability = Status::MissingInput;
    std::vector<ResourceIdentity> candidates;
};
[[nodiscard]] ResourceSelection selectTile(std::string_view key, const KnownBytes&, const Catalog& = {});
[[nodiscard]] ResourceSelection selectOverworld(bool script, const KnownBytes&, const std::vector<ResourceIdentity>& = {});
[[nodiscard]] bool isOverworldSelector(std::uint32_t index);
struct ByteAssignment {
    spice::sct::SctInstructionId instruction;
    std::uint32_t destination;
    spice::sct::SctCanonicalExpression expression;
};
[[nodiscard]] std::optional<ByteAssignment> byteAssignment(const spice::sct::SctDocumentInstruction&);
[[nodiscard]] std::optional<std::uint8_t> evaluateByte(const spice::sct::SctCanonicalExpression&, const KnownBytes&);
}
