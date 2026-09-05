#pragma once

#include <cstdint>
#include <compare>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace salsa::core {

enum class ExclusiveOperationPageRole {
    Configuration,
    Processing,
    Review,
    Commit,
    Finishing,
    Summary,
};

enum class ExclusiveOperationProgressVisibility {
    Hidden,
    Visible,
};

enum class ExclusiveOperationPageLayout {
    Compact,
    Standard,
    Expanded,
};

struct ExclusiveOperationLogicalSize final {
    int width = 0;
    int height = 0;
    auto operator<=>(const ExclusiveOperationLogicalSize&) const = default;
};

[[nodiscard]] ExclusiveOperationLogicalSize resolveExclusiveOperationSize(
    ExclusiveOperationPageLayout layout,
    ExclusiveOperationLogicalSize pageMinimum,
    ExclusiveOperationLogicalSize availableScreen,
    std::optional<ExclusiveOperationLogicalSize> currentSize = std::nullopt) noexcept;

struct ExclusiveOperationPageNode final {
    ExclusiveOperationPageNode(std::string pageId, ExclusiveOperationPageRole pageRole,
        ExclusiveOperationProgressVisibility pageProgress,
        ExclusiveOperationPageLayout pageLayout)
        : id(std::move(pageId)), role(pageRole), progress(pageProgress),
          layout(pageLayout) {}

    std::string id;
    ExclusiveOperationPageRole role;
    ExclusiveOperationProgressVisibility progress;
    ExclusiveOperationPageLayout layout;
};

struct ExclusiveOperationPageEdge final {
    std::string id;
    std::string source;
    std::string event;
    std::string destination;
};

struct ExclusiveOperationFlowDefinition final {
    std::string entry;
    std::vector<ExclusiveOperationPageNode> pages;
    std::vector<ExclusiveOperationPageEdge> edges;
};

class ExclusiveOperationFlow final {
public:
    using EdgeGuard = std::function<bool(std::string_view)>;

    explicit ExclusiveOperationFlow(ExclusiveOperationFlowDefinition definition);

    [[nodiscard]] const ExclusiveOperationFlowDefinition& definition() const noexcept;
    [[nodiscard]] const std::string& currentPage() const noexcept;
    [[nodiscard]] const ExclusiveOperationPageNode* currentNode() const noexcept;
    [[nodiscard]] std::vector<std::string> validationErrors() const;
    [[nodiscard]] std::vector<const ExclusiveOperationPageEdge*> outgoing(
        const EdgeGuard& enabled = {}) const;
    [[nodiscard]] bool dispatch(std::string_view event, const EdgeGuard& enabled = {});

private:
    ExclusiveOperationFlowDefinition definition_;
    std::string currentPage_;
};

class ExclusiveOperationGate final {
private:
    struct State;

public:
    class Lease final {
    public:
        Lease() = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;
        ~Lease();

        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] std::uint64_t generation() const noexcept;
        void release() noexcept;

    private:
        friend class ExclusiveOperationGate;
        Lease(std::shared_ptr<State> state, std::uint64_t generation);
        std::shared_ptr<State> state_{};
        std::uint64_t generation_ = 0;
    };

    ExclusiveOperationGate();
    [[nodiscard]] std::optional<Lease> tryAcquire();
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept;

private:
    std::shared_ptr<State> state_;
};

}  // namespace salsa::core
