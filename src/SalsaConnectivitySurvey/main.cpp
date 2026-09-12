#include "SalsaCore/Authoring/SctStoryPaths.h"
#include "SpiceSCT/SctTextContract.h"
#include "Survey.h"
#include "Worker.h"
#include "smores/script/Relationships.h"
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <thread>
using namespace salsa::survey;
namespace fs = std::filesystem;
namespace {
std::atomic_bool stopped = false;
BOOL WINAPI cancel(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT) {
        stopped = true;
        return TRUE;
    }
    return FALSE;
}
std::string utf8(const fs::path &p) {
    auto s = p.generic_u8string();
    return {s.begin(), s.end()};
}
fs::path path(const std::string &s) {
    return fs::path(std::u8string(s.begin(), s.end()));
}
struct Handle {
    HANDLE value = nullptr;
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    Handle(const Handle &) = delete;
};
fs::path executable() {
    std::wstring s(32768, L'\0');
    auto n = GetModuleFileNameW(nullptr, s.data(), static_cast<DWORD>(s.size()));
    if (!n || n == s.size())
        throw std::runtime_error("Cannot identify executable");
    s.resize(n);
    return s;
}
using Clock = std::chrono::steady_clock;
Json failure(const Json &identity, const std::string &status, const std::string &message) {
    return {{"identity", identity},
            {"status", status},
            {"diagnostics", Json::array({{{"stage", "survey"}, {"message", message}}})}};
}
Json workerProcess(const fs::path &exe, const fs::path &jobFile, const Json &job, unsigned timeout) {
    const auto child = runIsolated(exe, {L"--worker", jobFile.wstring()}, std::chrono::seconds(timeout), stopped);
    const auto &status = child.interruption;
    const auto code = child.exitCode;
    Json result;
    if (!status.empty())
        result =
            failure(job.at("identity"), status, status == "timeout" ? "Per-file time limit exceeded" : "Run cancelled");
    else if (code == 2)
        throw std::runtime_error("Worker failed to write its report");
    else if (code > 1 || !fs::exists(path(job.at("result").get<std::string>())))
        result = failure(job.at("identity"), "crashed",
                         "Worker exited without a complete result (code " + std::to_string(code) + ")");
    else {
        result = readJson(path(job.at("result").get<std::string>()));
        if (result.at("identity") != job.at("identity"))
            throw std::runtime_error("Worker identity mismatch");
    }
    result["wallMs"] = child.wallMs;
    result["peakWorkingSetBytes"] = child.peakWorkingSetBytes;
    return result;
}
struct Options {
    std::map<std::string, std::string> values;
    std::set<std::string> flags;
    Options(int argc, wchar_t **argv, int start) {
        for (int n = start; n < argc; ++n) {
            auto k = utf8(argv[n]);
            if (k == "--resume" || k == "--all" || k == "--json") {
                if (!flags.insert(k).second)
                    throw std::runtime_error("Duplicate option");
            } else {
                if (!k.starts_with("--") || n + 1 == argc || !values.emplace(k, utf8(argv[++n])).second)
                    throw std::runtime_error("Invalid option");
            }
        }
    }
    std::string need(const std::string &k) const {
        auto i = values.find(k);
        if (i == values.end())
            throw std::runtime_error("Missing " + k);
        return i->second;
    }
    unsigned number(const std::string &k, unsigned fallback) const {
        auto i = values.find(k);
        if (i == values.end())
            return fallback;
        if (!std::regex_match(i->second, std::regex("[0-9]+")))
            throw std::runtime_error("Invalid " + k);
        auto n = std::stoull(i->second);
        if (n > UINT_MAX)
            throw std::runtime_error("Option too large");
        return static_cast<unsigned>(n);
    }
    void check(std::initializer_list<std::string> allowed, std::initializer_list<std::string> switches) const {
        for (const auto &[k, v] : values)
            if (std::find(allowed.begin(), allowed.end(), k) == allowed.end())
                throw std::runtime_error("Unknown option " + k);
        for (const auto &k : flags)
            if (std::find(switches.begin(), switches.end(), k) == switches.end())
                throw std::runtime_error("Unknown flag " + k);
    }
};
bool inside(const fs::path &child, const fs::path &parent) {
    auto c = child.begin();
    for (auto p = parent.begin(); p != parent.end(); ++p, ++c)
        if (c == child.end() || _wcsicmp(c->c_str(), p->c_str()) != 0)
            return false;
    return true;
}
void verifyManifest(const Json &m, const char *format) {
    if (m.at("format") != format ||
        m.at("schemaVersion") !=
            (std::string_view(format) == "jahorta.salsa.connectivity-survey-input" ? 1 : SchemaVersion))
        throw std::runtime_error("Unsupported survey schema");
}
std::string factName(const Json &identity) {
    return hash(identity.dump()) + ".json.zlib";
}
Json aggregate(Json &run) {
    Json index = {
        {"format", "jahorta.salsa.connectivity-index"}, {"schemaVersion", SchemaVersion}, {"datasets", Json::object()}};
    Json summary = {
        {"discovered", 0ull},
        {"analyzed", 0ull},
        {"failed", 0ull},
        {"cached", 0ull},
        {"accesses", 0ull},
        {"scopes", 0ull},
        {"unresolved", 0ull},
        {"ungroupedWrites", 0ull},
        {"peakWorkerBytes", 0ull},
        {"groups", Json::object()},
        {"statuses", Json::object()},
        {"coverage", run.at("datasets")},
        {"limitations",
         Json::array({"Static potential connectivity; does not prove runtime reachability or story meaning.",
                      "Native opcode effects on global variables are not enumerated.",
                      "Named cutscene scopes are structural candidates and can share helper sections.",
                      "Dataset/disc graphs remain separate; absent files are inventory differences."})}};
    const auto add = [&](const char *k, std::uint64_t n) { summary[k] = summary[k].get<std::uint64_t>() + n; };
    for (const auto &f : run.at("files")) {
        if (f.at("identity").value("kind", "sct") != "sct")
            continue;
        add("discovered", 1);
        auto status = f.at("status").get<std::string>();
        summary["statuses"][status] = summary["statuses"].value(status, 0ull) + 1;
        summary["peakWorkerBytes"] =
            std::max(summary["peakWorkerBytes"].get<std::uint64_t>(), f.value("peakWorkingSetBytes", 0ull));
        if (status != "analyzed") {
            add("failed", 1);
            continue;
        }
        add("analyzed", 1);
        if (f.value("cached", false))
            add("cached", 1);
        for (auto k : {"accesses", "scopes", "unresolved", "ungroupedWrites"})
            add(k, f.at(k).get<std::uint64_t>());
        auto group = f.at("scriptGroup").get<std::string>();
        summary["groups"][group] = summary["groups"].value(group, 0ull) + 1;
        const auto &identity = f.at("identity");
        auto dataset = identity.at("dataset").get<std::string>();
        auto script = identity.at("key").get<std::string>();
        if (!index["datasets"].contains(dataset))
            index["datasets"][dataset] = {{"variables", Json::object()}, {"scripts", Json::array()}};
        auto &d = index["datasets"][dataset];
        d["scripts"].push_back(script);
        for (const auto &[variable, counts] : f.at("variables").items())
            d["variables"][variable].push_back({{"script", script}, {"counts", counts}});
    }
    summary["connectivity"] = Json::object();
    for (auto &[dataset, d] : index["datasets"].items()) {
        Json stats = Json::array();
        std::map<std::string, std::string> parents;
        for (const auto &s : d["scripts"])
            parents[s.get<std::string>()] = s.get<std::string>();
        const auto root = [&](std::string n) {
            while (parents.at(n) != n) {
                parents[n] = parents.at(parents.at(n));
                n = parents.at(n);
            }
            return n;
        };
        for (const auto &[variable, members] : d["variables"].items()) {
            std::uint64_t writes = 0, reads = 0, writerScripts = 0, readerScripts = 0;
            std::string first;
            for (const auto &member : members) {
                const auto &c = member.at("counts");
                auto w = c.at("set").get<std::uint64_t>() + c.at("clear").get<std::uint64_t>() +
                         c.at("toggle").get<std::uint64_t>() + c.at("assign").get<std::uint64_t>();
                auto r = c.at("read").get<std::uint64_t>();
                writes += w;
                reads += r;
                writerScripts += w > 0;
                readerScripts += r > 0;
                auto s = member.at("script").get<std::string>();
                if (first.empty())
                    first = s;
                else
                    parents[root(s)] = root(first);
            }
            stats.push_back({{"variable", variable},
                             {"writes", writes},
                             {"reads", reads},
                             {"writerScripts", writerScripts},
                             {"readerScripts", readerScripts},
                             {"scripts", members.size()},
                             {"potentialPairs", std::to_string(writes) + " * " + std::to_string(reads)}});
        }
        std::sort(stats.begin(), stats.end(), [](const Json &a, const Json &b) {
            return a.at("scripts") != b.at("scripts") ? a.at("scripts") > b.at("scripts")
                                                      : a.at("variable") < b.at("variable");
        });
        Json components = Json::object();
        for (const auto &[s, p] : parents)
            components[root(s)].push_back(s);
        Json noReaders = Json::array();
        for (const auto &v : stats)
            if (v.at("writes").get<std::uint64_t>() > 0 && v.at("reads") == 0)
                noReaders.push_back(v.at("variable"));
        d["variableStats"] = stats;
        d["components"] = components;
        summary["connectivity"][dataset] = {{"variables", stats.size()},
                                            {"components", components.size()},
                                            {"writesWithoutSctReaders", noReaders},
                                            {"mostConnected", Json::array()}};
        for (std::size_t n = 0; n < std::min<std::size_t>(20, stats.size()); ++n)
            summary["connectivity"][dataset]["mostConnected"].push_back(stats[n]);
    }
    Json copies = Json::object();
    for (const auto &f : run.at("files")) {
        const auto &id = f.at("identity");
        copies[id.at("path").get<std::string>()].push_back({{"script", id.at("key")}, {"sha256", id.at("sha256")}});
    }
    summary["repeatedPaths"] = Json::object();
    for (const auto &[p, v] : copies.items())
        if (v.size() > 1)
            summary["repeatedPaths"][p] = v;
    Json slow = Json::array();
    for (const auto &f : run.at("files"))
        slow.push_back({{"script", f.at("identity").at("key")},
                        {"wallMs", f.value("wallMs", 0.0)},
                        {"loadMs", f.value("loadMs", 0.0)},
                        {"extractMs", f.value("extractMs", 0.0)}});
    std::sort(slow.begin(), slow.end(), [](const Json &a, const Json &b) { return a.at("wallMs") > b.at("wallMs"); });
    summary["slowestScripts"] = Json::array();
    for (std::size_t n = 0; n < std::min<std::size_t>(20, slow.size()); ++n)
        summary["slowestScripts"].push_back(slow[n]);
    run["summary"] = std::move(summary);
    return index;
}
int scan(const Options &options) {
    options.check({"--manifest", "--output", "--jobs", "--timeout-seconds"}, {"--resume"});
    auto jobs = options.number("--jobs", 2), timeout = options.number("--timeout-seconds", 300);
    if (!jobs || jobs > 64 || !timeout)
        throw std::runtime_error("Jobs must be 1..64 and timeout positive");
    auto config = readJson(path(options.need("--manifest")));
    verifyManifest(config, "jahorta.salsa.connectivity-survey-input");
    if (!config.at("datasets").is_array() || config.at("datasets").empty())
        throw std::runtime_error("No datasets configured");
    auto output = fs::weakly_canonical(fs::absolute(path(options.need("--output"))));
    auto exe = executable();
    auto build = hash(readBytes(exe));
    std::set<std::string> datasetIds;
    std::vector<std::pair<Json, fs::path>> datasets;
    for (const auto &d : config.at("datasets")) {
        const auto id = d.at("id").get<std::string>();
        if (!std::regex_match(id, std::regex("[A-Za-z0-9_-]+")) || !datasetIds.insert(id).second)
            throw std::runtime_error("Invalid or duplicate dataset id");
        if (d.at("platform") != "dreamcast" && d.at("platform") != "gamecube")
            throw std::runtime_error("Invalid dataset platform");
        if (d.at("region") != "jp" && d.at("region") != "us" && d.at("region") != "eu")
            throw std::runtime_error("Invalid dataset region");
        if (d.contains("textConvention") && !d.at("textConvention").is_null()) {
            const auto selected = d.at("textConvention").get<std::string>();
            if (!std::ranges::any_of(spice::sct::kSctKnownTextConventions,
                                     [&](const auto &c) { return c.stableName == selected; }))
                throw std::runtime_error("Unknown text convention in dataset " + id);
        }
        d.at("release").get<std::string>();
        d.at("disc").get<std::string>();
        auto root = fs::canonical(path(d.at("root").get<std::string>()));
        if (!fs::is_directory(root) || inside(output, root))
            throw std::runtime_error("Output must be outside every input tree");
        datasets.emplace_back(d, root);
    }
    const bool resume = options.flags.contains("--resume");
    if (fs::exists(output) && !fs::is_empty(output)) {
        if (!resume)
            throw std::runtime_error("Output exists; choose an empty directory or --resume");
        verifyManifest(readJson(output / "survey.json"), "jahorta.salsa.connectivity-survey");
    }
    fs::create_directories(output / "facts");
    fs::create_directories(output / ".work");
    Handle lock(CreateFileW((output / ".survey.lock").c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_HIDDEN, nullptr));
    if (lock.value == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Another survey owns this output");
    const auto start = Clock::now();
    Json run = {{"format", "jahorta.salsa.connectivity-survey"},
                {"schemaVersion", SchemaVersion},
                {"analyzerVersion", AnalyzerVersion},
                {"buildIdentity", build},
                {"state", "inventory"},
                {"datasets", Json::array()},
                {"files", Json::array()}};
    std::vector<fs::path> sources;
    for (const auto &[d, root] : datasets) {
        std::vector<fs::path> files;
        for (const auto &entry : fs::recursive_directory_iterator(root)) {
            if (!entry.is_regular_file())
                continue;
            auto ext = entry.path().extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), towlower);
            if (ext == L".sct")
                files.push_back(entry.path());
        }
        if (files.empty())
            throw std::runtime_error("Configured dataset has no SCT files: " + d.at("id").get<std::string>());
        std::sort(files.begin(), files.end());
        Json coverage = d;
        coverage.erase("root");
        coverage["discovered"] = files.size();
        run["datasets"].push_back(coverage);
        std::set<std::string> locators;
        for (const auto &file : files) {
            auto relative = utf8(file.lexically_relative(root));
            std::transform(relative.begin(), relative.end(), relative.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (!locators.insert(relative).second)
                throw std::runtime_error("Case-colliding SCT locators");
            Json identity = {{"key", d.at("id").get<std::string>() + ":" + relative},
                             {"kind", "sct"},
                             {"dataset", d.at("id")},
                             {"release", d.at("release")},
                             {"disc", d.at("disc")},
                             {"path", relative},
                             {"platform", d.at("platform")},
                             {"region", d.at("region")},
                             {"sha256", nullptr},
                             {"textConvention", d.value("textConvention", Json(nullptr))},
                             {"analyzerVersion", AnalyzerVersion},
                             {"buildIdentity", build}};
            Json item;
            try {
                if (!inside(fs::canonical(file), root))
                    throw std::runtime_error("Source resolves outside dataset root");
                identity["sha256"] = hash(readBytes(file));
                item = {{"identity", identity}, {"status", "pending"}};
            } catch (const std::exception &e) {
                item = failure(identity, "inventory-failed", e.what());
            }
            run["files"].push_back(item);
            sources.push_back(file);
        }
        std::set<std::string> resourcePaths;
        if (d.contains("mlds")) {
            if (!d.at("mlds").is_array())
                throw std::runtime_error("mlds must be an array");
            for (auto association : d.at("mlds")) {
                const auto normalize = [&](const Json &j, const char *extension) {
                    auto asset = smores::script::assetKey(j.get<std::string>());
                    if (!asset.ends_with(extension) ||
                        !smores::script::valid({"dataset", asset, "revision", "document"}))
                        throw std::runtime_error("Invalid associated resource/script path");
                    return asset;
                };
                auto asset = normalize(association.at("path"), ".mld");
                if (!resourcePaths.insert(asset).second)
                    throw std::runtime_error("Duplicate MLD path; combine its script associations");
                auto role = association.value("role", std::string("unclassified"));
                if (role != "base" && role != "auxiliary" && role != "streamed-tile" && role != "unclassified")
                    throw std::runtime_error("Invalid MLD role");
                if (!association.at("scripts").is_array())
                    throw std::runtime_error("MLD scripts must be an array");
                std::set<std::string> scripts;
                for (const auto &script : association.at("scripts"))
                    scripts.insert(normalize(script, ".sct"));
                association = {{"path", asset}, {"role", role}, {"scripts", scripts}};
                Json identity = {{"key", d.at("id").get<std::string>() + ":" + asset},
                                 {"kind", "mld"},
                                 {"dataset", d.at("id")},
                                 {"release", d.at("release")},
                                 {"disc", d.at("disc")},
                                 {"path", asset},
                                 {"platform", d.at("platform")},
                                 {"region", d.at("region")},
                                 {"sha256", nullptr},
                                 {"textConvention", nullptr},
                                 {"analyzerVersion", AnalyzerVersion},
                                 {"buildIdentity", build},
                                 {"association", association}};
                auto file = root / path(asset);
                Json item;
                try {
                    if (!inside(fs::canonical(file), root))
                        throw std::runtime_error("MLD resolves outside dataset root");
                    identity["sha256"] = hash(readBytes(file));
                    item = {{"identity", identity}, {"status", "pending"}};
                } catch (const std::exception &e) {
                    item = failure(identity, "resource-unavailable", e.what());
                }
                run["files"].push_back(item);
                sources.push_back(file);
            }
        }
    }
    run["inventoryMs"] = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    run["state"] = "running";
    writeText(output / "survey.json", run.dump(2));
    struct Active {
        std::size_t index;
        std::future<Json> result;
    };
    std::vector<Active> active;
    // Declared after futures so fatal exits request cancellation before joining children.
    struct StopOnExit {
        ~StopOnExit() {
            stopped = true;
        }
    } stopOnExit;
    std::size_t next = 0, done = 0;
    while (next < sources.size() || !active.empty()) {
        while (!stopped && next < sources.size() && active.size() < jobs) {
            const auto n = next++;
            auto &item = run["files"][n];
            if (item["status"] != "pending") {
                ++done;
                continue;
            }
            const auto identity = item.at("identity");
            const auto name = factName(identity);
            const auto factsPath = output / "facts" / name;
            if (resume && fs::exists(factsPath)) {
                try {
                    auto facts = readFacts(factsPath);
                    if (facts.at("identity") == identity && facts.at("status") == "analyzed") {
                        item = summarize(facts);
                        item["cached"] = true;
                        item["facts"] = name;
                        ++done;
                        continue;
                    }
                } catch (const std::exception &) {
                }
            }
            auto jobFile = output / ".work" / (std::to_string(n) + ".job.json");
            auto resultFile = output / ".work" / (std::to_string(n) + ".result.json");
            if (fs::exists(resultFile))
                fs::remove(resultFile);
            Json job = {{"identity", identity},
                        {"source", utf8(sources[n])},
                        {"facts", utf8(factsPath)},
                        {"result", utf8(resultFile)}};
            writeText(jobFile, job.dump());
            active.push_back(
                {n, std::async(std::launch::async, [=] { return workerProcess(exe, jobFile, job, timeout); })});
        }
        bool progress = false;
        for (std::size_t n = 0; n < active.size();) {
            if (active[n].result.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
                ++n;
                continue;
            }
            auto result = active[n].result.get();
            auto &item = run["files"][active[n].index];
            item = std::move(result);
            if (item["status"] == "analyzed")
                item["facts"] = factName(item.at("identity"));
            ++done;
            progress = true;
            active.erase(active.begin() + n);
        }
        if (progress) {
            writeText(output / "survey.json", run.dump(2));
            std::cout << done << " / " << sources.size() << " completed\n" << std::flush;
        }
        if (stopped && active.empty())
            break;
        if (!progress)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    for (auto &f : run["files"])
        if (f["status"] == "pending")
            f = failure(f.at("identity"), "cancelled", "Not analyzed before cancellation");
    for (std::size_t n = 0; n < sources.size(); ++n) {
        for (const auto *suffix : {".job.json", ".result.json"})
            fs::remove(output / ".work" / (std::to_string(n) + suffix));
    }
    auto index = aggregate(run);
    indexResources(run, index, output);
    run["wallMs"] = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    const bool cancelled = stopped;
    run["state"] = cancelled ? "cancelled" : run["summary"]["failed"] == 0 ? "complete" : "partial";
    run["indexIdentity"] = hash(index.dump());
    writeFacts(output / "index.json.zlib", index);
    writeText(output / "survey.json", run.dump(2));
    writeText(output / "summary.json", run["summary"].dump(2));
    auto summary = run.at("summary");
    summary.erase("repeatedPaths");
    std::string report =
        "# SCT connectivity survey\n\nStatus: " + run.at("state").get<std::string>() +
        "\n\nStatic variable incidence, not proof of story meaning or runtime reachability. Graphs are separated by "
        "dataset/disc. Native opcode internal state effects are not enumerated.\n\n```json\n" +
        summary.dump(2) +
        "\n```\n\nQuery this directory with `query --survey <directory> --variable BitVar:1960`. Use `--script "
        "<dataset:relative-path>` to discover cutscene and entrypoint section IDs.\n";
    writeText(output / "summary.md", report);
    std::cout << "Survey " << run.at("state") << ": " << summary.at("analyzed") << " analyzed, " << summary.at("failed")
              << " incomplete, " << summary.at("cached") << " cached\n";
    return cancelled ? 130 : summary.at("failed") == 0 ? 0 : 1;
}
int query(const Options &options) {
    options.check({"--survey", "--variable", "--script", "--cutscene", "--entrypoint", "--resource", "--dataset",
                   "--offset", "--limit", "--paths", "--start", "--max-paths", "--max-steps"},
                  {"--json", "--all"});
    auto directory = path(options.need("--survey"));
    auto run = readJson(directory / "survey.json");
    verifyManifest(run, "jahorta.salsa.connectivity-survey");
    std::string kind, value;
    for (auto k : {"variable", "script", "cutscene", "entrypoint", "resource", "paths"})
        if (options.values.contains(std::string("--") + k)) {
            if (!kind.empty())
                throw std::runtime_error("Choose one query kind");
            kind = k;
            value = options.need(std::string("--") + k);
        }
    if (kind.empty())
        throw std::runtime_error("Missing query selector");
    if (kind != "variable" && options.values.contains("--dataset"))
        throw std::runtime_error(
            "Dataset filtering applies to variable queries; other keys already include the dataset");
    if (options.values.contains("--dataset") && !std::ranges::any_of(run.at("datasets"), [&](const auto &d) {
            return d.at("id") == options.need("--dataset");
        }))
        throw std::runtime_error("Unknown dataset");
    std::string script = value, local;
    if (kind == "cutscene" || kind == "entrypoint") {
        auto p = value.rfind('#');
        if (p == std::string::npos)
            throw std::runtime_error("Use <script-key>#<section-id>");
        script = value.substr(0, p);
        local = value.substr(p + 1);
    }
    auto index = readFacts(directory / "index.json.zlib");
    verifyManifest(index, "jahorta.salsa.connectivity-index");
    if (run.at("indexIdentity") != hash(index.dump()))
        throw std::runtime_error("Survey index does not match this run");
    if (kind == "paths") {
        if (!std::ranges::any_of(run.at("datasets"), [&](const auto &d) { return d.at("id") == value; }))
            throw std::runtime_error("Unknown path dataset.");
        std::vector<Json> scripts, resources;
        for (const auto &f : run.at("files"))
            if (f.at("identity").value("kind", "sct") == "sct" && f.at("identity").at("dataset") == value) {
                if (!f.contains("facts")) {
                    scripts.push_back(f);
                    continue;
                }
                auto name = f.at("facts").get<std::string>();
                if (name != factName(f.at("identity")))
                    throw std::runtime_error("Invalid fact filename.");
                auto facts = readFacts(directory / "facts" / name);
                if (facts.at("identity") != f.at("identity"))
                    throw std::runtime_error("Fact identity mismatch.");
                scripts.push_back(std::move(facts));
            }
        for (const auto &r : index.at("resources"))
            if (r.at("identity").at("dataset") == value)
                for (const auto &link : r.at("links"))
                    resources.push_back(link);
        salsa::core::SctStoryPathOptions pathOptions;
        if (options.values.contains("--start"))
            pathOptions.start = options.need("--start");
        pathOptions.maximumPaths = options.number("--max-paths", 64);
        pathOptions.maximumSteps = options.number("--max-steps", 128);
        auto result = salsa::core::SctStoryPaths::propose(scripts, resources, pathOptions);
        if (!result)
            throw std::runtime_error(result.diagnostics().front().message);
        std::cout << Json{{"format", "jahorta.salsa.connectivity-path-query"},
                          {"schemaVersion", 1},
                          {"dataset", value},
                          {"graph", result.value()}}
                         .dump(2)
                  << "\n";
        return 0;
    }
    if (kind == "resource") {
        if (!index.at("resources").contains(value))
            throw std::runtime_error("Unknown MLD resource key");
        std::cout << Json{{"format", "jahorta.salsa.connectivity-resource-query"},
                          {"schemaVersion", SchemaVersion},
                          {"resources", queryResources(run, index, directory, kind, value, script)}}
                         .dump(2)
                  << "\n";
        return 0;
    }
    std::set<std::string> relevant;
    if (kind == "variable") {
        if (!std::regex_match(value, std::regex("(BitVar|ByteVar|IntVar|FloatVar):[0-9]+")))
            throw std::runtime_error("Invalid variable key");
        for (const auto &[id, d] : index.at("datasets").items())
            if ((!options.values.contains("--dataset") || id == options.need("--dataset")) &&
                d.at("variables").contains(value))
                for (const auto &member : d.at("variables").at(value))
                    relevant.insert(member.at("script").get<std::string>());
    } else
        relevant.insert(script);
    const auto offset = options.number("--offset", 0), limit = options.number("--limit", 100);
    if (!limit)
        throw std::runtime_error("Limit must be positive");
    std::uint64_t total = 0;
    Json results = Json::array();
    Json incidence = Json::object();
    bool found = false;
    for (const auto &f : run.at("files")) {
        auto key = f.at("identity").at("key").get<std::string>();
        if (!relevant.contains(key))
            continue;
        found = true;
        if (!f.contains("facts")) {
            results.push_back(f);
            continue;
        }
        const auto name = f.at("facts").get<std::string>();
        if (name != factName(f.at("identity")))
            throw std::runtime_error("Invalid fact filename");
        auto facts = readFacts(directory / "facts" / name);
        if (facts.at("identity") != f.at("identity"))
            throw std::runtime_error("Fact identity mismatch");
        auto result = select(facts, kind, kind == "variable" ? value : local);
        const auto dataset = f.at("identity").at("dataset").get<std::string>();
        for (const auto &row : result["records"])
            if (row.contains("variable")) {
                const auto variable = row.at("variable").get<std::string>();
                incidence[dataset][variable] = index.at("datasets").at(dataset).at("variables").at(variable);
            }
        Json records = Json::array();
        for (auto &record : result["records"]) {
            if (total >= offset && (options.flags.contains("--all") || total - offset < limit))
                records.push_back(std::move(record));
            ++total;
        }
        result["matchingRecords"] = result["records"].size();
        result["records"] = std::move(records);
        if (kind == "variable") {
            std::set<std::string> visible;
            for (const auto &record : result["records"])
                visible.insert(record.at("section").get<std::string>());
            Json sections = Json::array();
            for (const auto &section : result["sections"])
                if (visible.contains(section.at("id").get<std::string>()))
                    sections.push_back(section);
            result["sections"] = std::move(sections);
        }
        results.push_back(std::move(result));
    }
    auto resourceLinks = queryResources(run, index, directory, kind, value, script);
    if (options.values.contains("--dataset")) {
        Json filtered = Json::array();
        for (auto &r : resourceLinks)
            if (r.at("identity").at("dataset") == options.need("--dataset"))
                filtered.push_back(std::move(r));
        resourceLinks = std::move(filtered);
    }
    if (!found && kind != "variable")
        throw std::runtime_error("Unknown script key");
    Json answer = {{"format", "jahorta.salsa.connectivity-query"},
                   {"schemaVersion", SchemaVersion},
                   {"kind", kind},
                   {"key", value},
                   {"surveyState", run.at("state")},
                   {"totalRecords", total},
                   {"offset", offset},
                   {"truncated", !options.flags.contains("--all") && total > std::uint64_t(offset) + limit},
                   {"results", results},
                   {"incidence", incidence},
                   {"resourceLinks", resourceLinks}};
    if (!options.flags.contains("--json"))
        std::cout << "Static connectivity (datasets remain separate). " << total << " records; offset " << offset
                  << ". Use --all or --offset/--limit for additional records.\n";
    std::cout << answer.dump(2) << "\n";
    return 0;
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(cancel, TRUE);
    try {
        if (argc == 3 && std::wstring(argv[1]) == L"--worker") {
            auto job = readJson(argv[2]);
            Json facts;
            try {
                facts = extract(readBytes(path(job.at("source").get<std::string>())), job.at("identity"));
            } catch (const std::exception &e) {
                facts = failure(job.at("identity"), "failed", e.what());
            }
            writeFacts(path(job.at("facts").get<std::string>()), facts);
            writeText(path(job.at("result").get<std::string>()), summarize(facts).dump());
            return facts.at("status") == "analyzed" ? 0 : 1;
        }
        if (argc < 2 || std::wstring(argv[1]) == L"--help") {
            std::cout << "SalsaConnectivitySurvey scan --manifest <file> --output <directory> [--resume] [--jobs 2] "
                         "[--timeout-seconds 300]\nSalsaConnectivitySurvey query --survey <directory> (--variable "
                         "BitVar:N | --script dataset:path | --cutscene dataset:path#section | --entrypoint "
                         "dataset:path#section | --resource dataset:asset.mld | --paths dataset) [--json] [--all | "
                         "--offset N --limit N]\n";
            return 0;
        }
        Options options(argc, argv, 2);
        if (std::wstring(argv[1]) == L"scan")
            return scan(options);
        if (std::wstring(argv[1]) == L"query")
            return query(options);
        throw std::runtime_error("Unknown command");
    } catch (const std::exception &e) {
        std::cerr << "Survey error: " << e.what() << "\n";
        return 2;
    }
}
