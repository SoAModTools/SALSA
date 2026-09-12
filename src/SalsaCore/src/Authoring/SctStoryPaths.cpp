#include "SalsaCore/Authoring/SctStoryPaths.h"
#include "SpiceSCT/SctScptEncoding.h"
#include <bit>
#include <cmath>
#include <deque>
#include <functional>
#include <set>
namespace salsa::core {
namespace {
using J = SctConnectivityJson;
namespace sct = spice::sct;
Diagnostic error(std::string s) {
    return {DiagnosticSeverity::Error, DiagnosticCode::InvalidSctAuthoringProject, std::move(s)};
}
// Only a conventional SPICE projection is evaluated. Unknown leaves stay unknown.
std::optional<double> evaluate(const std::string &text, const std::string &known, double value) {
    auto parsed = SctExpressionLanguage::parse(text);
    if (!parsed.succeeded())
        return {};
    auto *program = std::get_if<sct::SctTypedScptProgram>(&parsed.expression->body);
    if (!program)
        return {};
    auto a = sct::analyzeSctScptProgram(*program);
    if (!a.conventionalTree)
        return {};
    std::function<std::optional<double>(const sct::SctScptDerivedExpressionNode &)> visit;
    visit = [&](const auto &n) -> std::optional<double> {
        const auto &op = program->operations.at(n.operationOrdinal);
        if (auto *v = std::get_if<sct::SctScptValueOperation>(&op)) {
            if (v->kind == sct::SctScptValueKind::FloatLiteral && v->payloadWords.size() == 1) {
                auto f = std::bit_cast<float>(v->payloadWords[0]);
                return std::isfinite(f) ? std::optional<double>(f) : std::nullopt;
            }
            if (v->kind == sct::SctScptValueKind::DecimalLiteral) {
                auto x = static_cast<std::int32_t>(v->encodingWord << 8) >> 8;
                return x / 256.0;
            }
            std::string bank;
            if (v->kind == sct::SctScptValueKind::BitVariable)
                bank = "BitVar:";
            if (v->kind == sct::SctScptValueKind::ByteVariable)
                bank = "ByteVar:";
            if (v->kind == sct::SctScptValueKind::IntegerVariable)
                bank = "IntVar:";
            if (v->kind == sct::SctScptValueKind::FloatVariable)
                bank = "FloatVar:";
            if (!bank.empty() &&
                bank + std::to_string(v->encodingWord &
                                      (v->kind == sct::SctScptValueKind::BitVariable       ? 0x1fffffff
                                       : v->kind == sct::SctScptValueKind::IntegerVariable ? 0xffffff
                                                                                           : 0xfffffff)) ==
                    known)
                return value;
            return {};
        }
        auto *b = std::get_if<sct::SctScptBinaryOperation>(&op);
        if (!b || n.children.size() != 2)
            return {};
        if (n.comparisonMode == sct::SctScptComparisonMode::Low16Integer ||
            n.comparisonMode == sct::SctScptComparisonMode::Unknown)
            return {};
        auto l = visit(n.children[0]), r = visit(n.children[1]);
        if (b->encodingWord == 8) {
            if ((l && !*l) || (r && !*r))
                return 0;
            if (l && r)
                return 1;
            return {};
        }
        if (b->encodingWord == 9) {
            if ((l && *l) || (r && *r))
                return 1;
            if (l && r)
                return 0;
            return {};
        }
        if (!l || !r)
            return {};
        switch (b->encodingWord) {
        case 0:
            return *l < *r;
        case 1:
            return *l <= *r;
        case 2:
            return *l > *r;
        case 3:
            return *l >= *r;
        case 4:
            return *l == *r;
        case 5:
            return *l != *r;
        default:
            return {};
        }
    };
    return visit(*a.conventionalTree);
}
std::string nodeKey(const J &f, const std::string &section) {
    return f.at("identity").at("key").template get<std::string>() + "#" + section;
}
std::string predicate(const J &f, const std::string &instruction) {
    for (const auto &a : f.at("accesses"))
        if (a.at("instruction") == instruction && a.at("condition") == true)
            return a.at("expression");
    return {};
}
J guards(const J &f, const std::string &instruction) {
    J result = J::array();
    for (const auto &r : f.at("resourceContext").at("regions"))
        for (const auto &arm : r.at("arms"))
            if (std::ranges::find(arm.at("instructions"), J(instruction)) != arm.at("instructions").end())
                result.push_back({{"controller", r.at("controller")},
                                  {"expression", predicate(f, r.at("controller"))},
                                  {"arm", arm.at("kind")},
                                  {"caseLabels", arm.at("caseLabels")},
                                  {"continuation", r.at("continuation")}});
    return result;
}
std::string selectionKey(const std::vector<std::string> &nodes) {
    auto text = J(nodes).dump();
    return "story-path-1:" + sha256(std::as_bytes(std::span(text))).value().toHex();
}
} // namespace
Result<J> SctStoryPaths::propose(const std::vector<J> &scripts, const std::vector<J> &resources,
                                 const SctStoryPathOptions &o) {
    try {
        if (!o.maximumEdges || o.maximumEdges > 1000000 || !o.maximumPaths || o.maximumPaths > 10000 ||
            !o.maximumSteps || o.maximumSteps > 512)
            throw std::runtime_error("Invalid bounded path limits.");
        J out = {{"rule", "story-path-1"},   {"nodes", J::array()},  {"edges", J::array()},
                 {"components", J::array()}, {"starts", J::array()}, {"layers", J::array()},
                 {"paths", J::array()},      {"gaps", J::array()},   {"truncated", false}};
        std::map<std::string, const J *> files;
        std::map<std::string, J> nodes;
        std::map<std::string, J> transfers;
        for (const auto &f : scripts) {
            auto key = f.at("identity").at("key").get<std::string>();
            if (!files.emplace(key, &f).second)
                throw std::runtime_error("Duplicate qualified script identity.");
            if (f.at("status") != "analyzed") {
                out["gaps"].push_back({{"script", key}, {"status", f.at("status")}});
                continue;
            }
            std::set<std::string> roots;
            for (const auto &scope : f.at("scopes"))
                roots.insert(scope.at("id"));
            for (const auto &sec : f.at("sections"))
                if (sec.at("code") == true) {
                    auto id = nodeKey(f, sec.at("id"));
                    nodes[id] = {{"id", id},
                                 {"script", key},
                                 {"identity", f.at("identity")},
                                 {"section", sec.at("id")},
                                 {"name", sec.at("name")},
                                 {"kind", roots.contains(sec.at("id"))   ? "cutscene"
                                          : sec.at("entrypoint") == true ? "entrypoint"
                                                                         : "helper"}};
                }
        }
        std::set<std::string> signatures;
        const auto add = [&](J edge) {
            if (!signatures.insert(edge.dump()).second)
                return;
            if (out["edges"].size() >= o.maximumEdges) {
                out["truncated"] = true;
                return;
            }
            edge["id"] = std::to_string(out["edges"].size() + 1);
            out["edges"].push_back(std::move(edge));
        };
        for (const auto &[key, fp] : files) {
            const auto &f = *fp;
            if (f.at("status") != "analyzed")
                continue;
            std::map<std::string, J> instructions;
            for (const auto &i : f.at("instructions"))
                instructions[i.at("id")] = i;
            for (const auto &e : f.at("controlFlow")) {
                if (e.at("target").is_null() || !instructions.contains(e.at("target")))
                    continue;
                auto source = e.at("source").get<std::string>(), target = e.at("target").get<std::string>();
                auto from = nodeKey(f, instructions.at(source).at("section")),
                     to = nodeKey(f, instructions.at(target).at("section"));
                if (from == to)
                    continue;
                auto kind = e.at("kind").get<unsigned>();
                if (kind != 0 && kind != 4 && kind != 5)
                    continue;
                J edge = {{"from", from},
                          {"to", to},
                          {"kind", kind == 5   ? "call"
                                   : kind == 4 ? "jump"
                                               : "fallthrough"},
                          {"guards", guards(f, source)},
                          {"evidence", e},
                          {"instruction", instructions.at(source)},
                          {"rank", 3}};
                transfers[key + ":" + source] = edge;
                add(edge);
            }
            for (const auto &t : f.at("resourceContext").at("transitions")) {
                std::vector<std::string> matches;
                if (!t.at("target").is_null() && !t.at("target").at("text").is_null()) {
                    auto name = smores::script::assetKey(t.at("target").at("text").get<std::string>());
                    if (!name.ends_with(".sct"))
                        name += ".sct";
                    for (const auto &[other, g] : files)
                        if (g->at("identity").at("dataset") == f.at("identity").at("dataset")) {
                            const auto asset = g->at("identity").at("path").get<std::string>();
                            if (asset == name || (name.find('/') == std::string::npos &&
                                                  asset.substr(asset.find_last_of('/') + 1) == name))
                                matches.push_back(other);
                        }
                }
                if (matches.size() != 1) {
                    out["gaps"].push_back(
                        {{"script", key},
                         {"transition", t},
                         {"candidates", matches},
                         {"reason", matches.empty() ? "missing or unresolved target" : "ambiguous target"}});
                    continue;
                }
                const auto &destination = *files.at(matches.front());
                std::string entry;
                if (destination.at("status") == "analyzed")
                    for (const auto &s : destination.at("sections"))
                        if (s.at("name") == "loop" && s.at("code") == true)
                            entry = nodeKey(destination, s.at("id"));
                if (entry.empty()) {
                    out["gaps"].push_back({{"script", matches.front()}, {"reason", "destination loop unavailable"}});
                    continue;
                }
                auto action = t.at("instruction").get<std::string>();
                J edge = {{"from", nodeKey(f, t.at("section"))},
                          {"to", entry},
                          {"kind", "script-transition"},
                          {"guards", guards(f, action)},
                          {"evidence", t},
                          {"instruction", instructions.at(action)},
                          {"rank", 3}};
                transfers[key + ":" + action] = edge;
                add(edge);
            }
        }
        std::map<std::string,std::vector<const J*>> transfersByScript;
        for(const auto& [site,t]:transfers){auto from=t.at("from").get<std::string>();transfersByScript[from.substr(0,from.rfind('#'))].push_back(&t);}
        for(const auto& [site,transition]:transfers)if(transition.at("kind")=="script-transition" && transition.at("evidence").at("opcode")==43) {
            const auto from=transition.at("from").get<std::string>();const auto sourceKey=from.substr(0,from.rfind('#'));
            auto asset=files.at(sourceKey)->at("identity").at("path").get<std::string>();asset=asset.substr(asset.find_last_of('/')+1);if(asset.ends_with(".sct"))asset.resize(asset.size()-4);
            auto origin=SctSequenceAuthoring::arrivalLocation(asset);if(!origin)continue;
            const auto destination=transition.at("to").get<std::string>();const auto destKey=destination.substr(0,destination.rfind('#'));
            for(const auto* pointer:transfersByScript[destKey]) { const auto& response=*pointer;
                bool match=false;
                for(const auto& guard:response.at("guards")) {
                    auto text=guard.at("expression").get<std::string>();
                    if(text=="Low16IntVar[15]")for(const auto& label:guard.at("caseLabels"))if(label.at("value")==origin.value().encoded)match=true;
                    auto parsed=SctExpressionLanguage::parse(text);if(!parsed.succeeded())continue;
                    const auto* program=std::get_if<sct::SctTypedScptProgram>(&parsed.expression->body);
                    if(!program||program->operations.size()!=3||guard.at("arm")!=0)continue;
                    const auto* read=std::get_if<sct::SctScptValueOperation>(&program->operations[0]);
                    const auto* literal=std::get_if<sct::SctScptValueOperation>(&program->operations[1]);
                    const auto* op=std::get_if<sct::SctScptBinaryOperation>(&program->operations[2]);
                    if(!read||!literal||!op||read->kind!=sct::SctScptValueKind::IntegerVariableLow16Comparison||(read->encodingWord&0xffffff)!=15||op->encodingWord!=4)continue;
                    double number=-1;
                    if(literal->kind==sct::SctScptValueKind::DecimalLiteral)number=(static_cast<std::int32_t>(literal->encodingWord<<8)>>8)/256.0;
                    if(literal->kind==sct::SctScptValueKind::FloatLiteral&&literal->payloadWords.size()==1)number=std::bit_cast<float>(literal->payloadWords[0]);
                    match|=number==origin.value().encoded;
                }
                if(match)add({{"from",transition.at("from")},{"to",response.at("to")},{"kind","arrival-match"},{"rank",3},{"incoming",transition},{"response",response},{"qualification","Literal-load origin matches an explicit arrival condition; all other response guards still apply."}});
            }
        }
        // Only guards on actual cross-section or cross-script transfers nominate state dependencies.
        struct Write {
            std::string node, dataset;
            J access;
        };
        std::map<std::string, std::vector<Write>> writers;
        for (const auto &f : scripts)
            if (f.at("status") == "analyzed")
                for (const auto &scope : f.at("scopes"))
                    for (const auto &n : scope.at("writes")) {
                        const auto &a = f.at("accesses").at(n.get<std::size_t>());
                        if (a.at("operation") == "set" || a.at("operation") == "clear")
                            writers[f.at("identity").at("dataset").get<std::string>() + ":" +
                                    a.at("variable").get<std::string>()]
                                .push_back({nodeKey(f, scope.at("id")), f.at("identity").at("dataset"), a});
                    }
        for (const auto &[site, transfer] : transfers) {
            auto key = transfer.at("from").get<std::string>();
            key = key.substr(0, key.rfind('#'));
            const auto &f = *files.at(key);
            for (const auto &guard : transfer.at("guards")) {
                auto reads = SctExpressionLanguage::parse(guard.at("expression"));
                if (!reads.succeeded())
                    continue;
                for (const auto &read : SctSequenceAuthoring::stateReads(*reads.expression)) {
                    if (read.storage.kind != SctVariableKind::Bit)
                        continue;
                    auto variable = "BitVar:" + std::to_string(read.storage.index);
                    auto wi = writers.find(f.at("identity").at("dataset").get<std::string>() + ":" + variable);
                    if (wi == writers.end())
                        continue;
                    for (const auto &w : wi->second) {
                        if (transfer.at("to") == w.node)
                            continue;
                        double value = w.access.at("operation") == "set" ? 1 : 0;
                        auto before = evaluate(guard.at("expression"), variable, 1 - value),
                             after = evaluate(guard.at("expression"), variable, value);
                        auto arm = guard.at("arm").get<unsigned>();
                        if (arm > 1)
                            continue;
                        if (arm == 1) {
                            if (before)
                                before = !*before;
                            if (after)
                                after = !*after;
                        }
                        std::string kind = "state-related";
                        if (before != after) {
                            if ((after && *after) || (before && !*before && !after))
                                kind = "enables";
                            else if ((after && !*after) || (before && *before && !after))
                                kind = "inhibits";
                        }
                        add({{"from", w.node},
                             {"to", transfer.at("to")},
                             {"kind", kind},
                             {"rank", wi->second.size() == 1 ? 2 : 1},
                             {"variable", variable},
                             {"write", w.access},
                             {"guard", guard},
                             {"transfer", transfer},
                             {"qualification", "Candidate effect of one observed write; other guards and conditional "
                                               "writes remain relevant."}});
                        if (out["truncated"] == true)
                            break;
                    }
                }
            }
        }
        for (const auto &r : resources) {
            auto key = r.at("script").at("key").get<std::string>();
            if (!files.contains(key))
                continue;
            for (const auto &h : r.at("handlers")) {
                auto id = r.at("resource").at("key").get<std::string>() + "@" + h.at("entry").get<std::string>();
                nodes[id] = {{"id", id}, {"name", h.at("name")}, {"kind", "resource"}, {"identity", r.at("resource")}};
                add({{"from", id},
                     {"to", key + "#" + h.at("section").get<std::string>()},
                     {"kind", "m-handler"},
                     {"evidence", h},
                     {"rank", 3}});
            }
            for (const auto &h : r.at("eventhooks"))
                for (const auto &c : h.at("consumers")) {
                    auto id = r.at("resource").at("key").get<std::string>() + "@" + h.at("entry").get<std::string>();
                    nodes[id] = {
                        {"id", id}, {"name", h.at("variable")}, {"kind", "resource"}, {"identity", r.at("resource")}};
                    add({{"from", id},
                         {"to", key + "#" + c.at("section").get<std::string>()},
                         {"kind", "eventhook"},
                         {"evidence", c},
                         {"variable", h.at("variable")},
                         {"rank", 3}});
                }
        }
        // Iterative Kosaraju avoids recursion limits for large corpora.
        std::map<std::string, std::set<std::string>> edges, reverse;
        for (const auto &[id, n] : nodes) {
            edges[id];
            reverse[id];
            out["nodes"].push_back(n);
        }
        for (const auto &e : out["edges"])
            if (e.at("kind") != "inhibits" && e.at("kind") != "state-related" && nodes.contains(e.at("from")) &&
                nodes.contains(e.at("to"))) {
                edges[e.at("from")].insert(e.at("to"));
                reverse[e.at("to")].insert(e.at("from"));
            }
        std::set<std::string> seen;
        std::vector<std::string> order;
        for (const auto &[id, n] : nodes)
            if (!seen.contains(id)) {
                std::vector<std::pair<std::string, bool>> todo{{id, false}};
                while (!todo.empty()) {
                    auto [at, done] = todo.back();
                    todo.pop_back();
                    if (done) {
                        order.push_back(at);
                        continue;
                    }
                    if (!seen.insert(at).second)
                        continue;
                    todo.emplace_back(at, true);
                    for (const auto &next : edges[at])
                        if (!seen.contains(next))
                            todo.emplace_back(next, false);
                }
            }
        std::map<std::string, std::size_t> component;
        std::vector<std::vector<std::string>> groups;
        for (auto i = order.rbegin(); i != order.rend(); ++i)
            if (!component.contains(*i)) {
                auto index = groups.size();
                groups.emplace_back();
                std::vector<std::string> todo{*i};
                component[*i] = index;
                while (!todo.empty()) {
                    auto at = todo.back();
                    todo.pop_back();
                    groups.back().push_back(at);
                    for (const auto &next : reverse[at])
                        if (!component.contains(next)) {
                            component[next] = index;
                            todo.push_back(next);
                        }
                }
                std::ranges::sort(groups.back());
            }
        std::vector<std::set<std::size_t>> dag(groups.size());
        std::vector<unsigned> incoming(groups.size());
        for (const auto &[from, tos] : edges)
            for (const auto &to : tos)
                if (component[from] != component[to] && dag[component[from]].insert(component[to]).second)
                    ++incoming[component[to]];
        std::vector<std::size_t> roots;
        for (std::size_t i = 0; i < groups.size(); ++i) {
            out["components"].push_back(
                {{"id", std::to_string(i)},
                 {"members", groups[i]},
                 {"cycle", groups[i].size() > 1 || edges[groups[i][0]].contains(groups[i][0])}});
            if (!incoming[i])
                roots.push_back(i);
        }
        for (auto i : roots)
            out["starts"].push_back(std::to_string(i));
        auto frontier = roots;
        while (!frontier.empty()) {
            J layer = J::array();
            std::vector<std::size_t> next;
            for (auto i : frontier) {
                layer.push_back(std::to_string(i));
                for (auto j : dag[i])
                    if (!--incoming[j])
                        next.push_back(j);
            }
            out["layers"].push_back(layer);
            frontier = std::move(next);
        }
        if (o.start) {
            if (!component.contains(*o.start))
                throw std::runtime_error("Unknown path anchor.");
            roots = {component.at(*o.start)};
        }
        std::vector<std::vector<std::size_t>> todo;
        for (auto i : roots)
            todo.push_back({i});
        while (!todo.empty() && out["paths"].size() < o.maximumPaths) {
            auto path = std::move(todo.back());
            todo.pop_back();
            auto at = path.back();
            if (dag[at].empty() || path.size() >= o.maximumSteps) {
                J steps = J::array();
                for (auto i : path)
                    steps.push_back(std::to_string(i));
                out["paths"].push_back(steps);
                if (!dag[at].empty())
                    out["truncated"] = true;
                continue;
            }
            for (auto i : dag[at]) {
                if (todo.size() >= o.maximumPaths) {
                    out["truncated"] = true;
                    break;
                }
                auto branch = path;
                branch.push_back(i);
                todo.push_back(std::move(branch));
            }
        }
        if (!todo.empty())
            out["truncated"] = true;
        return Result<J>::success(std::move(out));
    } catch (const std::exception &e) {
        return Result<J>::failure(error(e.what()));
    }
}
Result<SctProjectStoryPaths> SctStoryPaths::analyze(const SctAuthoringState &state,
                                                    const SctStoryPathOptions &options) {
    SctProjectStoryPaths result{state.project.id, state.project.revision};
    std::vector<J> scripts, resourceLinks;
    for (const auto &script : state.project.scripts) {
        auto working = SctAuthoringMaterializer::workingState(state.project, state.programs, script.id);
        if (!working)
            return Result<SctProjectStoryPaths>::failure(working.diagnostics());
        const auto &b = *state.project.find(script.baseline);
        auto key = "script:" + std::to_string(script.id.value);
        J identity = {{"key", key},
                      {"dataset", b.datasetFingerprint.digest.toHex()},
                      {"path", b.source.locator.identityKey()},
                      {"sha256", b.source.revision.digest.toHex()},
                      {"project", state.project.id.value},
                      {"revision", std::to_string(state.project.revision.value)}};
        std::vector<SctConnectivityRoot> roots;
        for (const auto &module : state.project.modules)
            if (module.script == script.id) {
                SctMeaningTarget t;
                t.script = script.id;
                t.entity = module.id;
                roots.push_back({t, module.sections});
            }
        auto facts = sctConnectivityFacts(*working.value().document, identity, roots);
        for (const auto &section : working.value().document->sections) {
            auto t = SctInterpretation::sectionTarget(state.project, script.id, section.id);
            for (const auto &module : state.project.modules)
                if (module.script == script.id && !module.sections.empty() && module.sections.front() == section.id) {
                    t = {};
                    t.script = script.id;
                    t.entity = module.id;
                }
            result.targets[key + "#" + std::to_string(section.id.value())] = t;
        }
        for (const auto &entry : state.project.entrypoints)
            if (entry.script == script.id && entry.binding && entry.binding->provider) {
                auto *provider = std::get_if<smores::script::Entry>(&*entry.binding->provider);
                if (!provider)
                    continue;
                J resource = {{"identity",
                               {{"key", provider->resource.dataset + ":" + provider->resource.asset},
                                {"dataset", identity.at("dataset")},
                                {"originalDataset", provider->resource.dataset},
                                {"path", provider->resource.asset},
                                {"revision", provider->resource.revision},
                                {"document", provider->resource.document}}},
                              {"status", "analyzed"},
                              {"entries", J::array()}};
                J row = {{"index", std::to_string(provider->tableIndex)}, {"handler", nullptr}, {"signal", nullptr}};
                if (entry.binding->kind == SctEntrypointKind::SignalConsumer && entry.binding->signalBit)
                    row["signal"] = {
                        {"variable", "BitVar:" + std::to_string(*entry.binding->signalBit)},
                        {"evidence", "Explicit retained entrypoint binding; resource freshness advisory."}};
                else if (auto name = smores::script::handlerName(provider->tableId))
                    row["handler"] = *name;
                resource["entries"].push_back(row);
                resourceLinks.push_back(sctResourceLinks(resource, facts));
            }
        scripts.push_back(std::move(facts));
    }
    auto graph = propose(scripts, resourceLinks, options);
    if (!graph)
        return Result<SctProjectStoryPaths>::failure(graph.diagnostics());
    result.graph = std::move(graph.value());
    return Result<SctProjectStoryPaths>::success(std::move(result));
}
Result<SctAuthoringState> SctStoryPaths::adopt(const SctAuthoringState &state, const SctProjectStoryPaths &proposal,
                                               SctStorylineId story, const std::vector<std::string> &nodes) {
    if (nodes.empty() || state.project.id != proposal.project || state.project.revision != proposal.revision)
        return Result<SctAuthoringState>::failure(error("Path proposal is stale."));
    auto next = state;
    auto it = std::ranges::find(next.project.storylines, story, &SctStoryline::id);
    if (it == next.project.storylines.end())
        return Result<SctAuthoringState>::failure(error("Choose a storyline."));
    for (const auto &node : nodes) {
        auto found = proposal.targets.find(node);
        if (found == proposal.targets.end())
            return Result<SctAuthoringState>::failure(error("Path target is external or unresolved."));
        auto id = next.project.allocate<SctStoryStepId>();
        if (!id)
            return Result<SctAuthoringState>::failure(id.diagnostics());
        it->steps.push_back({id.value(), found->second, {}});
    }
    auto explanation = next.project.allocate<SctExplanationId>();
    if (!explanation)
        return Result<SctAuthoringState>::failure(explanation.diagnostics());
    SctExplanationRecord record;
    record.id = explanation.value();
    record.key = selectionKey(nodes);
    record.rule = "story-path";
    record.ruleVersion = "1";
    record.interpretation = "Adopted narrative order";
    record.reasoning = "Explicitly selected proposal nodes, in user-selected order.";
    record.qualification = "Narrative metadata; no executable routing change.";
    record.revision = state.project.revision;
    record.decision = SctInterpretationDecision::Accepted;
    for (const auto &node : nodes)
        record.subjects.push_back(proposal.targets.at(node));
    record.evidence = record.subjects;
    for(const auto& target:record.evidence) record.observations.push_back(SctInterpretation::observe(state,target));
    next.project.explanations.push_back(std::move(record));
    auto diagnostics=next.project.validate();if(hasErrors(diagnostics))return Result<SctAuthoringState>::failure(diagnostics);
    return Result<SctAuthoringState>::success(std::move(next));
}
Result<SctAuthoringState> SctStoryPaths::reject(const SctAuthoringState &state, const SctProjectStoryPaths &proposal,
                                                const std::vector<std::string> &nodes) {
    if (nodes.empty() || state.project.id != proposal.project || state.project.revision != proposal.revision)
        return Result<SctAuthoringState>::failure(error("Path proposal is stale."));
    auto next = state;
    auto id = next.project.allocate<SctExplanationId>();
    if (!id)
        return Result<SctAuthoringState>::failure(id.diagnostics());
    SctExplanationRecord record;
    record.id = id.value();
    record.key = selectionKey(nodes);
    record.rule = "story-path";
    record.ruleVersion = "1";
    record.interpretation = "Rejected narrative suggestion";
    record.reasoning = "Explicit user decision.";
    record.qualification = "Does not change source facts.";
    record.revision = state.project.revision;
    record.decision = SctInterpretationDecision::Rejected;
    for (const auto &node : nodes)
        if (proposal.targets.contains(node))
            record.subjects.push_back(proposal.targets.at(node));
    record.evidence = record.subjects;
    for(const auto& target:record.evidence) record.observations.push_back(SctInterpretation::observe(state,target));
    next.project.explanations.push_back(std::move(record));
    auto diagnostics=next.project.validate();if(hasErrors(diagnostics))return Result<SctAuthoringState>::failure(diagnostics);
    return Result<SctAuthoringState>::success(std::move(next));
}
} // namespace salsa::core
