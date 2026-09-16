#include "SalsaCore/Editor/Localization.h"
#include <algorithm>
#include <charconv>
#include <set>
#include <stdexcept>
namespace salsa::core::editor {
namespace {
namespace sct = spice::sct;
void need(bool b, const char *message) {
    if (!b)
        throw std::runtime_error(message);
}
template <class T, class F> Result<T> attempt(F f) {
    try {
        return Result<T>::success(f());
    } catch (const std::exception &e) {
        return Result<T>::failure(
            Diagnostic{DiagnosticSeverity::Error, DiagnosticCode::InvalidSctAuthoringProject, e.what()});
    }
}
std::uint64_t number(std::string_view s) {
    std::uint64_t n{};
    auto r = std::from_chars(s.data(), s.data() + s.size(), n);
    need(r.ec == std::errc{} && r.ptr == s.data() + s.size(), "Invalid decimal value");
    return n;
}
std::string escaped(std::string_view s) {
    std::string out;
    for (char c : s) {
        if (c == '\\' || c == ')')
            out += '\\';
        out += c;
    }
    return out;
}
const std::vector<std::string> commands{"a", "b", "c", "d", "e", "p", "r", "s", "u", "x", "wc", "wo"};
std::string encodeText(const sct::SctTextValue &value) {
    if (auto p = std::get_if<sct::SctPlainText>(&value))
        return "\\plain:" + p->utf8;
    if (std::holds_alternative<sct::SctEmptyIndexedText>(value))
        return "\\empty";
    auto p = std::get_if<sct::SctMessage>(&value);
    need(p, "Opaque text cannot be exported as a translation");
    std::string out = "\\message"; // Explicit kind, including an intentionally empty message.
    if (p->headerUtf8)
        out += "\\h(" + escaped(*p->headerUtf8) + ")";
    for (const auto &item : p->body.elements) {
        if (auto t = std::get_if<sct::SctTextChunk>(&item)) {
            out += escaped(t->utf8);
            continue;
        }
        const auto &c = std::get<sct::SctInlineCommand>(item);
        out += '\\';
        out += commands.at(static_cast<std::size_t>(c.code));
        if (auto a = std::get_if<sct::SctDecimalCommandArgument>(&c.argument))
            out += '(' + (a->value ? std::to_string(*a->value) : std::string{}) + ')';
        if (auto a = std::get_if<sct::SctByteListCommandArgument>(&c.argument)) {
            out += '[';
            for (std::size_t i = 0; i < a->values.size(); ++i) {
                if (i)
                    out += ',';
                out += std::to_string(a->values[i]);
            }
            out += ']';
        }
        out += "{}"; // Unambiguous boundary before ordinary text starting with command letters.
    }
    return out;
}
sct::SctTextValue decodeText(std::string_view s) {
    if (s.starts_with("\\plain:"))
        return sct::SctPlainText{std::string(s.substr(7))};
    if (s == "\\empty")
        return sct::SctEmptyIndexedText{};
    if (s.starts_with("\\message"))
        s.remove_prefix(8);
    sct::SctMessage out;
    std::size_t at = 0;
    std::string chunk;
    auto flush = [&] {
        if (!chunk.empty()) {
            out.body.elements.push_back(sct::SctTextChunk{std::move(chunk)});
            chunk.clear();
        }
    };
    while (at < s.size()) {
        char c = s[at++];
        if (c != '\\') {
            chunk += c;
            continue;
        }
        need(at < s.size(), "Incomplete text escape");
        c = s[at++];
        if (c == '\\' || c == ')') {
            chunk += c;
            continue;
        }
        flush();
        if (c == 'h') {
            need(out.body.elements.empty() && !out.headerUtf8 && at < s.size() && s[at++] == '(',
                 "Header must precede message body");
            std::string h;
            bool closed = false;
            while (at < s.size()) {
                char x = s[at++];
                if (x == ')') {
                    closed = true;
                    break;
                }
                if (x == '\\') {
                    need(at < s.size(), "Incomplete header escape");
                    x = s[at++];
                }
                h += x;
            }
            need(closed, "Unclosed message header");
            out.headerUtf8 = std::move(h);
            continue;
        }
        std::string code(1, c);
        if (c == 'w') {
            need(at < s.size(), "Incomplete command");
            code += s[at++];
        }
        auto pos = std::ranges::find(commands, code);
        need(pos != commands.end(), "Unknown message command");
        sct::SctInlineCommand command;
        command.code = static_cast<sct::SctMessageCommandCode>(pos - commands.begin());
        if (at < s.size() && s[at] == '(') {
            auto end = s.find(')', ++at);
            need(end != s.npos, "Unclosed decimal argument");
            sct::SctDecimalCommandArgument arg;
            if (end != at) {
                auto n = number(s.substr(at, end - at));
                need(n <= UINT32_MAX, "Decimal argument out of range");
                arg.value = static_cast<std::uint32_t>(n);
            }
            command.argument = arg;
            at = end + 1;
        } else if (at < s.size() && s[at] == '[') {
            auto end = s.find(']', ++at);
            need(end != s.npos, "Unclosed byte argument");
            sct::SctByteListCommandArgument arg;
            while (at < end) {
                auto comma = s.find(',', at);
                auto stop = comma == s.npos ? end : std::min(end, comma);
                auto n = number(s.substr(at, stop - at));
                need(n <= 255, "Byte argument out of range");
                arg.values.push_back(static_cast<std::uint8_t>(n));
                at = stop + 1;
            }
            command.argument = std::move(arg);
            at = end + 1;
        }
        if (s.substr(at, 2) == "{}")
            at += 2;
        out.body.elements.push_back(command);
    }
    flush();
    return out;
}
using Rows = std::vector<std::vector<std::string>>;
Rows read(std::string_view s) {
    need(s.size() <= 64 * 1024 * 1024, "CSV exceeds 64 MiB");
    if (s.starts_with("\xef\xbb\xbf"))
        s.remove_prefix(3);
    Rows rows;
    std::vector<std::string> row;
    std::string cell;
    bool quoted = false, closed = false, start = true;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        need(c != '\0', "NUL is not valid CSV text");
        if (quoted) {
            if (c == '"') {
                if (i + 1 < s.size() && s[i + 1] == '"') {
                    cell += '"';
                    ++i;
                } else {
                    quoted = false;
                    closed = true;
                }
            } else
                cell += c;
            continue;
        }
        if (c == '"') {
            need(start, "Unexpected quote in CSV cell");
            quoted = true;
            start = false;
            continue;
        }
        if (c == ',' || c == '\r' || c == '\n') {
            row.push_back(std::move(cell));
            cell.clear();
            closed = false;
            start = true;
            if (c != ',') {
                if (c == '\r' && i + 1 < s.size() && s[i + 1] == '\n')
                    ++i;
                rows.push_back(std::move(row));
                row.clear();
            }
            continue;
        }
        need(!closed, "Characters after closing CSV quote");
        cell += c;
        start = false;
    }
    need(!quoted, "Unclosed CSV quote");
    if (!row.empty() || !cell.empty() || closed) {
        row.push_back(std::move(cell));
        rows.push_back(std::move(row));
    }
    return rows;
}
void write(std::string &out, const std::vector<std::string> &row) {
    for (std::size_t i = 0; i < row.size(); ++i) {
        if (i)
            out += ',';
        out += '"';
        for (char c : row[i]) {
            if (c == '"')
                out += '"';
            out += c;
        }
        out += '"';
    }
    out += "\r\n";
}
} // namespace
Result<std::string> LocalizationCsv::text(const spice::sct::SctTextValue &v) {
    return attempt<std::string>([&] { return encodeText(v); });
}
Result<spice::sct::SctTextValue> LocalizationCsv::text(std::string_view v) {
    return attempt<spice::sct::SctTextValue>([&] { return decodeText(v); });
}
Result<std::string> LocalizationCsv::encode(const LocalizationCatalog &c,
                                            const std::vector<std::string> &languages) {
    return attempt<std::string>([&] {
        need(!languages.empty(), "Select at least one language column");
        std::set<std::string> unique;
        for (const auto &l : languages)
            need(!l.empty() && unique.insert(l).second, "Empty or duplicate language column");
        std::string out;
        write(out, {"# SALSA localization", "1"});
        write(out, {"# Project", c.revision.project.value});
        std::vector<std::string> header{"Message"};
        header.insert(header.end(), languages.begin(), languages.end());
        write(out, header);
        for (const auto &m : c.messages) {
            std::vector<std::string> row{std::to_string(m.id.value)};
            for (const auto &l : languages) {
                auto t = std::ranges::find(m.translations, l, &TranslationCell::language);
                row.push_back(t == m.translations.end() || !t->value ? "" : encodeText(*t->value));
            }
            write(out, row);
        }
        return out;
    });
}
Result<std::vector<TranslationCell>> LocalizationCsv::decode(std::string_view s,
                                                             const LocalizationCatalog &c) {
    return attempt<std::vector<TranslationCell>>([&] {
        auto rows = read(s);
        need(rows.size() >= 3 && rows[0] == std::vector<std::string>{"# SALSA localization", "1"},
             "Unsupported localization CSV header");
        need(rows[1] == std::vector<std::string>{"# Project", c.revision.project.value},
             "CSV belongs to another project");
        const auto &header = rows[2];
        need(header.size() > 1 && header[0] == "Message", "Expected Message followed by language columns");
        std::set<std::string> languages;
        for (std::size_t i = 1; i < header.size(); ++i)
            need(!header[i].empty() && languages.insert(header[i]).second,
                 "Duplicate or empty language column");
        std::set<std::uint64_t> seen;
        std::vector<TranslationCell> out;
        for (std::size_t r = 3; r < rows.size(); ++r) {
            const auto &row = rows[r];
            need(row.size() == header.size(), "CSV row has the wrong number of columns");
            auto id = SctLogicalTextId{number(row[0])};
            need(seen.insert(id.value).second, "Repeated message row");
            need(std::ranges::find(c.messages, id, &LocalizationMessage::id) != c.messages.end(),
                 "Unknown logical message identity");
            for (std::size_t i = 1; i < row.size(); ++i)
                out.push_back(
                    {id, header[i], row[i].empty() ? std::nullopt : std::optional(decodeText(row[i]))});
        }
        return out;
    });
}
} // namespace salsa::core::editor
