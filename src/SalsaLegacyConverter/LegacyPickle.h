#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace salsa::legacy {

using ValueId = std::uint64_t;
inline constexpr ValueId InvalidValueId = 0;

enum class ValueKind : std::uint8_t {
    Null, Boolean, Integer, Float, String, Bytes, List, Tuple, Set,
    FrozenSet, Dictionary, Global, Object,
};

struct StoredObject final {
    std::string className{};
    std::vector<std::pair<std::string, ValueId>> attributes{};
};

// Pickle values live in one append-only spool. Memory retains only compact
// node descriptors and the pickle VM's integer stack/memo handles.
class ValueStore final {
public:
    explicit ValueStore(std::filesystem::path path);
    ~ValueStore();
    ValueStore(const ValueStore&) = delete;
    ValueStore& operator=(const ValueStore&) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const std::string& error() const noexcept;
    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    [[nodiscard]] std::uint64_t nodeCount() const noexcept;

    [[nodiscard]] ValueId create(ValueKind kind);
    [[nodiscard]] ValueId createBoolean(bool value);
    [[nodiscard]] ValueId createFloat(std::uint64_t bits);
    [[nodiscard]] ValueId createText(ValueKind kind, std::string_view value);
    [[nodiscard]] ValueId createBytes(const std::vector<std::byte>& value);
    [[nodiscard]] ValueId createObject(std::string_view className);
    void appendItems(ValueId target, const std::vector<ValueId>& items);
    void appendEntries(ValueId target,
        const std::vector<std::pair<ValueId, ValueId>>& entries);
    void replaceItems(ValueId target, const std::vector<ValueId>& items);
    void replaceEntries(ValueId target,
        const std::vector<std::pair<ValueId, ValueId>>& entries);
    void replaceText(ValueId target, std::string_view value);

    [[nodiscard]] ValueKind kind(ValueId value) const;
    [[nodiscard]] bool boolean(ValueId value) const;
    [[nodiscard]] std::uint64_t floatBits(ValueId value) const;
    [[nodiscard]] std::string text(ValueId value) const;
    [[nodiscard]] std::vector<std::byte> bytes(ValueId value) const;
    [[nodiscard]] std::vector<ValueId> items(ValueId value) const;
    [[nodiscard]] std::vector<std::pair<ValueId, ValueId>> entries(ValueId value) const;
    [[nodiscard]] StoredObject object(ValueId value) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

struct PickleLimits final {
    std::uint64_t maxInputBytes = 1ull << 30;
    std::uint64_t maxMemoEntries = 32ull << 20;
    std::uint64_t maxStackEntries = 8ull << 20;
    std::uint64_t maxContainerEntries = 64ull << 20;
    std::uint64_t maxTextBytes = 64ull << 20;
    std::uint64_t maxRawBytes = 128ull << 20;
};

struct PickleResult final {
    std::shared_ptr<ValueStore> store{};
    ValueId root = InvalidValueId;
    std::string error{};
    std::uint32_t protocol = 0;
    [[nodiscard]] explicit operator bool() const noexcept {
        return store != nullptr && root != InvalidValueId;
    }
};

[[nodiscard]] PickleResult readLegacyPickle(
    const std::filesystem::path& path,
    const std::filesystem::path& spoolPath,
    const PickleLimits& limits,
    bool disableResourceLimits,
    const std::function<void(std::uint64_t, std::uint64_t)>& progress = {});

[[nodiscard]] ValueId dictionaryValue(const ValueStore& store,
    ValueId dictionary, std::string_view key);
[[nodiscard]] ValueId attributeValue(const ValueStore& store,
    ValueId object, std::string_view key);
[[nodiscard]] std::string valueString(const ValueStore& store, ValueId value);

}  // namespace salsa::legacy
