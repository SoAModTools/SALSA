#include "SalsaCore/Foundation/BackgroundConcurrency.h"
#include <algorithm>
#include <thread>
namespace salsa::core {
unsigned automaticBackgroundWorkers(unsigned logicalCores) noexcept { return std::max(1u, logicalCores / 2); }
BackgroundConcurrency::BackgroundConcurrency(unsigned limit) : limit_(std::max(1u, limit)) {}
unsigned BackgroundConcurrency::limit() const {
    std::lock_guard lock(mutex_);
    return limit_;
}
void BackgroundConcurrency::setLimit(unsigned n) {
    {
        std::lock_guard lock(mutex_);
        limit_ = std::max(1u, n);
    }
    changed_.notify_all();
}
std::optional<BackgroundConcurrency::Permit> BackgroundConcurrency::acquire(std::stop_token stop) {
    std::unique_lock lock(mutex_);
    if (!changed_.wait(lock, stop, [&] { return active_ < limit_; }) || stop.stop_requested())
        return {};
    auto owner = shared_from_this();
    ++active_;
    return Permit{std::move(owner)};
}
BackgroundConcurrency::Permit::~Permit() {
    if (owner_) {
        {
            std::lock_guard lock(owner_->mutex_);
            --owner_->active_;
        }
        owner_->changed_.notify_all();
    }
}
std::shared_ptr<BackgroundConcurrency> backgroundConcurrency() {
    static auto shared = std::make_shared<BackgroundConcurrency>(
        automaticBackgroundWorkers(std::thread::hardware_concurrency()));
    return shared;
}
} // namespace salsa::core
