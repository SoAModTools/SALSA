#include "SalsaCore/Program/LegacyProgramImport.h"
#include "ProgramInternal.h"
#include "../Legacy/LegacyProgramBridge.h"
#include "SpiceSCT/SctDocumentExporter.h"
#include <set>
namespace salsa::core::program {
Result<LegacyProgramResult> LegacyProgramImport::importCapsule(
    SctAuthoringProjectId project, const std::filesystem::path &root, const DatasetFingerprint &dataset,
    std::span<const LegacyProgramSelection> selected, std::stop_token stop, const SctLoadingObserver &observer) {
    std::vector<Diagnostic> diagnostics;
    try {
        auto progress = [&](std::string phase, std::uint64_t n = 0, std::uint64_t total = 0) {
            require(!stop.stop_requested(), "Legacy program import cancelled");
            if (observer)
                observer({std::move(phase), n, total, {}});
        };
        progress("Reading legacy capsule");
        auto capsule = LegacyCapsuleReader::validate(root);
        if (!capsule)
            return Result<LegacyProgramResult>::failure(capsule.diagnostics());
        require(!selected.empty(), "Select at least one accepted legacy script");
        auto projectMetadata = legacy_detail::projectMetadata(root);
        std::vector<std::string> pendingProject;
        if (!projectMetadata)
            for (const auto &diagnostic : projectMetadata.diagnostics())
                pendingProject.push_back(diagnostic.message);
        ProgramBuilder builder(project);
        LegacyProgramResult result;
        std::set<std::uint32_t> ordinals;
        std::set<AssetLocator> locators;
        if (projectMetadata)
            result.project.presentation.opcodeColors = std::move(projectMetadata.value().opcodeColors);
        auto aliases = [&](SctScriptId script, const auto &rows) {
            for (auto &row : rows) {
                auto id = builder.allocate<SctMeaningId>();
                require(bool(id), "Cannot allocate legacy alias identity");
                builder.metadata().meanings.push_back(
                    {id.value(),
                     script,
                     row.variable,
                     {},
                     row.alias,
                     {{SctEvidenceKind::UserAssertion,
                       "Imported v7 variable alias; capsule " + capsule.value().capsuleId}}});
            }
        };
        if (projectMetadata)
            aliases({}, projectMetadata.value().projectAliases);
        for (const auto &item : selected) {
            progress("Translating legacy script", result.baselines.size(), selected.size());
            require(ordinals.insert(item.ordinal).second && locators.insert(item.locator).second,
                    "Duplicate selected legacy script or asset");
            auto summary = std::ranges::find(capsule.value().scripts, item.ordinal, &LegacyScriptSummary::ordinal);
            require(summary != capsule.value().scripts.end(), "Selected script is absent from capsule");
            auto script = legacy_detail::translate(root, *summary, item.publication);
            if (!script)
                return Result<LegacyProgramResult>::failure(script.diagnostics());
            const auto &options = item.publication;
            auto native = spice::sct::SctDocumentExporter::exportDocument(
                script.value().document, spice::sct::SctDocumentExportOptions{options.platform, options.textEncoding,
                                                                              options.byteOrder, options.wrapper});
            if (!native.success) {
                std::string error = "Cannot retain imported legacy baseline";
                for (auto &d : native.diagnostics)
                    error += "; " + d.message;
                throw std::runtime_error(error);
            }
            auto bytes = std::as_bytes(std::span(native.bytes));
            auto digest = sha256(bytes);
            require(bool(digest), "Cannot hash legacy baseline");
            auto documentId = generateSctAuthoringUuid();
            require(bool(documentId), "Cannot allocate imported document identity");
            AssetDescriptor source{item.locator, bytes.size(), {digest.value()}};
            auto platform = options.platform == spice::sct::SctPlatform::Dreamcast ? GamePlatform::Dreamcast
                                                                                   : GamePlatform::GameCube;
            SctAuthoringBaseline baseline{
                {},
                dataset,
                source,
                {documentId.value()},
                spice::sct::findSctKnownTextConvention(options.textEncoding),
                {{SctEvidenceKind::SourceObservation, "Converted official v7 capsule " + capsule.value().capsuleId}},
                {platform, true}};
            SctScriptContext context;
            context.name = item.locator.path().stem().string();
            context.platform = platform;
            context.region = item.region;
            context.legacyOrigin = SctLegacyOrigin{capsule.value().capsuleId, summary->key, summary->ordinal};
            context.outputProfile = SctAuthoredOutputProfile{
                options.byteOrder == spice::sct::SctDocumentOutputByteOrder::BigEndian,
                options.wrapper == spice::sct::SctDocumentOutputWrapper::Aklz, spice::sct::kCanonicalSctHeaderValues};
            auto id = builder.appendImported(script.value().document, std::move(baseline), std::move(context));
            if (!id)
                return Result<LegacyProgramResult>::failure(id.diagnostics());
            aliases(id.value(), script.value().metadata.aliases);
            std::map<std::uint64_t, SectionId> sections;
            for (auto &section : builder.data_->sections)
                if (section.script == id.value())
                    sections[section.sourceSection] = section.id;
            auto offset = count32(result.project.presentation.folders.size());
            for (auto &folder : script.value().metadata.folders) {
                PresentationFolder out{id.value(), folder.name};
                for (auto section : folder.sections) {
                    auto found = sections.find(section.value());
                    require(found != sections.end(), "Legacy folder section has no program mapping");
                    out.sections.push_back(found->second);
                }
                if (folder.parent)
                    out.parent = offset + count32(*folder.parent);
                result.project.presentation.folders.push_back(std::move(out));
            }
            builder.metadata().annotations.push_back(
                {id.value(),
                 {},
                 {{SctEvidenceKind::SourceObservation,
                   "Legacy folders retained as presentation; remaining unsupported metadata stays in capsule " +
                       capsule.value().capsuleId}}});
            auto pending = pendingProject;
            pending.insert(pending.end(), script.value().metadata.pending.begin(),
                           script.value().metadata.pending.end());
            for (auto &message : pending) {
                auto description =
                    "Legacy metadata remains pending in capsule " + capsule.value().capsuleId + ": " + message;
                builder.metadata().annotations.push_back(
                    {id.value(), {}, {{SctEvidenceKind::SourceObservation, description}}});
                diagnostics.push_back({DiagnosticSeverity::Warning, DiagnosticCode::UnresolvedSctAuthoringBinding,
                                       std::move(description)});
            }
            result.baselines.push_back({std::move(source), {bytes.begin(), bytes.end()}});
        }
        progress("Finishing imported program");
        auto built = builder.finish();
        if (!built)
            return Result<LegacyProgramResult>::failure(built.diagnostics());
        diagnostics.insert(diagnostics.end(), built.diagnostics().begin(), built.diagnostics().end());
        result.project.program = std::move(built).takeValue();
        progress("Legacy program ready", selected.size(), selected.size());
        return Result<LegacyProgramResult>::success(std::move(result), std::move(diagnostics));
    } catch (const std::exception &e) {
        diagnostics.push_back(failure(e.what()));
        return Result<LegacyProgramResult>::failure(diagnostics);
    }
}
} // namespace salsa::core::program
