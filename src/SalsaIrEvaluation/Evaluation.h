#pragma once

#include <SalsaCore/Foundation/Result.h>
#include <nlohmann/json.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace salsa::evaluation {
using core::Result;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
template<class Tag> struct Id {
    uint64_t value{};
    auto operator<=>(const Id&) const = default;
};
using InstructionId = Id<struct InstructionTag>;
using SectionId = Id<struct SectionTag>;
using ScriptId = Id<struct ScriptTag>;
using OperandId = Id<struct OperandTag>;
using ExprId = Id<struct ExprTag>;
using ModuleId = Id<struct ModuleTag>;
using FactId = Id<struct FactTag>;
struct ProjectId { uint64_t high{}, low{}; auto operator<=>(const ProjectId&) const = default; };
enum class Bank : uint8_t { Bit, Byte, Int, Float };
enum class ExprOp : uint8_t { Literal, Variable, Add, Unknown, Fixed };
struct Expression {
    ExprId id; ExprOp op{}; Bank bank{}; uint32_t variable{};
    uint64_t literal{}; ExprId left{}, right{};
    auto operator<=>(const Expression&) const = default;
};
struct NativeSource {
    std::string dataset, asset, sha256, lineage;
    bool dreamcast{};
};
struct Script { ScriptId id; std::string name, source; std::optional<NativeSource> native; };
struct Section { SectionId id; ScriptId script; std::string name; std::vector<InstructionId> order; };

enum class ModuleRole : uint8_t { Cutscene, Interaction, Helper, FieldEntry };
struct Module { ModuleId id; ScriptId script; std::string name; std::vector<SectionId> sections; InstructionId entry; ModuleRole role{}; };

// A declared synthetic instruction language, not SPICE opcodes or the production IR.
enum class Op : uint8_t { Read, Write, Call, Transition, Guard, Unknown, Jump, Return, Frame, Sleep };
inline bool isControl(Op op) { return op==Op::Call || op==Op::Transition || op==Op::Jump; }
struct Record {
    InstructionId id;
    SectionId section;
    Op op{};
    uint32_t variable{};
    uint64_t value{}; // literal for Write; instruction ID for Call
    Bank bank{};
    ExprId expression{}, schedule{};
    uint32_t evidence{}; // 1-based unresolved/unknown evidence; zero means resolved
    uint32_t flags{};
    auto operator<=>(const Record&) const = default;
};
// A zero ID denotes absence for insertion/deletion. Records are command/query values.
struct Mutation { Record before, after; };
struct Placement { InstructionId instruction; SectionId section; InstructionId before; };
struct Rename { SectionId section; std::string before, after; };
struct EditBatch { std::vector<Mutation> mutations; std::vector<Placement> placements; std::vector<Rename> names; };
enum class ValueKind : uint8_t { Literal, Address, Target, Expression, Unresolved, Empty };
struct Operand {
    OperandId id; ValueKind kind{}; Bank bank{}; uint64_t value{};
    auto operator<=>(const Operand&) const = default;
};
struct StoredRecord {
    InstructionId id; SectionId section; Op op{}; uint32_t firstOperand{}, flags{};
    auto operator<=>(const StoredRecord&) const = default;
};
struct StorageChange { std::optional<StoredRecord> before, after; };
enum class QueryKind { Reads, Writes, Incoming, Fallthrough, Unknown, Guards, Connections };
struct Query { QueryKind kind; uint64_t key; Bank bank=Bank::Bit; };
struct Edit { Mutation change; };
struct Undo {};
struct Redo {};
using Action = std::variant<Query, Edit, Undo, Redo, EditBatch>;
struct Step { uint64_t id; Action action; };
// Persist identity roles, not derived values/postings or private pool locations.
struct FactIdentity { FactId id; OperandId operand; uint8_t kind{}; uint64_t role{}; };
struct SectionLayout { SectionId id; size_t count{}; };
struct RecordIdentity { InstructionId instruction; std::array<OperandId,3> operands; std::vector<FactIdentity> facts; };
struct Checkpoint { uint64_t revision{}, highWater{}; std::vector<RecordIdentity> identities; };
struct Fixture {
    ProjectId project{1,1};
    std::vector<Script> scripts;
    std::vector<Section> sections;
    std::vector<Module> modules;
    std::vector<Expression> expressions;
    std::vector<std::string> evidence;
    std::vector<InstructionId> reserved;
    std::vector<Record> records;
    std::vector<Step> trace;
    std::string checksum, traceChecksum;
    std::optional<Checkpoint> checkpoint;
};
struct Capture {
    uint64_t instance{}, revision{}, generation{};
    ProjectId project;
    auto operator<=>(const Capture&) const = default;
};

enum class Domain : size_t { Records, Preparation, History, Results, Values, Order, Index, Directory, Connectivity, Count };
inline constexpr std::array DomainNames{"records","preparation","history","query_results","values","order","indexes","directory","connectivity"};
struct AllocationTotals {
    uint64_t allocations{}, deallocations{}, requested{}, current{}, peak{};
};
class CountingResource final : public std::pmr::memory_resource {
public:
    AllocationTotals totals;
    std::optional<uint64_t> failAfter; // successful allocations still permitted
private:
    void* do_allocate(size_t bytes, size_t alignment) override;
    void do_deallocate(void* p, size_t bytes, size_t alignment) override;
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }
};
struct Context {
    std::array<CountingResource, static_cast<size_t>(Domain::Count)> memory;
    CountingResource* resource(Domain d) { return &memory[static_cast<size_t>(d)]; }
};
struct Work { uint64_t examined{}, replaced{}, directoryWrites{}, inserted{}, erased{}; };
struct StorageMetrics {
    uint64_t live{}, recordBytes{}; Work work;
    uint64_t capacity{}, groups{}, emptyGroups{}, directoryBytes{}, growths{}, growthRowsMoved{}, eraseRowsMoved{};
};

// Candidates implement this compile-time boundary, not the semantic workload.
// The host supplies unique-ID batches with checked owners, and rejects stale preparations.
// Preparation may retain spare capacity but never publishes record changes. Lookup pointers
// are borrowed until the next prepare/apply/load; stable IDs survive all slot movement.
struct RecordVisitor { void operator()(const StoredRecord&) const noexcept {} };
template<class T> concept StorageCandidate = requires(T store, const T view,
    std::span<const StoredRecord> records, std::span<const SectionLayout> sections, InstructionId id, std::span<const StorageChange> mutations, typename T::Prepared&& prepared) {
    { store.load(records,sections) } -> std::same_as<Result<void>>;
    { view.lookup(id) } -> std::same_as<const StoredRecord*>;
    view.forEachPhysical(RecordVisitor{});
    { store.prepare(mutations) } -> std::same_as<Result<typename T::Prepared>>;
    { store.apply(std::move(prepared)) } noexcept;
    { view.metrics() } -> std::same_as<StorageMetrics>;
    { view.validate() } -> std::same_as<Result<void>>;
};
struct CandidateDescriptor { std::string_view name; uint32_t interfaceVersion, implementationVersion; };
inline constexpr CandidateDescriptor ReferenceDescriptor{"reference", 3, 3};

struct RunSpec {
    std::string candidate = "reference";
    std::string orderPolicy="shared",readerPolicy="selected";
    std::string persistence="none";
    std::string fixture = "small-story";
    std::string trace = "semantic-edits";
    uint64_t seed = 1, count = 64;
    uint64_t timeoutMs = 30000, memoryMb = 512;
    uint32_t repetition = 0;
};
struct Controls {
    const std::atomic_bool* cancelled = nullptr;
    bool (*externalCancellation)(void*) = nullptr;
    void* cancellationContext = nullptr;
    bool stopRequested() const {
        return (cancelled && cancelled->load()) || (externalCancellation && externalCancellation(cancellationContext));
    }
};
struct Sample { uint64_t operation{}, ns{}, emitted{}, digest{}; std::string_view kind; };

core::Diagnostic diagnostic(core::DiagnosticCode code, std::string message);
Result<void> validateSpec(const RunSpec& spec);
Fixture makeFixture(uint64_t count, uint64_t seed, std::string_view shape="small-story");
void fingerprintFixture(Fixture& fixture);
uint64_t hashRecord(uint64_t hash, const Record& record);
std::string canonicalDigest(std::span<const Record> records);
Json encodeSpec(const RunSpec& spec);
Result<RunSpec> decodeSpec(const Json& value);
Result<Json> readJson(const std::filesystem::path& path);
Result<void> writeNewReport(const std::filesystem::path& path, const Json& report);
Json buildIdentity();
Json execute(const RunSpec& spec, Controls controls = {});
Result<Json> compareReports(const std::vector<Json>& reports);
Result<void> validateReport(const Json& report);
int exitCode(std::string_view disposition);
int selfTests();
} // namespace salsa::evaluation
