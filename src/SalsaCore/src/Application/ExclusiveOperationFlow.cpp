#include "SalsaCore/Application/ExclusiveOperationFlow.h"

#include <algorithm>
#include <mutex>
#include <queue>
#include <ranges>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace salsa::core {

ExclusiveOperationLogicalSize resolveExclusiveOperationSize(
    const ExclusiveOperationPageLayout layout,
    const ExclusiveOperationLogicalSize pageMinimum,
    const ExclusiveOperationLogicalSize availableScreen,
    const std::optional<ExclusiveOperationLogicalSize> currentSize) noexcept {
    ExclusiveOperationLogicalSize target;
    switch (layout) {
    case ExclusiveOperationPageLayout::Compact: target = {560, 260}; break;
    case ExclusiveOperationPageLayout::Standard: target = {820, 600}; break;
    case ExclusiveOperationPageLayout::Expanded: target = {1080, 760}; break;
    }
    target.width = std::max(target.width, pageMinimum.width);
    target.height = std::max(target.height, pageMinimum.height);
    const auto maximumWidth = std::max(1, availableScreen.width * 9 / 10);
    const auto maximumHeight = std::max(1, availableScreen.height * 9 / 10);
    target.width = std::clamp(target.width, 1, maximumWidth);
    target.height = std::clamp(target.height, 1, maximumHeight);
    if (currentSize) {
        target.width = std::max(target.width, currentSize->width);
        target.height = std::max(target.height, currentSize->height);
    }
    return target;
}

ExclusiveOperationFlow::ExclusiveOperationFlow(ExclusiveOperationFlowDefinition definition)
    : definition_(std::move(definition)), currentPage_(definition_.entry) {}

const ExclusiveOperationFlowDefinition& ExclusiveOperationFlow::definition() const noexcept {
    return definition_;
}

const std::string& ExclusiveOperationFlow::currentPage() const noexcept {
    return currentPage_;
}

const ExclusiveOperationPageNode* ExclusiveOperationFlow::currentNode() const noexcept {
    const auto found = std::ranges::find(definition_.pages, currentPage_,
        &ExclusiveOperationPageNode::id);
    return found == definition_.pages.end() ? nullptr : &*found;
}

std::vector<std::string> ExclusiveOperationFlow::validationErrors() const {
    std::vector<std::string> errors;
    std::unordered_set<std::string> pageIds;
    for (const auto& page : definition_.pages) {
        if (page.id.empty() || !pageIds.insert(page.id).second)
            errors.push_back("Operation page identifiers must be non-empty and unique.");
    }
    if (!pageIds.contains(definition_.entry))
        errors.push_back("The operation entry page does not exist.");

    std::unordered_set<std::string> edgeIds;
    std::unordered_set<std::string> sourceEvents;
    for (const auto& edge : definition_.edges) {
        if (edge.id.empty() || !edgeIds.insert(edge.id).second)
            errors.push_back("Operation edge identifiers must be non-empty and unique.");
        if (!pageIds.contains(edge.source) || !pageIds.contains(edge.destination))
            errors.push_back("Every operation edge must connect existing pages.");
        if (edge.event.empty()
            || !sourceEvents.insert(edge.source + '\n' + edge.event).second)
            errors.push_back("Each operation page/event pair must identify one edge.");
    }

    std::unordered_set<std::string> reachable;
    std::queue<std::string> pending;
    if (pageIds.contains(definition_.entry)) {
        reachable.insert(definition_.entry);
        pending.push(definition_.entry);
    }
    while (!pending.empty()) {
        auto source = std::move(pending.front());
        pending.pop();
        for (const auto& edge : definition_.edges) {
            if (edge.source == source && reachable.insert(edge.destination).second)
                pending.push(edge.destination);
        }
    }
    for (const auto& page : definition_.pages)
        if (!reachable.contains(page.id))
            errors.push_back("Every operation page must be reachable from the entry page.");

    bool reachableSummary = false;
    std::unordered_set<std::string> canReachSummary;
    std::queue<std::string> reversePending;
    for (const auto& page : definition_.pages) {
        if (page.role != ExclusiveOperationPageRole::Summary) continue;
        reachableSummary = reachableSummary || reachable.contains(page.id);
        if (canReachSummary.insert(page.id).second) reversePending.push(page.id);
        if (std::ranges::any_of(definition_.edges,
                [&page](const auto& edge) { return edge.source == page.id; }))
            errors.push_back("Summary pages must be terminal.");
    }
    if (!reachableSummary)
        errors.push_back("An operation must have a reachable summary page.");
    while (!reversePending.empty()) {
        auto destination = std::move(reversePending.front());
        reversePending.pop();
        for (const auto& edge : definition_.edges)
            if (edge.destination == destination
                && canReachSummary.insert(edge.source).second)
                reversePending.push(edge.source);
    }
    for (const auto& page : definition_.pages)
        if (reachable.contains(page.id) && !canReachSummary.contains(page.id))
            errors.push_back("Every reachable operation page must be able to reach a summary page.");
    return errors;
}

std::vector<const ExclusiveOperationPageEdge*> ExclusiveOperationFlow::outgoing(
    const EdgeGuard& enabled) const {
    std::vector<const ExclusiveOperationPageEdge*> result;
    for (const auto& edge : definition_.edges)
        if (edge.source == currentPage_ && (!enabled || enabled(edge.id)))
            result.push_back(&edge);
    return result;
}

bool ExclusiveOperationFlow::dispatch(
    const std::string_view event, const EdgeGuard& enabled) {
    const auto found = std::ranges::find_if(definition_.edges, [&](const auto& edge) {
        return edge.source == currentPage_ && edge.event == event;
    });
    if (found == definition_.edges.end() || (enabled && !enabled(found->id))) return false;
    currentPage_ = found->destination;
    return true;
}

struct ExclusiveOperationGate::State final {
    mutable std::mutex mutex;
    bool active = false;
    std::uint64_t generation = 0;
};

ExclusiveOperationGate::Lease::Lease(
    std::shared_ptr<State> state, const std::uint64_t generation)
    : state_(std::move(state)), generation_(generation) {}

ExclusiveOperationGate::Lease::Lease(Lease&& other) noexcept
    : state_(std::move(other.state_)), generation_(std::exchange(other.generation_, 0)) {}

ExclusiveOperationGate::Lease& ExclusiveOperationGate::Lease::operator=(Lease&& other) noexcept {
    if (this == &other) return *this;
    release();
    state_ = std::move(other.state_);
    generation_ = std::exchange(other.generation_, 0);
    return *this;
}

ExclusiveOperationGate::Lease::~Lease() { release(); }

ExclusiveOperationGate::Lease::operator bool() const noexcept { return state_ != nullptr; }

std::uint64_t ExclusiveOperationGate::Lease::generation() const noexcept {
    return generation_;
}

void ExclusiveOperationGate::Lease::release() noexcept {
    if (!state_) return;
    const auto state = std::move(state_);
    std::scoped_lock lock(state->mutex);
    if (state->active && state->generation == generation_) state->active = false;
    generation_ = 0;
}

ExclusiveOperationGate::ExclusiveOperationGate() : state_(std::make_shared<State>()) {}

std::optional<ExclusiveOperationGate::Lease> ExclusiveOperationGate::tryAcquire() {
    std::scoped_lock lock(state_->mutex);
    if (state_->active) return std::nullopt;
    state_->active = true;
    ++state_->generation;
    return Lease(state_, state_->generation);
}

bool ExclusiveOperationGate::active() const noexcept {
    std::scoped_lock lock(state_->mutex);
    return state_->active;
}

std::uint64_t ExclusiveOperationGate::generation() const noexcept {
    std::scoped_lock lock(state_->mutex);
    return state_->generation;
}

}  // namespace salsa::core
