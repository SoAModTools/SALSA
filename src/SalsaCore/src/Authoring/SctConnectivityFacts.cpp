#include "SalsaCore/Authoring/SctConnectivityFacts.h"
#include "SpiceSCT/SctOpcodeMetadata.h"
#include "SpiceSCT/SctStructuredControlFlow.h"
#include "smores/script/Relationships.h"
#include <regex>
#include <set>
namespace salsa::core {
namespace core = salsa::core;
namespace rel = smores::script;
namespace sct = spice::sct;
using Json = SctConnectivityJson;
namespace {
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
std::string variableName(unsigned kind, std::uint32_t index) {
    const char *names[] = {"BitVar", "ByteVar", "IntVar", "FloatVar"};
    if (kind > 3)
        throw std::runtime_error("Unknown variable bank");
    return std::string(names[kind]) + ":" + std::to_string(index);
}
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
} // namespace
Json sctResourceContext(const sct::SctDocument &doc) {
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
Json sctResourceLinks(const Json &resource, const Json &script, bool details) {
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
Json sctConnectivityFacts(const sct::SctDocument &doc, const Json &identity,
                          std::span<const SctConnectivityRoot> roots) {
    Json out = {{"identity", identity}, {"status", "analyzed"}, {"diagnostics", Json::array()}};
    auto facts = SctInterpretation::extract(doc, {1}, {identity.at("sha256").get<std::string>()}, roots);
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
        out["scopes"].push_back(
            {{"id", decimal(scope.root.section.value_or(scope.sections.empty() ? sct::SctSectionId{}
                                                                               : *scope.sections.front().section))},
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
    out["resourceContext"] = sctResourceContext(doc);
    return out;
}
} // namespace salsa::core
