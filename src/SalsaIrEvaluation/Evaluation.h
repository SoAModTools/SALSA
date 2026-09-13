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

// A declared synthetic instruction language, not SPICE opcodes or the production IR.
enum class Op : uint8_t { Read, Write, Call };
struct Record {
    InstructionId id;
    SectionId section;
    Op op{};
    uint32_t variable{};
    uint64_t value{}; // literal for Write; instruction ID for Call
    auto operator<=>(const Record&) const = default;
};
struct Mutation { Record before, after; };
enum class QueryKind { Reads, Writes, Incoming };
struct Query { QueryKind kind; uint64_t key; };
struct Edit { Mutation change; };
struct Undo {};
struct Redo {};
using Action = std::variant<Query, Edit, Undo, Redo>;
struct Step { uint64_t id; Action action; };
struct Fixture {
    std::vector<Record> records;
    std::vector<Step> trace;
    std::string checksum, traceChecksum;
};
struct Capture {
    uint64_t instance{}, revision{}, generation{};
    auto operator<=>(const Capture&) const = default;
};

enum class Domain : size_t { Records, Preparation, History, Results, Count };
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
struct Work { uint64_t examined{}, replaced{}, directoryWrites{}; };
struct StorageMetrics { uint64_t live{}, recordBytes{}; Work work; };

// Candidates implement this compile-time boundary, not the semantic workload.
struct RecordVisitor { void operator()(const Record&) const noexcept {} };
template<class T> concept StorageCandidate = requires(T store, const T view,
    std::span<const Record> records, InstructionId id, Mutation mutation, typename T::Prepared&& prepared) {
    { store.load(records) } -> std::same_as<Result<void>>;
    { view.lookup(id) } -> std::same_as<const Record*>;
    view.forEachPhysical(RecordVisitor{});
    { store.prepare(mutation) } -> std::same_as<Result<typename T::Prepared>>;
    { store.apply(std::move(prepared)) } noexcept;
    { view.metrics() } -> std::same_as<StorageMetrics>;
    { view.validate() } -> std::same_as<Result<void>>;
};
struct CandidateDescriptor { std::string_view name; uint32_t interfaceVersion, implementationVersion; };
inline constexpr CandidateDescriptor ReferenceDescriptor{"reference", 1, 1};

struct RunSpec {
    std::string candidate = "reference";
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
Fixture makeFixture(uint64_t count, uint64_t seed);
bool matches(const Record& record, Query query);
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
