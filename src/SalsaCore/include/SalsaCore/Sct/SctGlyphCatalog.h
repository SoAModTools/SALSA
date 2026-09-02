#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace salsa::core {

enum class SctGlyphMembership : std::uint8_t {
    None = 0,
    European = 1,
    UsJapanese = 2,
};

struct SctGlyphCatalogEntry final {
    std::string utf8;
    std::uint32_t codePoint = 0;
    std::string displayName;
    std::string category;
    SctGlyphMembership membership = SctGlyphMembership::None;
    std::string provenance;
    std::string confidence;
};

class SctGlyphCatalog final {
public:
    static constexpr std::uint32_t SchemaVersion = 1;

    [[nodiscard]] static const SctGlyphCatalog& legacySupportedSet();
    [[nodiscard]] std::span<const SctGlyphCatalogEntry> entries() const noexcept;
    [[nodiscard]] std::vector<SctGlyphCatalogEntry> search(
        std::string_view query,
        SctGlyphMembership membership = SctGlyphMembership::None) const;

private:
    std::vector<SctGlyphCatalogEntry> entries_;
};

}  // namespace salsa::core
