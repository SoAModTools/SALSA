#pragma once
#include "Evaluation.h"
#include <algorithm>
#include <memory>
#include <utility>
#include <cassert>
#include <stdexcept>

namespace salsa::evaluation {
inline constexpr uint32_t NoSlot=UINT32_MAX;
inline void expect(bool ok,const char* message) { if(!ok) throw std::invalid_argument(message); }
inline uint32_t slotCount(size_t value) { expect(value<NoSlot,"Internal slot limit exceeded"); return static_cast<uint32_t>(value); }

// Common paged ID directory. Slots and pages are private, never authoring identity.
template<class State> class PagedDirectory {
    static constexpr size_t PageSize=4096;
    using Page=std::array<State,PageSize>;
public:
    explicit PagedDirectory(std::pmr::memory_resource* r):resource_(r),pages_(r){}
    ~PagedDirectory() { for(auto* page:pages_) if(page) { std::destroy_at(page); resource_->deallocate(page,sizeof(Page),alignof(Page)); } }
    PagedDirectory(const PagedDirectory&)=delete;
    template<class Key> State& ensure(Key id) {
        expect(id.value && id.value/PageSize<NoSlot,"Instruction directory limit exceeded");
        const auto page=static_cast<size_t>(id.value/PageSize);
        if(page>=pages_.size()) pages_.resize(page+1,nullptr);
        if(!pages_[page]) {
            auto* memory=static_cast<Page*>(resource_->allocate(sizeof(Page),alignof(Page)));
            std::construct_at(memory); pages_[page]=memory; ++allocatedPages_;
        }
        return (*pages_[page])[id.value%PageSize];
    }
    template<class Key> State* find(Key id) const noexcept {
        const auto page=id.value/PageSize;
        return page<pages_.size() && pages_[page]? &(*pages_[page])[id.value%PageSize]:nullptr;
    }
    uint64_t bytes() const noexcept { return allocatedPages_*sizeof(Page)+pages_.capacity()*sizeof(Page*); }
private:
    size_t allocatedPages_{};
    std::pmr::memory_resource* resource_;
    std::pmr::vector<Page*> pages_;
};

} // namespace salsa::evaluation
