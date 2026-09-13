#pragma once
#include "Evaluation.h"
#include <iterator>

namespace salsa::evaluation {
// Borrowed rows: native values or explicitly decoded little-endian table records.
// Iteration returns small values, never per-row heap objects. The input owner must
// outlive load(); the host retains none of these views after load completes.
template<class T> class InputRows {
public:
    using Decode=T(*)(std::span<const std::byte>,std::span<const std::byte>);
    InputRows()=default;
    InputRows(std::span<const T> values):values_(values),count_(values.size()){}
    InputRows(std::span<const std::byte> bytes,size_t stride,Decode decode,std::span<const std::byte> auxiliary={}):bytes_(bytes),auxiliary_(auxiliary),count_(bytes.size()/stride),stride_(stride),decode_(decode){}
    size_t size() const { return count_; }
    T operator[](size_t i) const { return decode_?decode_(bytes_.subspan(i*stride_,stride_),auxiliary_):values_[i]; }
    struct Iterator {
        using value_type=T; using difference_type=std::ptrdiff_t;
        using iterator_category=std::random_access_iterator_tag;
        const InputRows* rows{}; difference_type index{};
        T operator*() const { return (*rows)[static_cast<size_t>(index)]; }
        T operator[](difference_type n) const { return *(*this+n); }
        Iterator& operator++(){++index;return *this;} Iterator operator++(int){auto old=*this;++*this;return old;}
        Iterator& operator--(){--index;return *this;} Iterator operator--(int){auto old=*this;--*this;return old;}
        Iterator& operator+=(difference_type n){index+=n;return *this;} Iterator& operator-=(difference_type n){index-=n;return *this;}
        friend Iterator operator+(Iterator i,difference_type n){return i+=n;} friend Iterator operator+(difference_type n,Iterator i){return i+=n;}
        friend Iterator operator-(Iterator i,difference_type n){return i-=n;} friend difference_type operator-(Iterator a,Iterator b){return a.index-b.index;}
        auto operator<=>(const Iterator&) const=default;
    };
    Iterator begin() const { return {this,0}; } Iterator end() const { return {this,static_cast<std::ptrdiff_t>(count_)}; }
private:
    std::span<const T> values_;
    std::span<const std::byte> bytes_,auxiliary_;
    size_t count_{},stride_{}; Decode decode_{};
};
struct IdentityInput { InstructionId instruction; std::array<OperandId,3> operands; InputRows<FactIdentity> facts; };
inline IdentityInput identityInput(const RecordIdentity& seed) { return {seed.instruction,seed.operands,std::span<const FactIdentity>(seed.facts)}; }
inline IdentityInput identityInput(IdentityInput seed) { return seed; }
struct SectionInput { SectionId id; ScriptId script; std::string name; InputRows<InstructionId> order; };
struct CheckpointInput { uint64_t revision{},highWater{}; InputRows<IdentityInput> identities; };
struct BinaryProgramInput {
    ProjectId project;
    std::vector<Script> scripts; std::vector<SectionInput> sections; std::vector<Module> modules; std::vector<std::string> evidence;
    InputRows<Expression> expressions; InputRows<Record> records; InputRows<InstructionId> reserved;
    std::optional<CheckpointInput> checkpoint;
};
} // namespace salsa::evaluation
