#pragma once
#include "Evaluation.h"
#include <utility>

namespace salsa::evaluation {
// Storage-only map adapter. Values, targets, owners and authored order are host concerns.
class ReferenceStorage {
public:
    struct Prepared {
        const ReferenceStorage* owner;
        std::pmr::vector<StorageChange> changes;
        std::pmr::map<InstructionId,StoredRecord> inserts;
        Prepared(const ReferenceStorage* o, Context& c) : owner(o), changes(c.resource(Domain::Preparation)), inserts(c.resource(Domain::Records)) {}
        Prepared(Prepared&& other) noexcept : owner(std::exchange(other.owner,nullptr)),changes(std::move(other.changes)),inserts(std::move(other.inserts)) {}
        Prepared(const Prepared&)=delete;
    };
    explicit ReferenceStorage(Context& c):context_(c),records_(c.resource(Domain::Records)) {}
    Result<void> load(std::span<const StoredRecord> input, std::span<const Section>) {
        if (!records_.empty()) return error("Store already loaded");
        for (const auto& record:input) if (!record.id.value || !records_.emplace(record.id,record).second) return error("Duplicate/zero instruction ID");
        return Result<void>::success();
    }
    const StoredRecord* lookup(InstructionId id) const noexcept {
        const auto found=records_.find(id); return found==records_.end()?nullptr:&found->second;
    }
    template<class Visitor> void forEachPhysical(Visitor&& visitor) const { for(const auto& [id,r]:records_) visitor(r); }
    Result<Prepared> prepare(std::span<const StorageChange> changes) {
        Prepared plan(this,context_);
        plan.changes.assign(changes.begin(),changes.end());
        for (const auto& change:changes) {
            if (!change.before && !change.after) return Result<Prepared>::failure(diagnostic(core::DiagnosticCode::EvaluationOperationRejected,"Empty storage change"));
            const auto id=change.before?change.before->id:change.after->id;
            const auto* current=lookup(id);
            if ((change.before && (!current || *current!=*change.before)) || (!change.before && current) ||
                (change.after && change.after->id!=id)) return Result<Prepared>::failure(diagnostic(core::DiagnosticCode::EvaluationOperationRejected,"Storage precondition mismatch"));
            if (!change.before) plan.inserts.emplace(id,*change.after); // allocate map nodes before publication
        }
        return Result<Prepared>::success(std::move(plan));
    }
    void apply(Prepared&& plan) noexcept {
        assert(plan.owner==this);
        for(const auto& change:plan.changes) {
            if (!change.before) { records_.insert(plan.inserts.extract(change.after->id)); ++work_.inserted; }
            else if (!change.after) { records_.erase(change.before->id); ++work_.erased; }
            else { records_.find(change.before->id)->second=*change.after; ++work_.replaced; }
        }
        plan.owner=nullptr;
    }
    Result<void> validate() const {
        for(const auto& [id,r]:records_) if(id!=r.id) return error("Storage identity mismatch");
        return Result<void>::success();
    }
    StorageMetrics metrics() const noexcept { return {records_.size(),records_.size()*sizeof(StoredRecord),work_,records_.size()}; }
private:
    static Result<void> error(std::string message) { return Result<void>::failure(diagnostic(core::DiagnosticCode::EvaluationOperationRejected,std::move(message))); }
    Context& context_;
    std::pmr::map<InstructionId,StoredRecord> records_;
    Work work_;
};
static_assert(StorageCandidate<ReferenceStorage>);
} // namespace salsa::evaluation
