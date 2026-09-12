#include "SalsaCore/Sct/SctExpressionLanguage.h"
#include "SpiceMLD/MldDocumentImporter.h"
#include "SpiceSCT/SctOpcodeMetadata.h"
#include "SpiceSCT/SctStructuredControlFlow.h"
#include "Survey.h"
#include "smores/script/Relationships.h"
#include <algorithm>
#include <chrono>
#include <map>
#include <set>
namespace salsa::survey {
namespace rel = smores::script;
namespace sct = spice::sct;
namespace {
std::string id(auto value) {
    return std::to_string(value.value());
}
Json parameter(const sct::SctDocumentParameter &p, const sct::SctDocument &d) {
    Json value = {{"parameter", p.schemaIndex}, {"text", nullptr}, {"expression", nullptr}};
    if (const auto *e = std::get_if<sct::SctCanonicalExpression>(&p.value))
        value["expression"] = salsa::core::SctExpressionLanguage::project(*e).text;
    if (const auto *e = std::get_if<sct::SctEncodedWordValue>(&p.value))
        value["word"] = e->value;
    if (const auto *ref = std::get_if<sct::SctSupplementaryTextReference>(&p.value))
        for (const auto &text : d.supplementaryText)
            if (text.id == ref->target)
                if (const auto *plain = std::get_if<sct::SctPlainText>(&text.value))
                    value["text"] = plain->utf8;
    return value;
}
Json response(const Json &script, const std::set<std::string> &seeds, bool traverse) {
    std::map<std::string, Json> instructions;
    for (const auto &i : script.at("instructions"))
        instructions[i.at("id").get<std::string>()] = i;
    std::map<std::string, std::vector<std::string>> edges;
    for (const auto &e : script.at("controlFlow"))
        if (!e.at("target").is_null())
            edges[e.at("source").get<std::string>()].push_back(e.at("target").get<std::string>());
    std::set<std::string> included = seeds;
    std::vector<std::string> pending(seeds.begin(), seeds.end());
    if (traverse)
        for (std::size_t n = 0; n < pending.size(); ++n)
            for (const auto &next : edges[pending[n]])
                if (included.insert(next).second)
                    pending.push_back(next);
    Json result = {{"calls", Json::array()},
                   {"jumps", Json::array()},
                   {"writes", Json::array()},
                   {"conditions", Json::array()},
                   {"transitions", Json::array()}};
    std::set<std::string> conditions;
    for (const auto &a : script.at("accesses"))
        if (included.contains(a.at("instruction").get<std::string>())) {
            if (a.at("operation") != "read") {
                auto row = a;
                row["instructionInfo"] = instructions.at(a.at("instruction").get<std::string>());
                result["writes"].push_back(std::move(row));
            } else if (a.at("condition") == true && conditions.insert(a.at("instruction").get<std::string>()).second)
                result["conditions"].push_back(
                    {{"instruction", a.at("instruction")}, {"expression", a.at("expression")}});
        }
    for (const auto &e : script.at("controlFlow"))
        if ((e.at("kind") == static_cast<unsigned>(sct::SctControlFlowKind::Call) ||
             e.at("kind") == static_cast<unsigned>(sct::SctControlFlowKind::Jump)) &&
            included.contains(e.at("source").get<std::string>())) {
            Json call = e;
            call["instructionInfo"] = instructions.at(e.at("source").get<std::string>());
            if (!e.at("target").is_null() && instructions.contains(e.at("target").get<std::string>()))
                call["targetSection"] = instructions.at(e.at("target").get<std::string>()).at("section");
            result[e.at("kind") == static_cast<unsigned>(sct::SctControlFlowKind::Call) ? "calls" : "jumps"].push_back(
                std::move(call));
        }
    if (script.contains("resourceContext"))
        for (const auto &t : script.at("resourceContext").at("transitions"))
            if (included.contains(t.at("instruction").get<std::string>()))
                result["transitions"].push_back(t);
    result["instructionCount"] = included.size();
    return result;
}
Json routes(const Json &script, const std::string &instruction) {
    Json result = Json::array();
    if (!script.contains("resourceContext"))
        return result;
    for (auto region : script.at("resourceContext").at("regions"))
        if (region.at("controller") == instruction) {
            for (auto &arm : region["arms"]) {
                std::set<std::string> selected;
                for (const auto &i : arm.at("instructions"))
                    selected.insert(i.get<std::string>());
                arm["response"] = response(script, selected, false);
            }
            result.push_back(std::move(region));
        }
    return result;
}
void annotateTransitions(Json &value, const Json &run, const std::string &dataset) {
    if (!value.is_structured())
        return;
    if (value.is_object() && value.contains("transitions"))
        for (auto &transition : value["transitions"]) {
            transition["candidateScripts"] = Json::array();
            transition["targetStatus"] = "unresolved";
            if (!transition.at("target").is_null() && !transition.at("target").at("text").is_null()) {
                auto target = rel::assetKey(transition.at("target").at("text").get<std::string>());
                if (!target.ends_with(".sct"))
                    target += ".sct";
                for (const auto &file : run.at("files")) {
                    const auto &identity = file.at("identity");
                    if (identity.value("kind", "sct") != "sct" || identity.at("dataset") != dataset)
                        continue;
                    auto asset = identity.at("path").get<std::string>();
                    if (asset == target ||
                        (target.find('/') == std::string::npos && asset.substr(asset.find_last_of('/') + 1) == target))
                        transition["candidateScripts"].push_back(identity.at("key"));
                }
                const auto n = transition["candidateScripts"].size();
                transition["targetStatus"] = n == 0 ? "missing" : n == 1 ? "candidate" : "ambiguous";
            }
        }
    for (auto &child : value)
        if (child.is_structured())
            annotateTransitions(child, run, dataset);
}
Json factsFor(const Json &f, const std::filesystem::path &output) {
    auto name = f.at("facts").get<std::string>();
    if (name != hash(f.at("identity").dump()) + ".json.zlib")
        throw std::runtime_error("Resource fact filename mismatch");
    auto facts = readFacts(output / "facts" / name);
    if (facts.at("identity") != f.at("identity"))
        throw std::runtime_error("Resource fact identity mismatch");
    return facts;
}
} // namespace
Json extractResource(std::vector<std::byte> bytes, const Json &identity) {
    const auto start = std::chrono::steady_clock::now();
    if (identity.at("sha256") != hash(bytes))
        throw std::runtime_error("MLD source changed after inventory");
    const auto platform = identity.at("platform").get<std::string>();
    if (platform != "dreamcast" && platform != "gamecube")
        throw std::runtime_error("Invalid MLD platform");
    spice::mld::MldImportOptions options;
    options.platformHint =
        platform == "dreamcast" ? spice::mld::MldPlatform::Dreamcast : spice::mld::MldPlatform::GameCube;
    auto imported = spice::mld::MldDocumentImporter::importBytes(
        {reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()}, options);
    Json facts = {{"identity", identity},
                  {"status", imported.document ? "analyzed" : "failed"},
                  {"diagnostics", Json::array()},
                  {"entries", Json::array()}};
    for (const auto &d : imported.diagnostics)
        facts["diagnostics"].push_back({{"severity", static_cast<unsigned>(d.severity)},
                                        {"message", d.message},
                                        {"offset", d.decodedOffset ? Json(*d.decodedOffset) : Json(nullptr)}});
    if (imported.document) {
        const rel::ResourceIdentity source{identity.at("dataset"), identity.at("path"), identity.at("sha256"),
                                           "spice-mld-survey-2:" + identity.at("sha256").get<std::string>()};
        auto resource = rel::describe(source, rel::ResourceRole::Unclassified, *imported.document);
        const auto providers = rel::signalProviders({{resource}});
        for (const auto &entry : resource.entries) {
            Json row = {{"index", std::to_string(entry.tableIndex)},
                        {"localId", std::to_string(entry.localId.value)},
                        {"entryId", entry.entryId},
                        {"tblId", entry.tableId},
                        {"function", rel::assetKey(entry.function)},
                        {"parameters", entry.parameters},
                        {"document", source.document},
                        {"signal", nullptr},
                        {"handler", nullptr}};
            if (row["function"] == "eventhook") {
                for (const auto &p : providers)
                    if (p.entry.tableIndex == entry.tableIndex)
                        row["signal"] = {{"variable", variableName(0, p.bit)},
                                         {"parameter", 8},
                                         {"evidence", p.evidence.description}};
                if (row["signal"].is_null())
                    row["signalIssue"] =
                        "Eventhook has no representable signal candidate; original parameters retained.";
            } else if (auto name = rel::handlerName(entry.tableId))
                row["handler"] = *name;
            facts["entries"].push_back(std::move(row));
        }
    }
    facts["loadMs"] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return facts;
}
Json scriptResourceContext(const sct::SctDocument &doc) {
    Json result = {{"regions", Json::array()}, {"transitions", Json::array()}, {"resourceOperations", Json::array()}};
    auto structured = sct::SctStructuredControlFlowAnalysis::build(doc);
    for (const auto &section : structured.sections()) {
        std::map<sct::SctBasicBlockId, const sct::SctStructuredBasicBlock *> blocks;
        for (const auto &b : section.blocks)
            blocks[b.id] = &b;
        for (const auto &region : section.regions)
            if (region.controllerSite) {
                Json row = {{"section", id(section.section)},
                            {"controller", id(region.controllerSite->instruction)},
                            {"kind", static_cast<unsigned>(region.id.kind)},
                            {"continuation", region.join ? Json(id(region.join->entryInstruction)) : Json(nullptr)},
                            {"parent", region.parent ? Json(id(region.parent->headerInstruction)) : Json(nullptr)},
                            {"arms", Json::array()}};
                for (const auto &arm : region.arms) {
                    Json list = Json::array(), labels = Json::array();
                    for (const auto &block : arm.blocks)
                        for (const auto &i : blocks.at(block)->instructions)
                            list.push_back(id(i));
                    for (const auto &label : arm.caseLabels)
                        labels.push_back({{"ordinal", label.repeatedGroupOrdinal},
                                          {"value", label.value ? Json(*label.value) : Json(nullptr)}});
                    row["arms"].push_back({{"kind", static_cast<unsigned>(arm.kind)},
                                           {"entry", arm.entry ? Json(id(arm.entry->entryInstruction)) : Json(nullptr)},
                                           {"instructions", list},
                                           {"caseLabels", labels}});
                }
                result["regions"].push_back(std::move(row));
            }
    }
    for (const auto &sec : doc.sections)
        if (const auto *body = std::get_if<sct::SctScriptSectionContent>(&sec.content))
            for (const auto &i : body->instructions) {
                const auto *schema = sct::findSctOpcodeSchema(i.opcode);
                if (!schema || schema->semantic.effect.kind != sct::SctOpcodeEffectKind::LoadScript)
                    continue;
                Json target = nullptr;
                for (const auto &p : i.fixedParameters)
                    if (p.schemaIndex == schema->semantic.effect.firstParameter)
                        target = parameter(p, doc);
                result["transitions"].push_back(
                    {{"section", id(sec.id)},
                     {"instruction", id(i.id)},
                     {"opcode", i.opcode},
                     {"target", target},
                     {"confidence", static_cast<unsigned>(schema->semantic.effect.confidence)}});
            }
    for (const auto &operation : rel::resourceOperations(doc))
        result["resourceOperations"].push_back({{"section", id(operation.section)},
                                                {"instruction", id(operation.instruction)},
                                                {"kind", static_cast<unsigned>(operation.kind)},
                                                {"asset", operation.asset ? Json(*operation.asset) : Json(nullptr)}});
    return result;
}
Json linkResource(const Json &resource, const Json &script, bool details) {
    if (resource.at("identity").at("dataset") != script.at("identity").at("dataset"))
        throw std::runtime_error("Cross-dataset MLD binding rejected");
    Json result = {{"resource", resource.at("identity")},
                   {"script", script.at("identity")},
                   {"eventhooks", Json::array()},
                   {"handlers", Json::array()}};
    if (resource.at("status") != "analyzed" || script.at("status") != "analyzed")
        return result;
    for (const auto &entry : resource.at("entries")) {
        if (!entry.at("signal").is_null()) {
            const auto var = entry.at("signal").at("variable");
            Json consumers = Json::array();
            std::set<std::string> predicates;
            for (const auto &a : script.at("accesses"))
                if (a.at("operation") == "read" && a.at("variable") == var) {
                    auto read = a;
                    if (details && read.at("condition") == true &&
                        predicates.insert(read.at("instruction").get<std::string>()).second) {
                        read["routes"] = routes(script, read.at("instruction").get<std::string>());
                        read["boundaryStatus"] = read["routes"].empty() ? "unrecognized" : "structured";
                    }
                    consumers.push_back(std::move(read));
                }
            result["eventhooks"].push_back({{"entry", entry.at("index")},
                                            {"variable", var},
                                            {"consumers", consumers},
                                            {"evidence", entry.at("signal").at("evidence")}});
        }
        if (!entry.at("handler").is_null())
            for (const auto &section : script.at("sections"))
                if (section.at("name") == entry.at("handler") && section.at("code") == true) {
                    Json match = {{"entry", entry.at("index")},
                                  {"section", section.at("id")},
                                  {"name", section.at("name")},
                                  {"evidence", "MLD tblId matches an M-section in an explicitly associated script; "
                                               "distinct entries remain distinct."}};
                    if (details) {
                        std::set<std::string> seeds;
                        for (const auto &i : script.at("instructions"))
                            if (i.at("section") == section.at("id"))
                                seeds.insert(i.at("id").get<std::string>());
                        match["response"] = response(script, seeds, true);
                    }
                    result["handlers"].push_back(std::move(match));
                }
    }
    return result;
}
void indexResources(Json &run, Json &index, const std::filesystem::path &output) {
    Json resources = Json::object(), summary = {{"selected", 0ull},
                                                {"analyzed", 0ull},
                                                {"failed", 0ull},
                                                {"cached", 0ull},
                                                {"eventhooks", 0ull},
                                                {"unresolvedSignals", 0ull},
                                                {"unmatchedSignalBindings", 0ull},
                                                {"signalReadBindings", 0ull},
                                                {"directHandlerBindings", 0ull},
                                                {"gaps", Json::array()}};
    std::map<std::string, const Json *> scripts;
    for (const auto &f : run.at("files"))
        if (f.at("identity").value("kind", "sct") == "sct")
            scripts[f.at("identity").at("key").get<std::string>()] = &f;
    auto add = [&](const char *k, std::uint64_t n) { summary[k] = summary[k].get<std::uint64_t>() + n; };
    for (const auto &f : run.at("files"))
        if (f.at("identity").value("kind", "sct") == "mld") {
            add("selected", 1);
            const auto key = f.at("identity").at("key").get<std::string>();
            Json row = {{"identity", f.at("identity")},
                        {"status", f.at("status")},
                        {"diagnostics", f.value("diagnostics", Json::array())},
                        {"association", f.at("identity").at("association")},
                        {"entries", Json::array()},
                        {"links", Json::array()}};
            if (f.at("status") != "analyzed") {
                add("failed", 1);
                summary["gaps"].push_back({{"resource", key}, {"reason", f.at("status")}});
                resources[key] = row;
                continue;
            }
            add("analyzed", 1);
            add("cached", f.value("cached", false));
            add("eventhooks", f.at("eventhooks").get<std::uint64_t>());
            auto facts = factsFor(f, output);
            row["entries"] = facts.at("entries");
            for (const auto &entry : row["entries"])
                if (entry.at("function") == "eventhook" && entry.at("signal").is_null())
                    add("unresolvedSignals", 1);
            for (const auto &path : f.at("identity").at("association").at("scripts")) {
                auto scriptKey = f.at("identity").at("dataset").get<std::string>() + ":" + path.get<std::string>();
                if (!scripts.contains(scriptKey) || scripts.at(scriptKey)->at("status") != "analyzed") {
                    summary["gaps"].push_back(
                        {{"resource", key}, {"script", scriptKey}, {"reason", "associated script unavailable"}});
                    continue;
                }
                auto links = linkResource(facts, factsFor(*scripts.at(scriptKey), output));
                for (const auto &hook : links.at("eventhooks")) {
                    add("signalReadBindings", hook.at("consumers").size());
                    if (hook.at("consumers").empty())
                        add("unmatchedSignalBindings", 1);
                }
                add("directHandlerBindings", links.at("handlers").size());
                row["links"].push_back(std::move(links));
            }
            resources[key] = std::move(row);
        }
    index["resources"] = std::move(resources);
    run["summary"]["resources"] = std::move(summary);
}
Json queryResources(const Json &run, const Json &index, const std::filesystem::path &output, const std::string &kind,
                    const std::string &value, const std::string &script) {
    Json result = Json::array();
    if (!index.contains("resources"))
        return result;
    std::map<std::string, const Json *> files;
    for (const auto &f : run.at("files"))
        files[f.at("identity").at("key").get<std::string>()] = &f;
    for (const auto &[key, r] : index.at("resources").items()) {
        bool selected = kind == "resource" && key == value;
        if (kind == "variable")
            for (const auto &e : r.at("entries"))
                if (!e.at("signal").is_null() && e.at("signal").at("variable") == value)
                    selected = true;
        if (kind != "variable" && kind != "resource")
            for (const auto &asset : r.at("association").at("scripts"))
                if (r.at("identity").at("dataset").get<std::string>() + ":" + asset.get<std::string>() == script)
                    selected = true;
        if (!selected)
            continue;
        Json row = r;
        if (r.at("status") == "analyzed" && (kind == "resource" || kind == "entrypoint" || kind == "cutscene")) {
            row["links"] = Json::array();
            auto resource = factsFor(*files.at(key), output);
            for (const auto &link : r.at("links")) {
                auto target = link.at("script").at("key").get<std::string>();
                if (kind != "resource" && target != script)
                    continue;
                auto facts = factsFor(*files.at(target), output);
                auto detailed = linkResource(resource, facts, true);
                if (kind != "resource") {
                    const auto section = value.substr(value.rfind('#') + 1);
                    std::set<std::string> sections{section};
                    if (kind == "cutscene")
                        for (const auto &scope : facts.at("scopes"))
                            if (scope.at("id") == section)
                                for (const auto &s : scope.at("sections"))
                                    sections.insert(s.get<std::string>());
                    const auto reaches = [&](const Json &r) {
                        for (const auto *kind : {"calls", "jumps"})
                            for (const auto &call : r.at(kind))
                                // A shared helper is not evidence of entering the cutscene itself.
                                if (call.contains("targetSection") && call.at("targetSection") == section)
                                    return true;
                        return false;
                    };
                    Json handlers = Json::array(), hooks = Json::array();
                    for (const auto &h : detailed.at("handlers"))
                        if ((kind == "entrypoint" && h.at("section") == section) ||
                            (kind == "cutscene" && reaches(h.at("response"))))
                            handlers.push_back(h);
                    for (auto h : detailed.at("eventhooks")) {
                        Json consumers = Json::array();
                        for (const auto &c : h.at("consumers")) {
                            bool relevant = sections.contains(c.at("section").get<std::string>());
                            if (kind == "cutscene" && c.contains("routes"))
                                for (const auto &r : c.at("routes"))
                                    for (const auto &arm : r.at("arms"))
                                        relevant |= reaches(arm.at("response"));
                            if (relevant)
                                consumers.push_back(c);
                        }
                        h["consumers"] = std::move(consumers);
                        if (!h["consumers"].empty())
                            hooks.push_back(std::move(h));
                    }
                    detailed["handlers"] = std::move(handlers);
                    detailed["eventhooks"] = std::move(hooks);
                }
                row["links"].push_back(std::move(detailed));
            }
        }
        if (kind == "variable") {
            Json entries = Json::array();
            for (const auto &e : row["entries"])
                if (!e.at("signal").is_null() && e.at("signal").at("variable") == value)
                    entries.push_back(e);
            row["entries"] = std::move(entries);
            for (auto &link : row["links"]) {
                Json hooks = Json::array();
                for (const auto &h : link["eventhooks"])
                    if (h.at("variable") == value)
                        hooks.push_back(h);
                link["eventhooks"] = std::move(hooks);
                link["handlers"] = Json::array();
            }
        }
        annotateTransitions(row, run, r.at("identity").at("dataset").get<std::string>());
        result.push_back(std::move(row));
    }
    return result;
}
} // namespace salsa::survey
