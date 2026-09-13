#pragma once
#include "Evaluation.h"
#include <algorithm>
#include <utility>

namespace salsa::evaluation {
// A map-backed reference adapter. It is deliberately not a storage contender.
class ReferenceStorage {
public:
    struct Prepared {
        const ReferenceStorage* owner{};
        std::pmr::vector<Mutation> mutations;
        Prepared(const ReferenceStorage* o, std::pmr::memory_resource* resource) : owner(o), mutations(resource) {}
        Prepared(Prepared&& other) noexcept : owner(std::exchange(other.owner, nullptr)), mutations(std::move(other.mutations)) {}
        Prepared(const Prepared&) = delete;
    };
    explicit ReferenceStorage(Context& context) : context_(context), records_(context.resource(Domain::Records)) {}
    Result<void> load(std::span<const Record> input) {
        if (!records_.empty()) return Result<void>::failure(diagnostic(core::DiagnosticCode::EvaluationOperationRejected, "Store is already loaded"));
        for (const auto& r : input) {
            if (!r.id.value || !r.section.value || !records_.emplace(r.id, r).second)
                return Result<void>::failure(diagnostic(core::DiagnosticCode::EvaluationInvalidInput, "Duplicate/zero ID or owner"));
        }
        return validate();
    }
    const Record* lookup(InstructionId id) const noexcept {
        const auto found = records_.find(id);
        return found == records_.end() ? nullptr : &found->second;
    }
    template<class Visitor> void forEachPhysical(Visitor&& visit) const {
        for (const auto& [id, r] : records_) visit(r);
    }
    Result<Prepared> prepare(Mutation mutation) {
        const auto* current = lookup(mutation.before.id);
        if (!current || *current != mutation.before || mutation.after.id != mutation.before.id ||
            mutation.after.section != mutation.before.section || mutation.after.op != mutation.before.op ||
            (mutation.after.op == Op::Call && !lookup(InstructionId{mutation.after.value})))
            return Result<Prepared>::failure(diagnostic(core::DiagnosticCode::EvaluationOperationRejected, "Invalid replacement/precondition"));
        Prepared plan(this, context_.resource(Domain::Preparation));
        plan.mutations.push_back(mutation);
        return Result<Prepared>::success(std::move(plan));
    }
    // Host is the checked publication boundary; never call this with arbitrary plans.
    void apply(Prepared&& plan) noexcept {
        assert(plan.owner == this && plan.mutations.size() == 1);
        const auto& change = plan.mutations.front();
        auto found = records_.find(change.before.id);
        assert(found != records_.end() && found->second == change.before);
        found->second = change.after;
        ++work_.replaced;
        plan.owner = nullptr;
    }
    Result<void> validate() const {
        for (const auto& [id, r] : records_) {
            if (id != r.id || !r.section.value || (r.op == Op::Call && !lookup(InstructionId{r.value})))
                return Result<void>::failure(diagnostic(core::DiagnosticCode::EvaluationVerificationFailed, "Storage invariant failed"));
        }
        return Result<void>::success();
    }
    StorageMetrics metrics() const noexcept { return {records_.size(), records_.size() * sizeof(Record), work_}; }
private:
    Context& context_;
    std::pmr::map<InstructionId, Record> records_;
    Work work_;
};
static_assert(StorageCandidate<ReferenceStorage>);

// IR1 host: replacement/query/undo only. IR2 supplies production shared semantics.
template<StorageCandidate Storage>
class ReferenceHost {
public:
    struct Prepared {
        const ReferenceHost* owner;
        Capture capture;
        typename Storage::Prepared storage;
        Mutation change;
        Prepared(const ReferenceHost* o, Capture c, typename Storage::Prepared&& s, Mutation m)
            : owner(o), capture(c), storage(std::move(s)), change(m) {}
        Prepared(Prepared&& other) noexcept : owner(std::exchange(other.owner, nullptr)), capture(other.capture),
            storage(std::move(other.storage)), change(other.change) {}
        Prepared(const Prepared&) = delete;
    };
    explicit ReferenceHost(Context& context) : store_(context), history_(context.resource(Domain::History)) {
        capture_.instance = nextInstance_.fetch_add(1);
    }
    Result<void> load(std::span<const Record> records) { return store_.load(records); }
    Capture capture() const noexcept { return capture_; }
    const Record* lookup(InstructionId id) const noexcept { return store_.lookup(id); }
    template<class Sink> void query(Query query, Sink&& sink) const {
        store_.forEachPhysical([&](const Record& r) { ++examined_; if (matches(r, query)) sink(r); });
    }
    template<class Sink> void enumerate(Sink&& sink) const { store_.forEachPhysical(std::forward<Sink>(sink)); }
    Result<Prepared> prepare(Mutation change, Capture expected) {
        if (expected != capture_) return rejected<Prepared>("Stale capture");
        ++capture_.generation; // invalidates any earlier preparation, including after reserve failure
        auto storage = store_.prepare(change);
        if (!storage) return Result<Prepared>::failure(storage.diagnostics());
        history_.reserve(std::max(history_.capacity(), cursor_ + 1));
        return Result<Prepared>::success(Prepared(this, capture_, std::move(storage).takeValue(), change));
    }
    Result<void> commit(Prepared&& plan) {
        if (plan.owner != this || plan.capture != capture_) return rejected<void>("Stale, wrong-host, or consumed preparation");
        store_.apply(std::move(plan.storage));
        history_.resize(cursor_);
        history_.push_back(plan.change); // capacity reserved before publication
        ++cursor_; ++capture_.revision; ++capture_.generation;
        plan.owner = nullptr;
        return Result<void>::success();
    }
    void cancel(Prepared& plan) noexcept { if (plan.owner == this) { plan.owner = nullptr; ++capture_.generation; } }
    Result<void> undo() { return restore(false); }
    Result<void> redo() { return restore(true); }
    Result<void> validate() const { return store_.validate(); }
    size_t historySize() const noexcept { return history_.size(); }
    uint64_t examined() const noexcept { return examined_; }
    StorageMetrics metrics() const noexcept { return store_.metrics(); }
private:
    template<class T> static Result<T> rejected(std::string message) {
        return Result<T>::failure(diagnostic(core::DiagnosticCode::EvaluationOperationRejected, std::move(message)));
    }
    Result<void> restore(bool redo) {
        if ((!redo && !cursor_) || (redo && cursor_ == history_.size())) return rejected<void>("No history in requested direction");
        const auto& entry = history_[redo ? cursor_ : cursor_ - 1];
        Mutation change = redo ? entry : Mutation{entry.after, entry.before};
        ++capture_.generation;
        auto plan = store_.prepare(change);
        if (!plan) return Result<void>::failure(plan.diagnostics());
        store_.apply(std::move(plan).takeValue());
        if (redo) ++cursor_; else --cursor_;
        ++capture_.revision;
        return Result<void>::success();
    }
    Storage store_;
    std::pmr::vector<Mutation> history_;
    size_t cursor_{};
    Capture capture_;
    mutable uint64_t examined_{};
    inline static std::atomic_uint64_t nextInstance_{1};
};

} // namespace salsa::evaluation
