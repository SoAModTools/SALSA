#pragma once
#include "SalsaCore/Authoring/SctEntrypointAuthoring.h"
#include "SalsaCore/Authoring/SctRouteAuthoring.h"
#include "SpiceSCT/SctDocumentExporter.h"
#include <stdexcept>
namespace salsa::tests::route {
using namespace salsa::core;
namespace sct = spice::sct;
inline sct::SctCanonicalExpression expr(std::string text) {
    auto r = SctExpressionLanguage::parse(text);
    if (!r.succeeded())
        throw std::runtime_error(text);
    return *r.expression;
}
template <class T> T take(Result<T> r) {
    if (!r) {
        std::string text;
        for (const auto &d : r.diagnostics())
            text += d.message + "\n";
        throw std::runtime_error(text);
    }
    return std::move(r).takeValue();
}
struct Fixture {
    SctAuthoringState state;
    SctRouteRequest request;
    SctPortId alternative;
};
inline Fixture organize(SctAuthoringState state, bool switchCase = false) {
    Fixture f{std::move(state)};
    for (const auto &script : std::vector(f.state.project.scripts)) {
        auto working = take(SctAuthoringMaterializer::workingState(f.state.project, f.state.programs, script.id));
        for (const auto &section : working.document->sections) {
            if (section.nameBytes == "load" || section.nameBytes == "response" || section.nameBytes == "alternative") {
                f.state = take(SctModuleAuthoring::extract(f.state, script.id, {section.id}, section.nameBytes));
                auto port =
                    f.state.project.ports[f.state.project.ports.size() - (section.nameBytes == "load" ? 2 : 1)].id;
                if (script.id == f.state.project.scripts[0].id && section.nameBytes == "load")
                    f.request.source = port;
                if (script.id == f.state.project.scripts[1].id && section.nameBytes == "response")
                    f.request.response = port;
                if (script.id == f.state.project.scripts[1].id && section.nameBytes == "alternative")
                    f.alternative = port;
            }
            if (section.nameBytes == "loop" && !std::ranges::any_of(f.state.project.sequences, [&](const auto &s) {
                    return s.script == script.id && s.section == section.id;
                }))
                f.state = take(SctSequenceAuthoring::promoteSequence(f.state, script.id, section.id, "loop"));
        }
    }
    auto destination = f.state.project.scripts[1].id;
    auto seq = std::ranges::find_if(f.state.project.sequences,
                                    [&](const auto &s) { return s.script == destination && s.name == "loop"; });
    SctEntrypointBinding b;
    b.kind = SctEntrypointKind::Arrival;
    b.sequence = seq->id;
    b.predicate = sct::SctParameterSite{seq->actions[0].instruction, {0, {}}};
    b.responseSite = sct::SctParameterSite{seq->actions[1].instruction, {0, {}}};
    b.response = f.request.response;
    f.state = take(SctEntrypointAuthoring::bind(f.state, destination, "Arrival", b, {}));
    f.request.arrival = f.state.project.entrypoints.back().id;
    if (switchCase)
        f.request.arrivalCase = 0;
    return f;
}

inline Fixture fixture(bool switchCase = false) {
    Fixture f{{take(SctAuthoringProject::create())}};
    for (unsigned n = 0; n < 2; ++n) {
        sct::SctDocument d;
        const auto guard = d.allocateInstructionId(), call = d.allocateInstructionId(), ret = d.allocateInstructionId(),
                   response = d.allocateInstructionId(), other = d.allocateInstructionId();
        d.sections.push_back(
            {d.allocateSectionId(), "init", sct::SctScriptSectionContent{{{d.allocateInstructionId(), 12}}}});
        sct::SctDocumentInstruction condition{
            guard, 0, false, {}, {{0, expr("Low16IntVar[15] == 990")}, {1, sct::SctInstructionReference{ret}}}};
        if (switchCase) {
            condition = {guard, 3, true, {}, {{0, expr("Low16IntVar[15]")}}};
            condition.repeatedParameterGroups = {
                {{{2, sct::SctEncodedWordValue{990}}, {3, sct::SctInstructionReference{call}}}},
                {{{2, sct::SctEncodedWordValue{UINT32_MAX}}, {3, sct::SctInstructionReference{ret}}}}};
        }
        d.sections.push_back(
            {d.allocateSectionId(), "loop",
             sct::SctScriptSectionContent{
                 {condition, {call, 11, false, {}, {{0, sct::SctInstructionReference{response}}}}, {ret, 12}}}});
        d.sections.push_back({d.allocateSectionId(), "response", sct::SctScriptSectionContent{{{response, 12}}}});
        d.sections.push_back({d.allocateSectionId(), "alternative", sct::SctScriptSectionContent{{{other, 12}}}});
        auto text = d.allocateSupplementaryTextId();
        d.supplementaryText.push_back({text, sct::SctTextKind::PlainString, sct::SctPlainText{"me999a.sct"}});
        d.sections.push_back(
            {d.allocateSectionId(), "load",
             sct::SctScriptSectionContent{
                 {{d.allocateInstructionId(), 43, true, expr("4"), {{0, sct::SctSupplementaryTextReference{text}}}},
                  {d.allocateInstructionId(), 12}}}});
        auto output = sct::SctDocumentExporter::exportDocument(
            d, sct::SctDocumentExportOptions{sct::SctPlatform::Dreamcast,
                                             sct::kSctShiftJisByte7FEncoding,
                                             sct::SctDocumentOutputByteOrder::LittleEndian,
                                             sct::SctDocumentOutputWrapper::Raw,
                                             sct::SctOpaquePreservationPolicy::RequirePreservation,
                                             {sct::SctHeaderExportMode::ExplicitValues, {2001, 1, 2, 7}}});
        if (!output.success)
            throw std::runtime_error("fixture export");
        auto raw = std::as_bytes(std::span(output.bytes));
        std::vector<std::byte> bytes(raw.begin(), raw.end());
        auto hash = take(sha256(bytes));
        SctAuthoringImportRequest request{
            {{take(AssetLocator::fromRelativePath(n ? "me002b.sct" : "me002a.sct")), bytes.size(), {hash}}, bytes},
            {GamePlatform::Dreamcast, GameRegion::Europe, {hash}},
            {GamePlatform::Dreamcast, true},
            sct::SctKnownTextConvention::ShiftJisByte7F,
            n ? "Second" : "First"};
        auto imported = take(SctAuthoringImporter::import(f.state.project, f.state.project.revision, request));
        f.state.project = imported.project;
        f.state.programs.push_back(imported.program);
    }
    return organize(std::move(f.state), switchCase);
}
} // namespace salsa::tests::route
