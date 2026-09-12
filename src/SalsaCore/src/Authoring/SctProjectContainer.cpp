#include "SalsaCore/Authoring/SctProjectContainer.h"
#include "SalsaCore/Foundation/Hashing.h"
#include <lodepng.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace salsa::core {
namespace {
Diagnostic error(const std::exception& e) {
    return {DiagnosticSeverity::Error, DiagnosticCode::PersistenceReadFailed,
        std::string("Invalid SALSA project container: ") + e.what()};
}
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool validName(std::string_view name) {
    if (name.empty() || name.size() > 240 || name.front() == '/' || name.back() == '/') return false;
    std::size_t start = 0;
    while (start < name.size()) {
        auto end = name.find('/', start); if (end == std::string_view::npos) end = name.size();
        const auto part = name.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") return false;
        start = end + 1;
    }
    return std::ranges::all_of(name, [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '/' || c == '.' || c == '-' || c == '_';
    });
}
void append(std::vector<std::byte>& out, std::span<const std::byte> data) {
    require(data.size() <= SctProjectContainer::MaximumContainerBytes - out.size(), "Container limit exceeded.");
    out.insert(out.end(), data.begin(), data.end());
}
void integer(std::vector<std::byte>& out, std::uint64_t value, unsigned width) {
    require(out.size() <= SctProjectContainer::MaximumContainerBytes - width, "Container limit exceeded.");
    for (unsigned i = 0; i < width; ++i) out.push_back(static_cast<std::byte>((value >> (8 * i)) & 255));
}
struct Reader {
    std::span<const std::byte> data;
    std::span<const std::byte> take(std::size_t count) {
        require(count <= data.size(), "Truncated record."); auto value = data.first(count); data = data.subspan(count); return value;
    }
    std::uint64_t integer(unsigned width) {
        auto bytes = take(width); std::uint64_t value = 0;
        for (unsigned i = 0; i < width; ++i) value |= std::uint64_t(std::to_integer<unsigned>(bytes[i])) << (8 * i);
        return value;
    }
};
}
Result<std::vector<std::byte>> SctProjectContainer::encode(const SctProjectRecords& records) {
    try {
        require(!records.empty() && records.size() <= 4096, "Invalid record count.");
        std::vector<std::byte> out;
        constexpr std::string_view magic = "SALSAPRJ";
        append(out, std::as_bytes(std::span(magic.data(), magic.size())));
        integer(out, 1, 4); integer(out, records.size(), 4);
        std::size_t total = 0;
        for (const auto& [name, bytes] : records) {
            require(validName(name), "Invalid record name.");
            require(bytes.size() <= MaximumRecordBytes && bytes.size() <= MaximumContainerBytes - total, "Record limit exceeded.");
            total += bytes.size();
            std::vector<unsigned char> compressed;
            require(lodepng::compress(compressed, reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()) == 0, "Compression failed.");
            auto hash = sha256(bytes); require(bool(hash), "Hashing failed.");
            integer(out, name.size(), 4); integer(out, bytes.size(), 8); integer(out, compressed.size(), 8);
            append(out, std::span(hash.value().bytes()));
            append(out, std::as_bytes(std::span(name.data(), name.size())));
            append(out, std::as_bytes(std::span(compressed)));
        }
        return Result<std::vector<std::byte>>::success(std::move(out));
    } catch (const std::exception& e) { return Result<std::vector<std::byte>>::failure(error(e)); }
}
Result<SctProjectRecords> SctProjectContainer::decode(std::span<const std::byte> bytes) {
    try {
        require(bytes.size() <= MaximumContainerBytes, "Container limit exceeded.");
        Reader reader{bytes}; const auto magic = reader.take(8);
        require(std::memcmp(magic.data(), "SALSAPRJ", 8) == 0 && reader.integer(4) == 1, "Unsupported container format/version.");
        const auto count = reader.integer(4); require(count > 0 && count <= 4096, "Invalid record count.");
        SctProjectRecords records; std::size_t total = 0;
        for (std::uint64_t i = 0; i < count; ++i) {
            const auto nameSize = reader.integer(4), size = reader.integer(8), compressedSize = reader.integer(8);
            require(nameSize > 0 && nameSize <= 240 && size <= MaximumRecordBytes && size <= MaximumContainerBytes - total,
                "Record limit exceeded.");
            total += static_cast<std::size_t>(size);
            const auto expected = reader.take(32), nameBytes = reader.take(static_cast<std::size_t>(nameSize));
            const std::string name(reinterpret_cast<const char*>(nameBytes.data()), nameBytes.size());
            require(validName(name) && !records.contains(name), "Invalid or duplicate record name.");
            require(compressedSize <= reader.data.size(), "Invalid compressed length.");
            auto compressed = reader.take(static_cast<std::size_t>(compressedSize));
            auto settings = lodepng_default_decompress_settings;
            settings.max_output_size = std::max<std::size_t>(1, static_cast<std::size_t>(size));
            std::vector<unsigned char> decoded;
            require(lodepng::decompress(decoded, reinterpret_cast<const unsigned char*>(compressed.data()), compressed.size(), settings) == 0
                && decoded.size() == size, "Corrupt compressed record or expansion limit exceeded.");
            auto raw = std::as_bytes(std::span(decoded)); auto digest = sha256(raw);
            require(digest && std::ranges::equal(digest.value().bytes(), expected), "Record checksum mismatch.");
            records.emplace(name, std::vector<std::byte>(raw.begin(), raw.end()));
        }
        require(reader.data.empty(), "Trailing container data.");
        return Result<SctProjectRecords>::success(std::move(records));
    } catch (const std::exception& e) { return Result<SctProjectRecords>::failure(error(e)); }
}
} // namespace salsa::core
