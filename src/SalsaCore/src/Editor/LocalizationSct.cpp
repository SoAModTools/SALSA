#include "SalsaCore/Authoring/SctTextAuthoring.h"
#include "SalsaCore/Editor/Localization.h"
#include "SpiceSCT/SctDocumentImporter.h"
#include "SpiceSCT/SctParser.h"
#include "SpiceSCT/SctTextEvidence.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <stdexcept>
namespace salsa::core::editor {
Result<IndexedTranslationReview> LocalizationSct::read(const std::vector<std::filesystem::path> &paths,
                                                       const LocalizationCatalog &catalog,
                                                       std::stop_token stop, SctLoadingObserver progress) {
    namespace sct = spice::sct;
    try {
        auto check = [&] {
            if (stop.stop_requested())
                throw std::runtime_error("Translation import cancelled");
        };
        auto stem = [](std::string name) {
            name = std::filesystem::path(name).stem().string();
            std::transform(name.begin(), name.end(), name.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return name;
        };
        IndexedTranslationReview out;
        for (std::size_t n = 0; n < paths.size(); ++n) {
            check();
            auto path = paths[n].string();
            if (progress)
                progress({"Reading indexed translations", n, paths.size(), path});
            auto parsed = sct::SctParser{}.parseFile(path);
            check();
            auto assessment = sct::SctSourceTextDetector::assess(parsed);
            if (!assessment.sourceUsable)
                throw std::runtime_error("Unable to inspect translation source: " + path);
            auto convention = assessment.recommendation.convention;
            if (!convention && !assessment.indexedSctStrings.viableConventions.empty())
                convention = assessment.indexedSctStrings.viableConventions.front();
            if (!convention)
                throw std::runtime_error("No indexed text convention available for " + path);
            sct::SctDocumentImportOptions options;
            options.sourceTextEncoding = sct::sctTextEncodingFor(*convention);
            auto imported = sct::SctDocumentImporter::import(parsed, options);
            check();
            if (!imported.document)
                throw std::runtime_error("Unable to decode indexed translations: " + path);
            auto names = SctTextAuthoring::observe(*imported.document);
            SctScriptId script;
            for (const auto &m : catalog.messages)
                if (stem(m.scriptName) == stem(path)) {
                    if (script.valid() && script != m.script)
                        throw std::runtime_error("Ambiguous destination script for " + path);
                    script = m.script;
                }
            if (!script.valid()) {
                out.issues.push_back(paths[n].filename().string() +
                                     ": no matching imported script with discovered logical messages");
                continue;
            }
            for (const auto &section : imported.document->sections) {
                auto *string = std::get_if<sct::SctStringSectionContent>(&section.content);
                if (!string)
                    continue;
                if (std::holds_alternative<sct::SctOpaqueText>(string->string.value)) {
                    out.issues.push_back(section.nameBytes + ": undecoded indexed text");
                    continue;
                }
                auto observed = std::ranges::find_if(names, [&](auto &o) {
                    auto *id = std::get_if<sct::SctStringId>(&o.target);
                    return id && *id == string->string.id;
                });
                auto key = observed == names.end()               ? section.nameBytes
                           : observed->correspondenceKey.empty() ? observed->group + "/" + observed->name
                                                                 : observed->correspondenceKey;
                SctLogicalTextId target;
                bool ambiguous = false;
                for (const auto &m : catalog.messages)
                    if (m.script == script &&
                        (m.name == key ||
                         std::ranges::find(m.correspondenceKeys, key) != m.correspondenceKeys.end())) {
                        if (target.valid()) {
                            ambiguous = true;
                            break;
                        }
                        target = m.id;
                    }
                if (ambiguous) {
                    out.issues.push_back(section.nameBytes + ": ambiguous logical message match");
                    continue;
                }
                out.rows.push_back({target, script, paths[n].filename().string() + ":" + section.nameBytes,
                                    key, observed == names.end() ? "" : observed->languageKey.value_or(""),
                                    string->string.value});
            }
        }
        if (progress)
            progress({"Indexed translations ready for review", paths.size(), paths.size(), {}});
        return Result<IndexedTranslationReview>::success(std::move(out));
    } catch (const std::exception &e) {
        return Result<IndexedTranslationReview>::failure(
            Diagnostic{DiagnosticSeverity::Error, DiagnosticCode::InvalidSctAuthoringProject, e.what()});
    }
}
} // namespace salsa::core::editor
