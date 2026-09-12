#include "SctEntrypointTools.h"
#include "SalsaCore/Authoring/SctEntrypointAuthoring.h"
#include "SalsaCore/Authoring/SctOverworldAuthoring.h"
#include "SpiceSCT/SctDocumentIndex.h"
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTextEdit>
#include <QVBoxLayout>

namespace salsa::qt {
namespace {
using namespace core;
namespace sct = spice::sct;
namespace rel = smores::script;
void failure(QWidget* parent, const std::vector<Diagnostic>& diagnostics) {
    QString text; for (const auto& d : diagnostics) text += QString::fromStdString(d.message)+'\n';
    QMessageBox::warning(parent,QObject::tr("Authoring"),text);
}
struct Action { SctActionId id; SctSequenceId sequence; sct::SctInstructionId instruction; QString label; };
std::vector<Action> actions(const SctAuthoringState& state, SctScriptId script) {
    std::vector<Action> result;
    for (const auto& seq : state.project.sequences) if (seq.script == script) {
        auto views = SctSequenceAuthoring::actions(state,seq.id); if (!views) continue;
        for (const auto& a : views.value()) {
            QString label = QString::fromStdString(seq.name)+QString(" / %1 / %2").arg(a.id.value).arg(QString::fromStdString(a.label));
            for (const auto& p : a.instruction.fixedParameters) if (const auto* e = std::get_if<sct::SctCanonicalExpression>(&p.value)) label += " / "+QString::fromStdString(SctExpressionLanguage::project(*e).text);
            if (const auto assignment = rel::byteAssignment(a.instruction)) label += QString(" / ByteVar[%1]").arg(assignment->destination);
            result.push_back({a.id,seq.id,a.instruction.id,std::move(label)});
        }
    }
    return result;
}
bool promote(SctDocumentController& controller, SctScriptId script, QWidget* parent) {
    const auto snapshot = controller.authoringState(); if (!snapshot) return false;
    auto result = controller.executeAuthoringCommand(snapshot->project.revision,"Expose script actions",[&](const SctAuthoringState& state) -> Result<SctAuthoringState> {
        auto next = state; auto working = SctAuthoringMaterializer::workingState(next.project,next.programs,script);
        if (!working) return Result<SctAuthoringState>::failure(working.diagnostics());
        for (const auto& section : working.value().document->sections) if (std::holds_alternative<sct::SctScriptSectionContent>(section.content)
            && !std::ranges::any_of(next.project.sequences,[&](const auto& q) { return q.script == script && q.section == section.id; })) {
            auto changed = SctSequenceAuthoring::promoteSequence(next,script,section.id,section.nameBytes);
            if (!changed) return changed; next = std::move(changed).takeValue();
        }
        return Result<SctAuthoringState>::success(std::move(next));
    });
    if (!result) failure(parent,result.diagnostics()); return static_cast<bool>(result);
}
int pick(QWidget* parent, const QString& title, const QString& prompt, const QStringList& labels, int initial = 0) {
    if (labels.empty()) return -1; bool ok = false;
    const auto chosen = QInputDialog::getItem(parent,title,prompt,labels,initial,false,&ok);
    return ok ? static_cast<int>(labels.indexOf(chosen)) : -1;
}
}

void showSctNamedEntryEditor(SctDocumentController& controller, SctScriptId script, QWidget* parent) {
    const auto snapshot = controller.authoringState(); if (!snapshot) return;
    auto working = SctAuthoringMaterializer::workingState(snapshot->project,snapshot->programs,script);
    if (!working) { failure(parent,working.diagnostics()); return; }
    QStringList labels{QObject::tr("Create a named section…")}; std::vector<sct::SctSectionId> sections;
    for (const auto& s : working.value().document->sections) if (const auto* b = std::get_if<sct::SctScriptSectionContent>(&s.content); b && !b->instructions.empty()) {
        sections.push_back(s.id); labels << QString::fromStdString(s.nameBytes);
    }
    const auto selected = pick(parent,QObject::tr("Named entrypoint"),QObject::tr("Existing section or a new editable entry body"),labels); if (selected < 0) return;
    QString name;
    if (!selected) { bool ok; name = QInputDialog::getText(parent,QObject::tr("Named entrypoint"),QObject::tr("External section name (for example M04000)"),QLineEdit::Normal,{},&ok); if (!ok || name.isEmpty()) return; }
    auto result = controller.executeAuthoringCommand(snapshot->project.revision,"Declare named entrypoint",[&](const SctAuthoringState& state) -> Result<SctAuthoringState> {
        if (selected) return SctEntrypointAuthoring::declareNamed(state,script,sections[selected-1]);
        auto w = SctAuthoringMaterializer::workingState(state.project,state.programs,script); if (!w) return Result<SctAuthoringState>::failure(w.diagnostics());
        auto doc = std::make_shared<sct::SctDocument>(*w.value().document);
        const auto fail = [&](std::string message) { return Result<SctAuthoringState>::failure({DiagnosticSeverity::Error,DiagnosticCode::InvalidSctAuthoringProject,std::move(message)}); };
        const auto text = name.toStdString();
        if (text.size()>16 || std::ranges::any_of(text,[](unsigned char c) { return c<33 || c>126; })
            || std::ranges::any_of(doc->sections,[&](const auto& s) { return s.nameBytes == text; })) return fail("Choose a unique section name of at most 16 printable ASCII characters.");
        if (!doc->sections.empty()) if (const auto* tail = std::get_if<sct::SctScriptSectionContent>(&doc->sections.back().content);
            tail && (tail->instructions.empty() || (tail->instructions.back().opcode!=10 && tail->instructions.back().opcode!=12 && tail->instructions.back().opcode!=3))) return fail("Define the trailing section's continuation before appending a named entry.");
        const auto section = doc->allocateSectionId(); const auto guard = doc->allocateInstructionId(), ret = doc->allocateInstructionId();
        const auto yes = SctExpressionLanguage::parse("1");
        doc->sections.push_back({section,text,sct::SctScriptSectionContent{{{guard,0,true,{},{{0,*yes.expression},{1,sct::SctInstructionReference{ret}}}}, {ret,12}}}});
        w.value().document = doc; auto changed = SctAuthoringMaterializer::replaceWorkingState(state.project,state.programs,script,w.value());
        if (!changed) return Result<SctAuthoringState>::failure(changed.diagnostics());
        auto next = state; next.project = std::move(changed).takeValue(); return SctEntrypointAuthoring::declareNamed(next,script,section);
    });
    if (!result) failure(parent,result.diagnostics());
}

void showSctActivationEditor(SctDocumentController& controller, SctEntrypointId id, QWidget* parent) {
    auto snapshot = controller.authoringState(); if (!snapshot || !snapshot->project.find(id)) return;
    const auto script = snapshot->project.find(id)->script;
    if (!promote(controller,script,parent)) return; snapshot = controller.authoringState();
    const auto* entry = snapshot->project.find(id); if (!entry || !entry->binding) return;
    const auto all = actions(*snapshot,script); QStringList labels; for (const auto& a : all) labels << a.label;
    auto working = SctAuthoringMaterializer::workingState(snapshot->project,snapshot->programs,script); if (!working) return;
    const auto& doc = *working.value().document; const auto index = sct::SctDocumentIndex::build(doc);
    const auto* sequence = snapshot->project.find(entry->binding->sequence);
    const auto* section = sequence ? index.find(doc,sequence->section) : nullptr;
    if (!section) return;
    SctEntrypointActivation activation;
    if (entry->binding->kind == SctEntrypointKind::NamedSection || entry->binding->kind == SctEntrypointKind::DirectHandler) {
        activation = SctNamedActivation{section->nameBytes,sequence->actions.front().id};
    } else {
        int defaultStart = 0, defaultEnd = 0;
        if (entry->binding->predicate) {
            const auto analysis = sct::SctDocumentAnalysis::build(doc);
            if (const auto* structure = analysis.structuredControlFlow.findSection(section->id))
                for (const auto& region : structure->regions) if (region.id.headerInstruction == entry->binding->predicate->instruction && region.join)
                    for (int i=0;i<static_cast<int>(all.size());++i) {
                        if (all[i].instruction == region.id.headerInstruction) defaultStart=i;
                        if (all[i].instruction == region.join->entryInstruction) defaultEnd=i;
                    }
        }
        const auto start = pick(parent,QObject::tr("Activation block"),QObject::tr("First action to skip, including preparation"),labels,defaultStart); if (start<0) return;
        const auto continuation = pick(parent,QObject::tr("Activation block"),QObject::tr("Resume here after skipping response and consumption"),labels,defaultEnd); if (continuation<0) return;
        std::vector<SctLocalActivation> edges; QStringList edgeLabels;
        const auto startLoc = index.instructionLocation(all[start].instruction);
        const auto* startSection = startLoc ? index.find(doc,startLoc->sectionId) : nullptr;
        if (startSection && startLoc->instructionOrdinal==0 && (startSection->nameBytes=="init" || startSection->nameBytes=="loop")) {
            edges.push_back({all[start].id,{},all[start].id,all[continuation].id,true}); edgeLabels << QObject::tr("Required lifecycle enters this first action (root remains enabled)");
        }
        for (const auto& a : all) {
            const auto* i = index.find(doc,a.instruction);
            const auto add = [&](const auto& ps, std::optional<std::uint32_t> group) { for (const auto& p : ps)
                if (const auto* ref = std::get_if<sct::SctInstructionReference>(&p.value); ref && ref->target == all[start].instruction) {
                    edges.push_back({a.id,sct::SctParameterAddress{p.schemaIndex,group},all[start].id,all[continuation].id});
                    edgeLabels << a.label+QString(" / target %1 / group %2").arg(p.schemaIndex).arg(group ? QString::number(*group) : "none");
                }
            };
            add(i->fixedParameters,{}); for (std::uint32_t g=0;g<i->repeatedParameterGroups.size();++g) add(i->repeatedParameterGroups[g].parameters,g);
            auto trial = snapshot->project; auto e = std::ranges::find(trial.entrypoints,id,&SctAuthoringEntrypoint::id);
            SctLocalActivation fall{a.id,{},all[start].id,all[continuation].id}; e->activation=fall;
            if (SctEntrypointAuthoring::validateActivations(trial,script,doc)) { edges.push_back(fall); edgeLabels << a.label+QObject::tr(" / fallthrough"); }
        }
        const auto edge = pick(parent,QObject::tr("Activation edge"),QObject::tr("Only this incoming route will bypass the selected block"),edgeLabels); if (edge<0) return;
        activation = edges[edge];
        QDialog review(parent); review.setWindowTitle(QObject::tr("Review activation block")); auto* layout = new QVBoxLayout(&review);
        auto* text = new QTextEdit(&review); text->setReadOnly(true); layout->addWidget(text);
        QString description = QObject::tr("Incoming: ")+edgeLabels[edge]+"\n"+QObject::tr("Start: ")+labels[start]+"\n"+QObject::tr("Bypass continuation: ")+labels[continuation]+"\n\n";
        description += QObject::tr("Ordered script actions; branches retain their original targets:\n");
        for (int i=start;i<static_cast<int>(all.size()) && (continuation<=start || i<=continuation);++i) description += all[i].label+'\n';
        text->setPlainText(description); auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&review); layout->addWidget(buttons);
        QObject::connect(buttons,&QDialogButtonBox::accepted,&review,&QDialog::accept); QObject::connect(buttons,&QDialogButtonBox::rejected,&review,&QDialog::reject);
        review.resize(850,550); if (review.exec()!=QDialog::Accepted) return;
    }
    auto result = controller.executeAuthoringCommand(snapshot->project.revision,"Define activation boundary",[&](const SctAuthoringState& state) { return SctEntrypointAuthoring::setActivation(state,id,activation); });
    if (!result) failure(parent,result.diagnostics());
}

void showSctTileSelectorEditor(SctDocumentController& controller, SctScriptId script, const GameProjectContext& context, QWidget* parent) {
    if (!promote(controller,script,parent)) return;
    auto snapshot = controller.authoringState(); auto all = actions(*snapshot,script); if (all.empty()) return;
    QDialog dialog(parent); dialog.setWindowTitle(QObject::tr("Overworld selectors")); dialog.resize(1050,750); auto* layout = new QVBoxLayout(&dialog);
    auto* explanation = new QLabel(QObject::tr("Selection for subsequent overworld initialization. Ordinary streaming reuses captured filenames. MLD availability is optional context."),&dialog); explanation->setWordWrap(true); layout->addWidget(explanation);
    auto* first = new QComboBox(&dialog); auto* last = new QComboBox(&dialog); layout->addWidget(new QLabel(QObject::tr("First action / last action (inclusive)"))); layout->addWidget(first); layout->addWidget(last);
    auto* seeds = new QTextEdit(&dialog); seeds->setPlaceholderText("Known initial byte values, one per line: 6=16\n7=6\n63=11"); seeds->setMaximumHeight(95); layout->addWidget(seeds);
    auto* preview = new QPushButton(QObject::tr("Preview selected assignments"),&dialog); layout->addWidget(preview);
    auto* report = new QTextEdit(&dialog); report->setReadOnly(true); layout->addWidget(report,1);
    auto* replace = new QPushButton(QObject::tr("Edit selected byte assignment…"),&dialog); layout->addWidget(replace);
    auto* insert = new QPushButton(QObject::tr("Insert assignments before first action…"),&dialog); layout->addWidget(insert);
    auto* choose = new QPushButton(QObject::tr("Select tile or overworld variant…"),&dialog); layout->addWidget(choose);
    rel::Catalog catalog; std::vector<rel::ResourceIdentity> assets;
    for (const auto& asset : context.assets().snapshot().assets) {
        rel::ResourceIdentity r{context.dataset().identity.fingerprint.digest.toHex(),asset.locator.identityKey(),asset.revision.digest.toHex(),"catalog"}; assets.push_back(r);
        if (rel::tileVariant(r.asset)) catalog.resources.push_back({r,rel::ResourceRole::StreamedTile,{}});
    }
    const auto describe = [&](const rel::KnownBytes& values) {
        QString text;
        const auto show = [&](const rel::ResourceSelection& r) { return r.state == rel::SelectionState::Unknown ? QObject::tr("unknown") : r.state == rel::SelectionState::Unrepresentable ? QObject::tr("unrepresentable filename") : QString::fromStdString(r.filename)+ (r.availability==rel::Status::Exact ? QObject::tr(" — available") : r.availability==rel::Status::Ambiguous ? QObject::tr(" — multiple matches") : QObject::tr(" — no project match")); };
        const auto known = [&](std::uint32_t index) { const auto v=values.find(index); return v!=values.end() && v->second ? QString::number(*v->second) : QObject::tr("unknown"); };
        text += "ByteVar[6] = "+known(6)+": "+show(rel::selectOverworld(true,values,assets))+" / "+show(rel::selectOverworld(false,values,assets))+"\n";
        for (const auto& f : rel::tileFamilies()) {
            const auto selected=rel::selectTile(f.key,values,catalog);
            text += QString::fromStdString(f.key)+" / "+(f.byteVariable ? QString("ByteVar[%1] = %2").arg(*f.byteVariable).arg(known(*f.byteVariable)) : QObject::tr("fixed a"))+" / "+show(selected)+"\n";
            for (const auto& asset:selected.candidates) text += "    "+QString::fromStdString(asset.asset)+"\n";
        }
        report->setPlainText(text);
    };
    const auto refresh = [&] { snapshot=controller.authoringState(); all=actions(*snapshot,script); first->clear(); last->clear(); for (const auto& a : all) { first->addItem(a.label); last->addItem(a.label); } describe({}); };
    QObject::connect(preview,&QPushButton::clicked,&dialog,[&] {
        rel::KnownBytes values; bool valid=true;
        for (const auto& line : seeds->toPlainText().split('\n')) if (!line.trimmed().isEmpty()) { const auto parts=line.split('='); bool a=false,b=false;
            const auto index=parts.value(0).trimmed().toUInt(&a); const auto value=parts.value(1).trimmed().toUInt(&b);
            if (parts.size()!=2 || !a || !b || index>0x7fff || value>255 || values.contains(index)) { valid=false; break; } values[index]=static_cast<std::uint8_t>(value);
        }
        if (!valid) { QMessageBox::warning(&dialog,QObject::tr("Preview"),QObject::tr("Use unique index=value lines with byte values 0–255.")); return; }
        const auto a=first->currentIndex(),b=last->currentIndex(); if (a<0 || b<0) return;
        if (all[a].sequence!=all[b].sequence) { QMessageBox::warning(&dialog,QObject::tr("Preview"),QObject::tr("Choose a range within one sequence.")); return; }
        auto result=SctOverworldAuthoring::preview(*snapshot,all[a].sequence,all[a].id,all[b].id,values); if (!result) failure(&dialog,result.diagnostics()); else describe(result.value());
    });
    const auto edit = [&](bool replacing) {
        const auto at=first->currentIndex(); if (at<0) return; bool ok=false; QString initial;
        if (replacing) { auto views=SctSequenceAuthoring::actions(*snapshot,all[at].sequence); if (!views) return;
            const auto found=std::ranges::find(views.value(),all[at].id,&SctSequenceActionView::id); const auto assignment=rel::byteAssignment(found->instruction);
            if (!assignment) { QMessageBox::information(&dialog,QObject::tr("Assignment"),QObject::tr("Select a SetByteVar action.")); return; }
            initial=QString::number(assignment->destination)+"="+QString::fromStdString(SctExpressionLanguage::project(assignment->expression).text);
        }
        const auto text=QInputDialog::getMultiLineText(&dialog,QObject::tr("Byte assignments"),QObject::tr("One index=expression per line. Tile 32 uses 7; tile 22 uses 45; overworld script uses 6. Copies such as 6=ByteVar[63] are supported."),initial,&ok); if (!ok) return;
        std::vector<SctSelectorEdit> edits;
        for (const auto& line : text.split('\n')) if (!line.trimmed().isEmpty()) { const auto equal=line.indexOf('='); bool number=false; const auto destination=line.left(equal).trimmed().toUInt(&number); auto expression=SctExpressionLanguage::parse(line.mid(equal+1).toStdString());
            if (equal<0 || !number || !expression.succeeded() || (replacing && !edits.empty())) { QMessageBox::warning(&dialog,QObject::tr("Assignment"),QObject::tr("Invalid assignment. Replacement accepts one line.")); return; }
            edits.push_back({replacing ? std::optional{all[at].id} : std::nullopt,destination,*expression.expression});
        }
        auto result=controller.executeAuthoringCommand(snapshot->project.revision,replacing ? "Edit selector assignment" : "Insert selector assignments",[&](const SctAuthoringState& state) { return SctOverworldAuthoring::edit(state,all[at].sequence,all[at].id,edits); });
        if (!result) failure(&dialog,result.diagnostics()); else refresh();
    };
    QObject::connect(replace,&QPushButton::clicked,&dialog,[&] { edit(true); }); QObject::connect(insert,&QPushButton::clicked,&dialog,[&] { edit(false); });
    QObject::connect(choose,&QPushButton::clicked,&dialog,[&] {
        const auto at=first->currentIndex(); if (at<0) return;
        QStringList families{QObject::tr("Overworld script and base MLD")}; std::vector<std::uint32_t> indices{6};
        for (const auto& f:rel::tileFamilies()) if (f.byteVariable) { families << QObject::tr("Tile %1").arg(QString::fromStdString(f.key)); indices.push_back(*f.byteVariable); }
        const auto family=pick(&dialog,QObject::tr("Select variant"),QObject::tr("Resource family"),families); if (family<0) return;
        bool ok=false; const auto suffix=QInputDialog::getText(&dialog,QObject::tr("Select variant"),QObject::tr("Variant letter (a, b, c, …); project availability is advisory"),QLineEdit::Normal,"a",&ok);
        if (!ok || suffix.size()!=1 || suffix[0]<'a' || suffix[0]>'z') return;
        const auto value=static_cast<unsigned>(suffix[0].unicode()-'a'); const auto expression=SctExpressionLanguage::parse(std::to_string(value));
        const auto mode=pick(&dialog,QObject::tr("Placement"),QObject::tr("Apply this selection"),{QObject::tr("Insert before selected action"),QObject::tr("Replace selected byte assignment")}); if (mode<0) return;
        const std::vector<SctSelectorEdit> edits{{mode==1 ? std::optional{all[at].id} : std::nullopt,indices[family],*expression.expression}};
        auto result=controller.executeAuthoringCommand(snapshot->project.revision,"Select resource variant",[&](const SctAuthoringState& state) { return SctOverworldAuthoring::edit(state,all[at].sequence,all[at].id,edits); });
        if (!result) failure(&dialog,result.diagnostics()); else refresh();
    });
    refresh(); dialog.exec();
}
}
