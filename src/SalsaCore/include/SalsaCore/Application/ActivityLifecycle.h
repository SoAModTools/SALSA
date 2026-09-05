#pragma once

#include <algorithm>
#include <concepts>
#include <ranges>
#include <utility>
#include <vector>

namespace salsa::core {

enum class IssueActivityTransition { Raised, Resolved, Reraised };

template<std::equality_comparable Issue>
class IssueActivityTracker final {
public:
    struct Event final {
        IssueActivityTransition transition = IssueActivityTransition::Raised;
        Issue issue;
    };

    [[nodiscard]] std::vector<Event> observe(const std::vector<Issue>& current) {
        std::vector<Event> events;
        std::vector<bool> previousMatched(current_.size(), false);
        std::vector<bool> currentMatched(current.size(), false);
        for (std::size_t next = 0; next < current.size(); ++next) {
            for (std::size_t previous = 0; previous < current_.size(); ++previous) {
                if (!previousMatched[previous] && current[next] == current_[previous]) {
                    previousMatched[previous] = true;
                    currentMatched[next] = true;
                    break;
                }
            }
        }
        for (std::size_t previous = 0; previous < current_.size(); ++previous)
            if (!previousMatched[previous]) events.push_back(
                {IssueActivityTransition::Resolved, current_[previous]});
        for (std::size_t next = 0; next < current.size(); ++next) {
            if (currentMatched[next]) continue;
            const bool seen = std::ranges::find(seen_, current[next]) != seen_.end();
            events.push_back({seen ? IssueActivityTransition::Reraised
                                   : IssueActivityTransition::Raised,
                current[next]});
            if (!seen) seen_.push_back(current[next]);
        }
        current_ = current;
        return events;
    }

private:
    std::vector<Issue> current_{};
    std::vector<Issue> seen_{};
};

}  // namespace salsa::core
