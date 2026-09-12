#include "Survey.h"
#include "SalsaCore/Authoring/SctInterpretation.h"
#include "SalsaCore/Sct/SctDocumentLoader.h"
#include "SpiceSCT/SctDocumentIndex.h"
#include "lodepng.h"
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
namespace salsa::survey {
namespace core = salsa::core;
namespace sct = spice::sct;
namespace {
constexpr std::size_t MaximumBytes = 256ull * 1024 * 1024;
std::string decimal(auto id) {
    return std::to_string(id.value());
}
Json site(const sct::SctParameterSite &p) {
    return {{"instruction", decimal(p.instruction)},
            {"parameter", p.parameter.schemaIndex},
            {"group", p.parameter.repeatedGroupOrdinal ? Json(*p.parameter.repeatedGroupOrdinal) : Json(nullptr)}};
}
const char *action(core::SctStateAccessKind k) {
    switch (k) {
    case core::SctStateAccessKind::Read:
        return "read";
    case core::SctStateAccessKind::Set:
        return "set";
    case core::SctStateAccessKind::Clear:
        return "clear";
    case core::SctStateAccessKind::Toggle:
        return "toggle";
    case core::SctStateAccessKind::Assign:
        return "assign";
    default:
        return "unknown";
    }
}
Json diagnostics(const core::SctLoadResult &loaded) {
    Json out = Json::array();
    for (const auto &d : loaded.infrastructureDiagnostics)
        out.push_back({{"severity", static_cast<int>(d.severity)}, {"stage", "load"}, {"message", d.message}});
    const auto append = [&](const auto &ds) {
        for (const auto &d : ds)
            out.push_back({{"severity", static_cast<int>(d.severity)},
                           {"stage", static_cast<int>(d.stage)},
                           {"code", d.code},
                           {"message", d.message},
                           {"offset", d.payloadOffset ? Json(*d.payloadOffset) : Json(nullptr)}});
    };
    if (loaded.document)
        append(loaded.document->diagnostics);
    else if (loaded.inspection)
        append(loaded.inspection->diagnostics);
    return out;
}
} // namespace
std::string hash(std::span<const std::byte> bytes) {
    auto h = core::sha256(bytes);
    if (!h)
        throw std::runtime_error("SHA-256 failed");
    return h.value().toHex();
}
std::string hash(const std::string &text) {
    return hash(std::as_bytes(std::span(text.data(), text.size())));
}
std::vector<std::byte> readBytes(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
        throw std::runtime_error("Cannot read file");
    auto size = in.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > MaximumBytes)
        throw std::runtime_error("File exceeds 256 MiB limit");
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    in.seekg(0);
    in.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
    if (!in)
        throw std::runtime_error("File read incomplete");
    return bytes;
}
void writeText(const std::filesystem::path &path, const std::string &text) {
    std::filesystem::create_directories(path.parent_path());
    auto temporary = path;
    temporary += L".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(text.data(), text.size());
        out.flush();
        if (!out)
            throw std::runtime_error("Report write failed");
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Report replacement failed");
}
Json readJson(const std::filesystem::path &path) {
    auto bytes = readBytes(path);
    return Json::parse(reinterpret_cast<const char *>(bytes.data()),
                       reinterpret_cast<const char *>(bytes.data()) + bytes.size());
}
void writeFacts(const std::filesystem::path &path, const Json &facts) {
    const auto payload = facts.dump();
    const Json envelope = {{"format", "jahorta.salsa.connectivity-facts"},
                           {"schemaVersion", SchemaVersion},
                           {"sha256", hash(payload)},
                           {"payload", facts}};
    const auto text = envelope.dump();
    if (text.size() > MaximumBytes)
        throw std::runtime_error("Fact record exceeds 256 MiB limit");
    std::vector<unsigned char> compressed;
    if (lodepng::compress(compressed, reinterpret_cast<const unsigned char *>(text.data()), text.size()))
        throw std::runtime_error("Fact compression failed");
    writeText(path, std::string(reinterpret_cast<const char *>(compressed.data()), compressed.size()));
}
Json readFacts(const std::filesystem::path &path) {
    auto bytes = readBytes(path);
    auto settings = lodepng_default_decompress_settings;
    settings.max_output_size = MaximumBytes;
    std::vector<unsigned char> plain;
    if (lodepng::decompress(plain, reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size(), settings))
        throw std::runtime_error("Fact decompression failed");
    auto envelope = Json::parse(plain);
    if (envelope.at("format") != "jahorta.salsa.connectivity-facts" || envelope.at("schemaVersion") != SchemaVersion ||
        envelope.at("sha256") != hash(envelope.at("payload").dump()))
        throw std::runtime_error("Fact schema or checksum mismatch");
    return envelope.at("payload");
}
std::string variableName(unsigned kind, std::uint32_t index) {
    const char *names[] = {"BitVar", "ByteVar", "IntVar", "FloatVar"};
    if (kind > 3)
        throw std::runtime_error("Unknown variable bank");
    return std::string(names[kind]) + ":" + std::to_string(index);
}
Json extract(std::vector<std::byte> bytes, const Json &identity) {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    Json out = {{"identity", identity}, {"status", "failed"}, {"diagnostics", Json::array()}};
    if (identity.at("sha256") != hash(bytes))
        throw std::runtime_error("Source changed after inventory");
    auto loc = core::AssetLocator::fromRelativePath(identity.at("path").get<std::string>());
    if (!loc)
        throw std::runtime_error("Invalid asset locator");
    auto digest = core::sha256(bytes).value();
    const auto platform = identity.at("platform").get<std::string>();
    const auto region = identity.at("region").get<std::string>();
    if (platform != "gamecube" && platform != "dreamcast")
        throw std::runtime_error("Invalid platform");
    if (region != "jp" && region != "us" && region != "eu")
        throw std::runtime_error("Invalid region");
    std::optional<sct::SctKnownTextConvention> convention;
    if (!identity.at("textConvention").is_null()) {
        auto name = identity.at("textConvention").get<std::string>();
        for (const auto &c : sct::kSctKnownTextConventions)
            if (c.stableName == name)
                convention = c.convention;
        if (!convention)
            throw std::runtime_error("Unknown text convention");
    }
    auto loaded = core::SctDocumentLoader::loadSnapshot(
        {{loc.value(), bytes.size(), {digest}}, std::move(bytes)},
        {platform == "gamecube" ? core::GamePlatform::GameCube : core::GamePlatform::Dreamcast,
         region == "eu"   ? core::GameRegion::Europe
         : region == "jp" ? core::GameRegion::Japan
                          : core::GameRegion::NorthAmerica,
         {digest}},
        convention);
    const auto parsed = Clock::now();
    out["diagnostics"] = diagnostics(loaded);
    out["loadMs"] = std::chrono::duration<double, std::milli>(parsed - start).count();
    if (!loaded.document)
        return out;
    const auto &doc = *loaded.document->document;
    auto facts = core::SctInterpretation::extract(doc, {1}, {identity.at("sha256").get<std::string>()});
    out["status"] = "analyzed";
    out["readiness"] = static_cast<unsigned>(loaded.document->readiness);
    out["textConvention"] = loaded.document->provenance->textConvention
                                ? Json(core::sctTextConventionName(*loaded.document->provenance->textConvention))
                                : Json(nullptr);
    out["sections"] = Json::array();
    out["instructions"] = Json::array();
    out["accesses"] = Json::array();
    out["unresolved"] = Json::array();
    out["scopes"] = Json::array();
    out["controlFlow"] = Json::array();
    std::map<sct::SctInstructionId, sct::SctSectionId> owners;
    std::regex handler("M[0-9]{5}");
    for (const auto &sec : doc.sections) {
        const auto *body = std::get_if<sct::SctScriptSectionContent>(&sec.content);
        const bool entry =
            sec.nameBytes == "init" || sec.nameBytes == "loop" || std::regex_match(sec.nameBytes, handler);
        out["sections"].push_back({{"id", decimal(sec.id)},
                                   {"name", sec.nameBytes},
                                   {"code", body != nullptr},
                                   {"entrypoint", entry},
                                   {"instructionCount", body ? body->instructions.size() : 0}});
        if (body)
            for (std::size_t n = 0; n < body->instructions.size(); ++n) {
                const auto &i = body->instructions[n];
                owners[i.id] = sec.id;
                Json row = {{"id", decimal(i.id)},
                            {"section", decimal(sec.id)},
                            {"ordinal", n},
                            {"opcode", i.opcode},
                            {"skipRefresh", i.skipRefresh},
                            {"schedule", i.scheduledExpression
                                             ? Json(core::SctExpressionLanguage::project(*i.scheduledExpression).text)
                                             : Json(nullptr)},
                            {"offset", nullptr}};
                if (const auto *receipt = loaded.document->provenance->importReceipt())
                    if (auto location = receipt->sourceMap.location(sct::SctDocumentEntityId{i.id}))
                        row["offset"] = location->primarySpan.offset;
                out["instructions"].push_back(std::move(row));
            }
    }
    for (const auto &a : facts.accesses) {
        auto row = site(*a.target.site);
        row["section"] = decimal(owners.at(a.target.site->instruction));
        row["variable"] = variableName(static_cast<unsigned>(a.variable.kind), a.variable.index);
        row["operation"] = action(a.kind);
        row["expression"] = a.expression;
        row["condition"] = a.condition;
        out["accesses"].push_back(std::move(row));
    }
    for (const auto &u : facts.unresolvedWrites) {
        auto row = site(*u.target.site);
        row["section"] = decimal(owners.at(u.target.site->instruction));
        row["operation"] = action(u.kind);
        row["expression"] = u.expression;
        out["unresolved"].push_back(std::move(row));
    }
    for (const auto &scope : facts.effects) {
        Json sections = Json::array();
        for (const auto &sec : scope.sections)
            sections.push_back(decimal(*sec.section));
        out["scopes"].push_back({{"id", decimal(*scope.root.section)},
                                 {"sections", sections},
                                 {"writes", scope.writes},
                                 {"unresolvedWrites", scope.unresolvedWrites}});
    }
    for (const auto &e : facts.controlFlow)
        out["controlFlow"].push_back(
            {{"source", decimal(e.sourceInstruction)},
             {"target", e.targetInstruction ? Json(decimal(*e.targetInstruction)) : Json(nullptr)},
             {"kind", static_cast<unsigned>(e.kind)},
             {"site", e.origin ? site(*e.origin) : Json(nullptr)}});
    out["extractMs"] = std::chrono::duration<double, std::milli>(Clock::now() - parsed).count();
    return out;
}
Json summarize(const Json &f) {
    Json result = {{"identity", f.at("identity")},
                   {"status", f.at("status")},
                   {"diagnostics", f.value("diagnostics", Json::array())},
                   {"loadMs", f.value("loadMs", 0.0)},
                   {"extractMs", f.value("extractMs", 0.0)}};
    if (f.at("status") != "analyzed")
        return result;
    std::set<std::string> grouped;
    for (const auto &scope : f.at("scopes"))
        for (const auto &sec : scope.at("sections"))
            grouped.insert(sec.get<std::string>());
    Json variables = Json::object();
    std::uint64_t ungrouped = 0;
    for (const auto &a : f.at("accesses")) {
        auto var = a.at("variable").get<std::string>();
        if (!variables.contains(var))
            variables[var] = {{"read", 0ull}, {"set", 0ull}, {"clear", 0ull}, {"toggle", 0ull}, {"assign", 0ull}};
        auto op = a.at("operation").get<std::string>();
        variables[var][op] = variables[var][op].get<std::uint64_t>() + 1;
        if (op != "read" && !grouped.contains(a.at("section").get<std::string>()))
            ++ungrouped;
    }
    result["variables"] = variables;
    result["sections"] = f.at("sections").size();
    result["instructions"] = f.at("instructions").size();
    result["accesses"] = f.at("accesses").size();
    result["scopes"] = f.at("scopes").size();
    result["ungroupedWrites"] = ungrouped;
    result["unresolved"] = f.at("unresolved").size();
    auto stem = std::filesystem::path(f.at("identity").at("path").get<std::string>()).stem().string();
    std::smatch match;
    std::regex number("[mM][eE]([0-9]+)[a-zA-Z].*");
    std::string kind = "other";
    if (std::regex_match(stem, match, number)) {
        const auto n = std::stoul(match[1]);
        kind = n < 99 ? "field" : n == 99 ? "overworld" : n < 200 ? "dungeon" : n < 500 ? "cutscene" : "ship-battle";
    }
    result["scriptGroup"] = kind;
    return result;
}
Json select(const Json &facts, const std::string &kind, const std::string &value) {
    Json result = {{"identity", facts.at("identity")},
                   {"status", facts.at("status")},
                   {"diagnostics", facts.value("diagnostics", Json::array())},
                   {"records", Json::array()}};
    if (facts.at("status") != "analyzed")
        return result;
    std::set<std::string> sections;
    bool found = kind == "script" || kind == "variable";
    if (kind == "cutscene")
        for (const auto &scope : facts.at("scopes"))
            if (scope.at("id") == value) {
                found = true;
                result["scope"] = scope;
                for (const auto &sec : scope.at("sections"))
                    sections.insert(sec.get<std::string>());
            }
    if (kind == "entrypoint")
        for (const auto &sec : facts.at("sections"))
            if (sec.at("id") == value && sec.at("entrypoint") == true) {
                found = true;
                sections.insert(value);
            }
    if (!found)
        throw std::runtime_error("Unknown cutscene/entrypoint key");
    std::map<std::string, Json> instructions;
    for (const auto &i : facts.at("instructions"))
        instructions[i.at("id").get<std::string>()] = i;
    result["sections"] = Json::array();
    for (const auto &sec : facts.at("sections"))
        if (sections.empty() || sections.contains(sec.at("id").get<std::string>()))
            result["sections"].push_back(sec);
    if (kind == "script")
        result["scopes"] = facts.at("scopes");
    if (kind == "cutscene" || kind == "entrypoint") {
        result["controlFlow"] = Json::array();
        for (const auto &e : facts.at("controlFlow")) {
            const auto i = instructions.find(e.at("source").get<std::string>());
            if (i != instructions.end() && sections.contains(i->second.at("section").get<std::string>()))
                result["controlFlow"].push_back(e);
        }
    }
    for (const auto *name : {"accesses", "unresolved"})
        for (auto row : facts.at(name)) {
            if (kind == "variable" && row.value("variable", "") != value)
                continue;
            if (!sections.empty() && !sections.contains(row.at("section").get<std::string>()))
                continue;
            row["instructionInfo"] = instructions.at(row.at("instruction").get<std::string>());
            row["script"] = facts.at("identity").at("key");
            result["records"].push_back(std::move(row));
        }
    return result;
}
} // namespace salsa::survey
