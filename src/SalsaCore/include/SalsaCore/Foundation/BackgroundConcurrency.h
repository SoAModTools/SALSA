#pragma once
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
namespace salsa::core {
unsigned automaticBackgroundWorkers(unsigned logicalCores) noexcept;
// Application-wide admission budget. Import/export consumers share permits;
// lowering the limit lets existing work finish before admitting more work.
class BackgroundConcurrency : public std::enable_shared_from_this<BackgroundConcurrency> {
public:
    explicit BackgroundConcurrency(unsigned limit);
    unsigned limit() const;
    void setLimit(unsigned);
    class Permit {
    public:
        Permit(Permit &&) noexcept = default;
        Permit(const Permit &) = delete;
        ~Permit();

    private:
        friend class BackgroundConcurrency;
        explicit Permit(std::shared_ptr<BackgroundConcurrency> owner) : owner_(std::move(owner)) {}
        std::shared_ptr<BackgroundConcurrency> owner_;
    };
    std::optional<Permit> acquire(std::stop_token);

private:
    mutable std::mutex mutex_;
    std::condition_variable_any changed_;
    unsigned limit_, active_ = 0;
};
std::shared_ptr<BackgroundConcurrency> backgroundConcurrency();
} // namespace salsa::core
