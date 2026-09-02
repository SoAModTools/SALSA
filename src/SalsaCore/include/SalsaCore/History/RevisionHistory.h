#pragma once

#include <cassert>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace salsa::core {

struct RevisionId final {
    std::uint64_t value = 0;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return value != 0;
    }

    auto operator<=>(const RevisionId&) const = default;
};

enum class HistoryDirection {
    Undo,
    Redo,
};

template <typename State>
struct RevisionSnapshot final {
    RevisionId id;
    std::shared_ptr<const State> state;
};

struct HistoryCommitResult final {
    RevisionId revision;
    bool created = false;
};

template <typename State>
struct HistoryNavigation final {
    HistoryDirection direction = HistoryDirection::Undo;
    RevisionId from;
    RevisionId to;
    std::shared_ptr<const State> state;
    std::string description{};
};

template <typename State>
class RevisionHistory final {
public:
    explicit RevisionHistory(std::shared_ptr<const State> initialState) {
        assert(initialState != nullptr);
        entries_.push_back(Entry{
            RevisionId{ 1 },
            RevisionId{},
            std::move(initialState),
            {},
        });
        checkpointRevision_ = entries_.front().id;
        checkpointState_ = entries_.front().state;
    }

    RevisionHistory(const RevisionHistory&) = delete;
    RevisionHistory& operator=(const RevisionHistory&) = delete;
    RevisionHistory(RevisionHistory&&) noexcept = default;
    RevisionHistory& operator=(RevisionHistory&&) noexcept = default;

    [[nodiscard]] HistoryCommitResult commit(
        std::shared_ptr<const State> state,
        std::string description) {
        assert(state != nullptr);
        assert(!description.empty());

        const auto& current = entries_[currentIndex_];
        if (state == current.state) {
            return HistoryCommitResult{ current.id, false };
        }

        if (currentIndex_ + 1 < entries_.size()) {
            entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(currentIndex_ + 1),
                entries_.end());
        }

        assert(nextRevisionValue_ != 0);
        const RevisionId revision{ nextRevisionValue_++ };
        entries_.push_back(Entry{
            revision,
            current.id,
            std::move(state),
            std::move(description),
        });
        currentIndex_ = entries_.size() - 1;
        return HistoryCommitResult{ revision, true };
    }

    [[nodiscard]] std::optional<HistoryNavigation<State>> undo() {
        if (!canUndo()) {
            return std::nullopt;
        }

        const auto& source = entries_[currentIndex_];
        const auto from = source.id;
        const auto description = source.description;
        --currentIndex_;
        const auto& target = entries_[currentIndex_];
        assert(source.parent == target.id);
        return HistoryNavigation<State>{
            HistoryDirection::Undo,
            from,
            target.id,
            target.state,
            description,
        };
    }

    [[nodiscard]] std::optional<HistoryNavigation<State>> redo() {
        if (!canRedo()) {
            return std::nullopt;
        }

        const auto from = entries_[currentIndex_].id;
        ++currentIndex_;
        const auto& target = entries_[currentIndex_];
        assert(target.parent == from);
        return HistoryNavigation<State>{
            HistoryDirection::Redo,
            from,
            target.id,
            target.state,
            target.description,
        };
    }

    [[nodiscard]] RevisionSnapshot<State> currentRevision() const {
        const auto& current = entries_[currentIndex_];
        return RevisionSnapshot<State>{ current.id, current.state };
    }

    [[nodiscard]] std::optional<RevisionSnapshot<State>> revision(
        const RevisionId id) const noexcept {
        const auto found = std::ranges::find(entries_, id, &Entry::id);
        if (found == entries_.end()) return std::nullopt;
        return RevisionSnapshot<State>{ found->id, found->state };
    }

    [[nodiscard]] std::optional<RevisionSnapshot<State>> undoTarget() const noexcept {
        if (!canUndo()) return std::nullopt;
        const auto& target = entries_[currentIndex_ - 1];
        return RevisionSnapshot<State>{ target.id, target.state };
    }

    [[nodiscard]] std::optional<RevisionSnapshot<State>> redoTarget() const noexcept {
        if (!canRedo()) return std::nullopt;
        const auto& target = entries_[currentIndex_ + 1];
        return RevisionSnapshot<State>{ target.id, target.state };
    }

    // Exceptional recovery operation. The target must be an ancestor on the
    // active lineage. Revision identifiers remain monotonic and are never
    // reused after the discarded descendants are removed.
    [[nodiscard]] std::optional<RevisionSnapshot<State>>
        selectAncestorAndDiscardDescendants(const RevisionId id) {
        const auto found = std::ranges::find(entries_, id, &Entry::id);
        if (found == entries_.end()) return std::nullopt;
        const auto index = static_cast<std::size_t>(std::distance(entries_.begin(), found));
        if (index > currentIndex_) return std::nullopt;
        auto cursor = currentIndex_;
        while (cursor > index) {
            if (entries_[cursor].parent != entries_[cursor - 1].id) return std::nullopt;
            --cursor;
        }
        currentIndex_ = index;
        entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index + 1),
            entries_.end());
        return RevisionSnapshot<State>{entries_[currentIndex_].id,
            entries_[currentIndex_].state};
    }

    [[nodiscard]] bool canUndo() const noexcept {
        return currentIndex_ > 0;
    }

    [[nodiscard]] bool canRedo() const noexcept {
        return currentIndex_ + 1 < entries_.size();
    }

    [[nodiscard]] std::optional<std::string_view> undoDescription() const noexcept {
        return canUndo()
            ? std::optional<std::string_view>(entries_[currentIndex_].description)
            : std::nullopt;
    }

    [[nodiscard]] std::optional<std::string_view> redoDescription() const noexcept {
        return canRedo()
            ? std::optional<std::string_view>(entries_[currentIndex_ + 1].description)
            : std::nullopt;
    }

    void markCheckpoint() noexcept {
        const auto& current = entries_[currentIndex_];
        checkpointRevision_ = current.id;
        checkpointState_ = current.state;
    }

    [[nodiscard]] bool markCheckpoint(const RevisionId id) noexcept {
        const auto found = std::ranges::find(entries_, id, &Entry::id);
        if (found == entries_.end()) return false;
        checkpointRevision_ = found->id;
        checkpointState_ = found->state;
        return true;
    }

    [[nodiscard]] bool markCheckpoint(
        const RevisionId id, std::shared_ptr<const State> state) noexcept {
        if (!id.valid() || state == nullptr) return false;
        checkpointRevision_ = id;
        checkpointState_ = std::move(state);
        return true;
    }

    [[nodiscard]] RevisionId checkpointRevision() const noexcept {
        return checkpointRevision_;
    }

    [[nodiscard]] std::shared_ptr<const State> checkpointState() const noexcept {
        return checkpointState_;
    }

    [[nodiscard]] bool isDirty() const noexcept {
        return entries_[currentIndex_].id != checkpointRevision_;
    }

private:
    struct Entry final {
        RevisionId id;
        RevisionId parent;
        std::shared_ptr<const State> state;
        std::string description{};
    };

    std::vector<Entry> entries_{};
    std::size_t currentIndex_ = 0;
    std::uint64_t nextRevisionValue_ = 2;
    RevisionId checkpointRevision_{};
    std::shared_ptr<const State> checkpointState_{};
};

}  // namespace salsa::core
