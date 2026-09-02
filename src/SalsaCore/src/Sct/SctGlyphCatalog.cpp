#include "SalsaCore/Sct/SctGlyphCatalog.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <unordered_map>

namespace salsa::core {
namespace {

constexpr std::string_view EuropeanGlyphs =
    "‘’“”‚„…€ƒ†‡ˆ‰Š‹ŒŽ•–—˜™š›œžŸ¡¢£¤¥¦§¨©ª¬®¯°±²³´µ¶·¸¹º«»¼½¾¿ÀÁÂÃÄÅÆÇÈÉÊËÌÍÎÏÐÑÒÓÔÕÖ×ØÙÚÛÜÝÞàáâãäåæçèéêëìíîïðñòóôõö÷øùúûüýþÿ";
constexpr std::string_view UsJapaneseGlyphs =
    "、。，．・：；？！゛゜´｀¨＾￣＿ヽヾゝゞ〃仝々〆〇ー―‐／＼uFF5E∥｜…‥‘’“”（）〔〕［］｛｝〈〉《》「」『』【】＋－±×÷＝≠＜＞≦≧∞∴♂♀°′″℃￥＄￠￡％＃＆＊＠§☆★○●◎◇◆□■△▲▽▼※〒→←↑↓〓∈∋⊆⊇⊂⊃∪∩∧∨￢⇒⇔∀∃∠⊥⌒∂∇≡≒≪≫√∽∝∵∫∬Å‰♯♭♪†‡¶◯ΑΒΓΔΕΖΗΘΙΚΛΜΝΞΟΠΡΣΤΥΦΧΨΩαβγδεζηθικλμνξοπρστυφχψωАБВГДЕЁЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯабвгдеёжзийклмнопрстуфхцчшщъыьэюя│┌┐┘└├┬┤┴┼━┃┏┓┛┗┣┳┫┻╋┠┯┨┷┿┝┰┥┸╂①②③④⑤⑥⑦⑧⑨⑩⑪⑫⑬⑭⑮⑯⑰⑱⑲⑳ⅠⅡⅢⅣⅤⅥⅦⅧⅨⅩⅰⅱⅲⅳⅴⅵⅶⅷⅸⅹ㍉㌔㌢㍍㌘㌧㌃㌶㍑㍗㌍㌦㌣㌫㍊㌻㎜㎝㎞㎎㎏㏄㎡㍻〝〟№㏍℡㊤㊥㊦㊧㊨㈱㈲㈹㍾㍽㍼∮∑∟⊿￤＇＂";
constexpr std::string_view FullwidthBackslashAndTilde =
    "\xEF\xBC\xBC\xEF\xBD\x9E";

[[nodiscard]] std::vector<std::pair<std::uint32_t, std::string>> decode(
    const std::string_view input) {
    std::vector<std::pair<std::uint32_t, std::string>> result;
    for (std::size_t offset = 0; offset < input.size();) {
        const auto first = static_cast<unsigned char>(input[offset]);
        std::size_t count = first < 0x80 ? 1 : first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
        if (offset + count > input.size()) break;
        std::uint32_t codePoint = first & (count == 1 ? 0x7fu : count == 2 ? 0x1fu : count == 3 ? 0x0fu : 0x07u);
        for (std::size_t index = 1; index < count; ++index)
            codePoint = (codePoint << 6u) | (static_cast<unsigned char>(input[offset + index]) & 0x3fu);
        result.emplace_back(codePoint, std::string(input.substr(offset, count)));
        offset += count;
    }
    return result;
}

[[nodiscard]] std::string lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

[[nodiscard]] bool includes(const SctGlyphMembership value,
    const SctGlyphMembership requested) {
    return requested == SctGlyphMembership::None
        || (static_cast<unsigned>(value) & static_cast<unsigned>(requested)) != 0u;
}

}  // namespace

const SctGlyphCatalog& SctGlyphCatalog::legacySupportedSet() {
    static const auto catalog = [] {
        SctGlyphCatalog result;
        std::unordered_map<std::uint32_t, std::size_t> known;
        const auto add = [&](const std::string_view source,
            const SctGlyphMembership membership, const std::string_view category) {
            for (auto [codePoint, utf8] : decode(source)) {
                if (codePoint < 0x80u) continue;
                if (const auto found = known.find(codePoint); found != known.end()) {
                    auto& entry = result.entries_[found->second];
                    entry.membership = static_cast<SctGlyphMembership>(
                        static_cast<unsigned>(entry.membership)
                        | static_cast<unsigned>(membership));
                    continue;
                }
                SctGlyphCatalogEntry entry;
                entry.utf8 = std::move(utf8);
                entry.codePoint = codePoint;
                entry.displayName = std::format("U+{:04X}", codePoint);
                entry.category = category;
                entry.membership = membership;
                entry.provenance = "legacy SALSA special_chars_widget.py";
                entry.confidence = "Legacy-supported authoring set; export support must be validated";
                known.emplace(codePoint, result.entries_.size());
                result.entries_.push_back(std::move(entry));
            }
        };
        add(EuropeanGlyphs, SctGlyphMembership::European, "European legacy set");
        add(UsJapaneseGlyphs, SctGlyphMembership::UsJapanese, "US/JP legacy set");
        add(FullwidthBackslashAndTilde,
            SctGlyphMembership::UsJapanese, "US/JP legacy set");
        return result;
    }();
    return catalog;
}

std::span<const SctGlyphCatalogEntry> SctGlyphCatalog::entries() const noexcept {
    return entries_;
}

std::vector<SctGlyphCatalogEntry> SctGlyphCatalog::search(
    const std::string_view query, const SctGlyphMembership membership) const {
    std::vector<SctGlyphCatalogEntry> result;
    const auto needle = lower(std::string(query));
    for (const auto& entry : entries_) {
        if (!includes(entry.membership, membership)) continue;
        const auto haystack = lower(entry.utf8 + " " + entry.displayName + " "
            + entry.category + " " + entry.provenance);
        if (needle.empty() || haystack.find(needle) != std::string::npos)
            result.push_back(entry);
    }
    return result;
}

}  // namespace salsa::core
