#include "SalsaCore/Authoring/SctInterpretation.h"
#include "SctEntrypointEditorDialog.h"
#include "SctEntrypointTools.h"
#include "SalsaCore/Authoring/SctEntrypointAuthoring.h"
#include "SpiceSCT/SctDocumentIndex.h"
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTextEdit>
#include <QVBoxLayout>
namespace salsa::qt {
using namespace core;
namespace sct = spice::sct;
namespace rel = smores::script;
namespace {
struct Candidate {
    std::string name;
    SctEntrypointBinding binding;
    sct::SctSectionId section;
    bool newHandler = false;
    std::optional<SctEntrypointId> existing;
};
QString errors(const std::vector<Diagnostic>& diagnostics) {
    QString result; for (const auto& d : diagnostics) result += QString::fromStdString(d.message) + '\n'; return result;
}
}
void showSctEntrypointEditor(SctDocumentController& controller, const AssetLocator& locator,
    const GameProjectContext& context, QWidget* parent) {
    QDialog dialog(parent); dialog.setWindowTitle(QObject::tr("Entrypoints and Resources")); dialog.resize(1040, 760);
    auto* layout = new QVBoxLayout(&dialog);
    auto* help = new QLabel(QObject::tr("init and loop are required and always enabled. MLDs are optional editing context. Available tile entries and handler matches do not establish runtime invocation or residency."), &dialog); help->setWordWrap(true); layout->addWidget(help);
    auto* resourceRow = new QHBoxLayout; layout->addLayout(resourceRow);
    auto* resources = new QComboBox(&dialog); resources->setMinimumContentsLength(35); resourceRow->addWidget(resources, 1);
    std::vector<AssetLocator> assets;
    for (const auto& asset : context.assets().snapshot().assets) if (rel::assetKey(asset.locator.path().extension().string()) == ".mld") {
        assets.push_back(asset.locator); resources->addItem(QString::fromStdString(asset.locator.path().generic_string()));
    }
    auto* role = new QComboBox(&dialog); role->addItems({QObject::tr("Base"), QObject::tr("Auxiliary"), QObject::tr("Streamed tile")}); resourceRow->addWidget(role);
    auto* inspect = new QPushButton(QObject::tr("Inspect / refresh MLD"), &dialog); resourceRow->addWidget(inspect);
    auto* list = new QListWidget(&dialog); layout->addWidget(list, 1);
    auto* details = new QTextEdit(&dialog); details->setReadOnly(true); layout->addWidget(details, 1);
    auto* bindButton = new QPushButton(QObject::tr("Connect response to module…"), &dialog); layout->addWidget(bindButton);
    auto* declareButton = new QPushButton(QObject::tr("Declare named entrypoint…"), &dialog); layout->addWidget(declareButton);
    auto* boundaryButton = new QPushButton(QObject::tr("Activation boundary…"), &dialog); layout->addWidget(boundaryButton);
    auto* enabledButton = new QPushButton(QObject::tr("Enable / disable entrypoint"), &dialog); layout->addWidget(enabledButton);
    auto* aliasButton = new QPushButton(QObject::tr("Name selected handler or resource entry..."), &dialog); layout->addWidget(aliasButton);
    auto* tileButton = new QPushButton(QObject::tr("Overworld selectors…"), &dialog); layout->addWidget(tileButton);
    rel::Catalog catalog; std::vector<Candidate> candidates; std::shared_ptr<const SctAuthoringState> snapshot; SctScriptId script;
    const auto refresh = [&] {
        snapshot = controller.authoringState(); list->clear(); candidates.clear(); details->clear(); if (!snapshot) return;
        for (const auto& s : snapshot->project.scripts) if (snapshot->project.find(s.baseline)->source.locator == locator) script = s.id;
        if (!script.valid()) return;
        if (catalog.resources.empty()) for (const auto& association : snapshot->project.find(script)->resources) {
            auto resourceLocator = AssetLocator::fromRelativePath(association.resource.asset);
            if (!resourceLocator) continue;
            auto inspected = SctEntrypointAuthoring::inspectResource(context,resourceLocator.value(),association.role);
            if (inspected) catalog.resources.push_back(std::move(inspected).takeValue());
        }
        auto working = SctAuthoringMaterializer::workingState(snapshot->project, snapshot->programs, script); if (!working) { details->setPlainText(errors(working.diagnostics())); return; }
        const auto& doc = *working.value().document;
        const auto add = [&](Candidate c) {
            SctMeaningTarget target;target.script=script;
            if(c.existing)target.entity=*c.existing;
            else if(c.binding.provider && std::holds_alternative<rel::Entry>(*c.binding.provider))target.resource=std::get<rel::Entry>(*c.binding.provider);
            else target=SctInterpretation::sectionTarget(snapshot->project,script,c.section);
            auto alias=SctInterpretation::label(*snapshot,target,"");list->addItem(QString::fromStdString(alias.empty()?c.name:alias+" — "+c.name)); candidates.push_back(std::move(c));
        };
        for (const auto& e : snapshot->project.entrypoints) if (e.script == script && e.binding) {
            const auto* seq = snapshot->project.find(e.binding->sequence);
            add({(e.binding->kind == SctEntrypointKind::Lifecycle ? "Required: " : e.enabled ? "Enabled: " : "Disabled: ") + e.name, *e.binding, seq ? seq->section : sct::SctSectionId{}, false, e.id});
        }
        for (const auto& section : doc.sections) if (const auto* body = std::get_if<sct::SctScriptSectionContent>(&section.content)) {
            if (section.nameBytes == "init" || section.nameBytes == "loop") add({"Required lifecycle: " + section.nameBytes, {}, section.id});
            for (const auto& i : body->instructions) if (i.opcode == 0 || i.opcode == 3) for (const auto& p : i.fixedParameters) if (p.schemaIndex == 0)
                if (const auto* expression = std::get_if<sct::SctCanonicalExpression>(&p.value)) {
                    for (const auto& read : rel::reads(*expression)) {
                        SctEntrypointBinding b; b.predicate = sct::SctParameterSite{i.id, {0,{}}};
                        if (read.kind == sct::SctScptValueKind::IntegerVariableLow16Comparison && read.index == 15) {
                            b.kind = SctEntrypointKind::Arrival; add({"Arrival predicate: " + section.nameBytes + " / " + std::to_string(i.id.value()), b, section.id});
                        } else if (read.kind == sct::SctScptValueKind::BitVariable) {
                            b.kind = SctEntrypointKind::SignalConsumer; b.signalBit = read.index;
                            b.provider = SctUnresolvedBinding{"BitVar[" + std::to_string(read.index) + "] read", "Provider not selected; ordered level check retained.", {}};
                            add({"Signal consumer: BitVar[" + std::to_string(read.index) + "] · " + section.nameBytes + " / " + std::to_string(i.id.value()), b, section.id});
                        }
                    }
                }
        }
        for (const auto& r : catalog.resources) for (const auto& entry : r.entries) {
            if (rel::assetKey(entry.function) == "eventhook") continue;
            const auto name = rel::handlerName(entry.tableId); if (!name) continue;
            SctEntrypointBinding b; b.kind = SctEntrypointKind::DirectHandler; b.provider = entry; b.resourceRole = r.role;
            const auto found = std::ranges::find(doc.sections, *name, &sct::SctDocumentSection::nameBytes);
            add({r.identity.asset + " · index " + std::to_string(entry.tableIndex) + " · entryId " + std::to_string(entry.entryId) + " · tblId " + std::to_string(entry.tableId) + " · " + entry.function + " → " + *name,
                b, found == doc.sections.end() ? sct::SctSectionId{} : found->id, found == doc.sections.end()});
        }
        QString report;
        for (const auto& d : SctEntrypointAuthoring::inspectBindings(snapshot->project, script, catalog)) report += QString::fromStdString(d.message) + '\n';
        report += QObject::tr("\nResource operations in section/instruction order (not a global residency timeline):\n");
        for (const auto& op : rel::resourceOperations(doc)) report += QString::number(op.section.value()) + " / " + QString::number(op.instruction.value()) + " · "
            + (op.kind == rel::ResourceOperationKind::Load ? "Load " : op.kind == rel::ResourceOperationKind::Wait ? "Wait " : "Release ")
            + QString::fromStdString(op.asset.value_or("unresolved resource")) + (op.scheduled ? " · scheduled" : "") + (op.skipRefresh ? " · skip refresh" : "") + '\n';
        details->setPlainText(report);
    };
    QObject::connect(inspect, &QPushButton::clicked, &dialog, [&] {
        if (resources->currentIndex() < 0) return;
        auto result = SctEntrypointAuthoring::inspectResource(context, assets[resources->currentIndex()], role->currentIndex() == 0 ? rel::ResourceRole::Base : role->currentIndex() == 1 ? rel::ResourceRole::Auxiliary : rel::ResourceRole::StreamedTile);
        if (!result) { QMessageBox::warning(&dialog, QObject::tr("Resource"), errors(result.diagnostics())); return; }
        auto associated = controller.executeAuthoringCommand(snapshot->project.revision,"Associate optional MLD",[&](const auto& state) { return SctEntrypointAuthoring::associateResource(state,script,result.value()); });
        if (!associated) { QMessageBox::warning(&dialog,QObject::tr("Resource"),errors(associated.diagnostics())); return; }
        std::erase_if(catalog.resources, [&](const auto& r) { return r.identity.asset == result.value().identity.asset; });
        catalog.resources.push_back(std::move(result).takeValue()); refresh();
    });
    QObject::connect(declareButton,&QPushButton::clicked,&dialog,[&] { showSctNamedEntryEditor(controller,script,&dialog); refresh(); });
    QObject::connect(boundaryButton,&QPushButton::clicked,&dialog,[&] {
        const auto row = list->currentRow(); if (row < 0 || row >= static_cast<int>(candidates.size()) || !candidates[row].existing) return;
        showSctActivationEditor(controller,*candidates[row].existing,&dialog); refresh();
    });
    QObject::connect(enabledButton,&QPushButton::clicked,&dialog,[&] {
        const auto row = list->currentRow(); if (row < 0 || row >= static_cast<int>(candidates.size()) || !candidates[row].existing || !snapshot) return;
        const auto id = *candidates[row].existing; const auto* e = snapshot->project.find(id);
        auto result = controller.executeAuthoringCommand(snapshot->project.revision,"Change entrypoint activation",[&](const auto& state) { return SctEntrypointAuthoring::setEnabled(state,id,!e->enabled); });
        if (!result) QMessageBox::warning(&dialog,QObject::tr("Activation"),errors(result.diagnostics())); refresh();
    });
    QObject::connect(aliasButton,&QPushButton::clicked,&dialog,[&] {
        const auto row=list->currentRow();if(row<0||row>=static_cast<int>(candidates.size())||!snapshot)return;
        const auto& c=candidates[row];SctMeaningTarget t;t.script=script;
        if(c.existing)t.entity=*c.existing;
        else if(c.binding.provider && std::holds_alternative<rel::Entry>(*c.binding.provider))t.resource=std::get<rel::Entry>(*c.binding.provider);
        else t=SctInterpretation::sectionTarget(snapshot->project,script,c.section);
        bool ok=false;auto name=QInputDialog::getText(&dialog,QObject::tr("Display alias"),QString::fromStdString(SctInterpretation::describe(*snapshot,t)),QLineEdit::Normal,{},&ok);
        if(ok&&!name.trimmed().isEmpty()){auto result=controller.executeAuthoringCommand(snapshot->project.revision,"Name entrypoint or resource",[&](const auto& state){return SctInterpretation::setName(state,t,name.toStdString());});if(!result)QMessageBox::warning(&dialog,QObject::tr("Alias"),errors(result.diagnostics()));refresh();}
    });
    QObject::connect(tileButton,&QPushButton::clicked,&dialog,[&] { showSctTileSelectorEditor(controller,script,context,&dialog); refresh(); });
    QObject::connect(list, &QListWidget::currentRowChanged, &dialog, [&](int row) {
        const bool editable = row >= 0 && row < static_cast<int>(candidates.size()) && candidates[row].existing.has_value()
            && candidates[row].binding.kind != SctEntrypointKind::Lifecycle;
        boundaryButton->setEnabled(editable); enabledButton->setEnabled(editable);
        if (row < 0 || row >= static_cast<int>(candidates.size()) || !snapshot) return;
        const auto& c = candidates[row]; QString text = QString::fromStdString(c.name) + '\n';
        if (c.binding.provider) if (const auto* entry = std::get_if<rel::Entry>(&*c.binding.provider)) {
            text += QString::fromStdString(entry->resource.asset + "\nRevision: " + entry->resource.revision + "\n" + rel::resolve(*entry, catalog).explanation) + '\n';
        }
        auto working = SctAuthoringMaterializer::workingState(snapshot->project, snapshot->programs, script); if (!working) return;
        for (const auto& section : working.value().document->sections) if (section.id == c.section) if (const auto* body = std::get_if<sct::SctScriptSectionContent>(&section.content)) {
            text += QObject::tr("Ordered section response, including preparation and explicit consumption:\n");
            for (const auto& i : body->instructions) {
                const auto* schema = sct::findSctOpcodeSchema(i.opcode);
                text += QString::number(i.id.value()) + " · opcode " + QString::number(i.opcode) + (schema ? " · " + QString::fromUtf8(schema->semantic.mnemonic.data(), static_cast<int>(schema->semantic.mnemonic.size())) : "");
                for (const auto& p : i.fixedParameters) if (const auto* e = std::get_if<sct::SctCanonicalExpression>(&p.value)) text += " · " + QString::fromStdString(SctExpressionLanguage::project(*e).text);
                text += '\n';
            }
        }
        details->setPlainText(text);
    });
    QObject::connect(bindButton, &QPushButton::clicked, &dialog, [&] {
        const int row = list->currentRow(); if (row < 0 || row >= static_cast<int>(candidates.size()) || !snapshot) return;
        auto candidate = candidates[row]; auto b = candidate.binding;
        if (candidate.existing) candidate.name = snapshot->project.find(*candidate.existing)->name;
        bool selectedEvidence = !candidate.existing && b.provider && std::holds_alternative<rel::Entry>(*b.provider);
        QStringList labels; std::vector<SctPortId> ports;
        for (const auto& port : snapshot->project.ports) if (port.action) if (const auto* owner = std::get_if<SctModuleId>(&port.owner)) {
            const auto* module = snapshot->project.find(*owner); if (module && module->script == script) { labels << QString::fromStdString(module->name + " / " + port.name); ports.push_back(port.id); }
        }
        if (ports.empty()) { QMessageBox::information(&dialog, QObject::tr("Response"), QObject::tr("Create or extract a module with an action first.")); return; }
        bool ok = false; const auto label = QInputDialog::getItem(&dialog, QObject::tr("Response"), QObject::tr("Module action port"), labels, 0, false, &ok); if (!ok) return;
        b.response = ports[labels.indexOf(label)];
        if (candidate.existing && (b.kind == SctEntrypointKind::DirectHandler || b.kind == SctEntrypointKind::NamedSection)) {
            QStringList entries{QObject::tr("Keep current optional resource evidence")}; std::vector<std::pair<rel::Entry,rel::ResourceRole>> choices;
            for (const auto& r : catalog.resources) for (const auto& e : r.entries) if (rel::handlerName(e.tableId)) { choices.push_back({e,r.role}); entries << QString::fromStdString(e.resource.asset) + QObject::tr(" · index %1 · tblId %2 · ").arg(e.tableIndex).arg(e.tableId) + QString::fromStdString(e.function); }
            const auto selected = QInputDialog::getItem(&dialog, QObject::tr("Optional source"), QObject::tr("Retain evidence or explicitly select an MLD entry"), entries, 0, false, &ok); if (!ok) return;
            if (const auto choice = entries.indexOf(selected); choice > 0) {
                b.provider = choices[choice-1].first; b.resourceRole = choices[choice-1].second;
                b.kind = SctEntrypointKind::DirectHandler; selectedEvidence = true;
            }
        }
        if (b.kind == SctEntrypointKind::SignalConsumer) {
            const auto providers = rel::signalProviders(catalog); QStringList names{QObject::tr("Keep current optional provider evidence")}; std::vector<rel::SignalProvider> matching;
            for (const auto& p : providers) if (p.bit == b.signalBit) { matching.push_back(p); names << QString::fromStdString(p.entry.resource.asset) + QObject::tr(" · index %1 · candidate BitVar[%2]").arg(p.entry.tableIndex).arg(p.bit); }
            const auto selected = QInputDialog::getItem(&dialog, QObject::tr("Signal provider"), QObject::tr("Parameter 8 candidates are inferred; selection is your assertion"), names, 0, false, &ok); if (!ok) return;
            if (names.indexOf(selected) > 0) { b.provider = matching[names.indexOf(selected)-1].entry; selectedEvidence = true; for (const auto& r : catalog.resources) if (r.identity == matching[names.indexOf(selected)-1].entry.resource) b.resourceRole = r.role; }
        }
        if (selectedEvidence) {
            const auto& entry = std::get<rel::Entry>(*b.provider);
            auto resourceLocator = AssetLocator::fromRelativePath(entry.resource.asset);
            if (!resourceLocator) { QMessageBox::warning(&dialog,QObject::tr("Resource"),errors(resourceLocator.diagnostics())); return; }
            auto current = SctEntrypointAuthoring::inspectResource(context,resourceLocator.value(),b.resourceRole);
            if (!current) { QMessageBox::warning(&dialog,QObject::tr("Resource"),errors(current.diagnostics())); return; }
            const auto resolution = rel::resolve(entry,{{current.value()}});
            if (resolution.status != rel::Status::Exact) {
                QMessageBox::warning(&dialog,QObject::tr("Resource changed"),QString::fromStdString(resolution.explanation)); return;
            }
        }
        if (!candidate.newHandler) {
            auto working = SctAuthoringMaterializer::workingState(snapshot->project, snapshot->programs, script); if (!working) return;
            QStringList sites{QObject::tr("Section starts at selected module port (no operand edit)")}; std::vector<sct::SctParameterSite> addresses;
            for (const auto& section : working.value().document->sections) if (section.id == candidate.section) if (const auto* body = std::get_if<sct::SctScriptSectionContent>(&section.content)) for (const auto& i : body->instructions) {
                const auto add = [&](const auto& ps, std::optional<std::uint32_t> group) { for (const auto& p : ps) if (std::holds_alternative<sct::SctInstructionReference>(p.value)) {
                    addresses.push_back({i.id,{p.schemaIndex,group}}); sites << QObject::tr("Instruction %1 · opcode %2 · target operand %3%4").arg(i.id.value()).arg(i.opcode).arg(p.schemaIndex).arg(group ? QObject::tr(" · group %1").arg(*group) : QString{});
                }};
                add(i.fixedParameters, {}); for (std::uint32_t g=0; g<i.repeatedParameterGroups.size(); ++g) add(i.repeatedParameterGroups[g].parameters,g);
            }
            const auto selected = QInputDialog::getItem(&dialog, QObject::tr("Response placement"), QObject::tr("Existing call/branch operand; conditions and clears keep their order"), sites, 0, false, &ok); if (!ok) return;
            b.responseSite = sites.indexOf(selected) == 0 ? std::nullopt : std::optional{addresses[sites.indexOf(selected)-1]};
        }
        const auto result = controller.executeAuthoringCommand(snapshot->project.revision, "Bind entrypoint response", [=,&catalog](const auto& state) mutable -> Result<SctAuthoringState> {
            if (candidate.newHandler) return SctEntrypointAuthoring::createDirectHandler(state, script, std::get<rel::Entry>(*b.provider), b.resourceRole, std::get<SctPortId>(b.response), catalog);
            auto next = state;
            const auto found = std::ranges::find_if(next.project.sequences, [&](const auto& s) { return s.script == script && s.section == candidate.section; });
            if (found == next.project.sequences.end()) { auto promoted = SctSequenceAuthoring::promoteSequence(next,script,candidate.section,candidate.name); if (!promoted) return promoted; next = std::move(promoted).takeValue(); b.sequence = next.project.sequences.back().id; }
            else b.sequence = found->id;
            return SctEntrypointAuthoring::bind(next,script,candidate.name,b,catalog,candidate.existing);
        });
        if (!result) QMessageBox::warning(&dialog, QObject::tr("Entrypoint"), errors(result.diagnostics())); refresh();
    });
    refresh(); dialog.exec();
}
}
