#include "SalsaCore/Editor/ProgramBackend.h"
#include "SalsaCore/Program/ProgramMaterializer.h"
#include "SalsaCore/Program/ProgramValues.h"
#include "SalsaCore/Sct/SctExpressionLanguage.h"
#include <fstream>
namespace salsa::core::editor {
namespace ir = program;
Result<StoryCatalog> ProgramBackend::storyCatalog(Revision r, std::stop_token s) {
    return service_->storyCatalog(r, s).get();
}
Result<StoryView> ProgramBackend::story(Revision r, StoryRequest q, std::stop_token s) {
    return service_->story(r, std::move(q), s).get();
}
Result<InteractionView> ProgramBackend::interaction(Revision r, SctScriptId id, std::stop_token s) {
    return service_->storyInteraction(r, id, s).get();
}
Result<ItemExplanation> ProgramBackend::explanation(Revision r, Subject id, std::stop_token s) {
    return service_->storyExplanation(r, id, s).get();
}
Result<OutputReview> ProgramBackend::reviewOutput(Revision r, std::vector<SctScriptId> ids,
                                                  std::stop_token s) {
    return service_->reviewOutput(r, std::move(ids), s).get();
}
Result<RouteDetail> ProgramBackend::routes(Revision r, SctModuleId id, std::stop_token stop) {
    auto q = service_->cutscene(r, id, stop).get();
    if (!q)
        return Result<RouteDetail>::failure(q.diagnostics());
    RouteDetail out{r, id};
    auto &s = *q.value().summary;
    auto expression = [&](ir::ValueId v) {
        if (!v.valid())
            return std::string{};
        auto e = ir::ProgramValues::expression(s.payload, v);
        return e ? SctExpressionLanguage::project(e.value()).text : std::string("unavailable");
    };
    auto append = [&](const auto &routes, const char *direction) {
        for (const auto &route : routes) {
            std::string label = std::string(direction) + " " + std::to_string(route.source.value) + " -> " +
                                std::to_string(route.target.value);
            label += route.inferred ? " (inferred)" : " (authored)";
            if (route.unresolved)
                label += " unresolved";
            if (route.changed)
                label += " changed since binding";
            const char *methods[]={"unknown entry method","internal call","arrival","battle return","handler","signal","named entry"};
            const char *outcomes[]={"observed transfer","local transfer","return to caller","resume loop","script transition","scheduled call"};
            label += std::string("; ")+methods[static_cast<unsigned>(route.method)]+"; "+outcomes[static_cast<unsigned>(route.outcome)];
            for (auto value : route.requirementValues)
                label += "; guard: " + expression(value);
            if (route.schedule.valid())
                label += "; schedule: " + expression(route.schedule);
            for (const auto &path : route.paths)
                label += "; preparation " + std::to_string(path.preparation.size()) + ", consumption " +
                         std::to_string(path.consumption.size());
            out.rows.push_back({route.source, std::move(label)});
            out.evidence.insert(out.evidence.end(), route.evidence.begin(), route.evidence.end());
        }
    };
    append(s.incoming, "Entry");
    append(s.exits, "Exit");
    for (const auto &effect : s.effects) {
        const char *banks[]={"BitVar","ByteVar","IntVar","FloatVar"};
        const char *access[]={"read","set to 1","clear to 0","assign","modify","computed write"};
        auto variable=effect.variable?std::string(banks[static_cast<unsigned>(effect.variable->bank)])+"["+std::to_string(effect.variable->index)+"]":"computed destination";
        out.rows.push_back({effect.site.action,variable+" — "+access[static_cast<unsigned>(effect.access)]+": "+expression(effect.expression)});
    }
    for (const auto &d : s.diagnostics)
        out.evidence.push_back({SctEvidenceKind::Inference, d.message});
    return Result<RouteDetail>::success(std::move(out));
}
Result<OutputResult> ProgramBackend::publishOutput(OutputRequest request, std::stop_token stop,
                                                   const SctLoadingObserver &progress) {
    std::filesystem::path stage;
    try {
        auto take = []<class T>(Result<T> value) -> T {
            if (!value)
                throw std::runtime_error(value.diagnostics().empty() ? "Output failed"
                                                                     : value.diagnostics().front().message);
            return std::move(value).takeValue();
        };
        auto checkStop = [&] {
            if (stop.stop_requested())
                throw std::runtime_error("Output cancelled");
        };
        checkStop();
        auto review = take(reviewOutput(request.review.revision, request.review.roots, stop));
        if (review.items.size() != request.review.items.size())
            throw std::runtime_error("Output review changed");
        for (std::size_t i = 0; i < review.items.size(); ++i)
            if (review.items[i].script != request.review.items[i].script ||
                review.items[i].filename != request.review.items[i].filename)
                throw std::runtime_error("Output review changed");
        auto destination = std::filesystem::absolute(request.directory).lexically_normal();
        if (destination.filename().empty() || std::filesystem::exists(destination) ||
            !std::filesystem::is_directory(destination.parent_path()))
            throw std::runtime_error("Choose a new output directory inside an existing parent");
        auto cp = take(service_->checkpoint(review.revision, stop).get());
        stage = destination.parent_path() / (".salsa-output-" + take(generateSctAuthoringUuid()));
        if (!std::filesystem::create_directory(stage))
            throw std::runtime_error("Cannot create output staging directory");
        ir::ProgramMaterializer materializer;
        OutputResult out{review.revision};
        for (std::size_t i = 0; i < review.items.size(); ++i) {
            checkStop();
            auto &item = review.items[i];
            if (progress)
                progress({"Encoding output scripts", i, review.items.size(), item.filename});
            auto encoded = take(materializer.exportScript(
                cp, review.revision, item.script, {take(generateSctAuthoringUuid())}, request.options, stop));
            auto path = std::filesystem::path(item.filename);
            if (path != path.filename() || path == "." || path == "..")
                throw std::runtime_error("Invalid output filename");
            std::ofstream file(stage / path, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char *>(encoded.bytes.data()), encoded.bytes.size());
            file.close();
            if (!file)
                throw std::runtime_error("Cannot write output file");
            out.files.push_back(destination / path);
        }
        checkStop();
        if (take(service_->status().get()).current != review.revision)
            throw std::runtime_error("Authoring changed while preparing output");
        if (progress)
            progress({"Publishing output directory", review.items.size(), review.items.size(), {}, false});
        if(std::filesystem::exists(destination))throw std::runtime_error("Output directory was created while export was prepared");
        std::filesystem::rename(stage, destination);
        stage.clear();
        return Result<OutputResult>::success(std::move(out));
    } catch (const std::exception &e) {
        if (!stage.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(stage, ec);
        }
        return Result<OutputResult>::failure(
            {{DiagnosticSeverity::Error, DiagnosticCode::SctExportFailed, e.what()}});
    }
}
} // namespace salsa::core::editor
