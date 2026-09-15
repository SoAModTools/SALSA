#pragma once
#include "Editor/FeatureUi.h"
#include "Editor/StoryTools.h"
#include "SalsaCore/Sct/SctExpressionLanguage.h"
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
namespace salsa::qt::story_ui {
using namespace feature_ui;
inline std::uint64_t number(ed::Subject id) {
    return std::visit([](auto id) { return id.value; }, id);
}
inline void catalog(EditorSessionPresenter *s, QObject *owner, std::function<void(ed::StoryCatalog)> done) {
    if (!s->backend() || s->busy())
        return;
    auto b = s->backend();
    auto r = s->revision();
    background(
        owner, [=] { return b->storyCatalog(r); },
        [=](auto result) {
            if (r == s->revision() && report(s, result))
                done(std::move(result).takeValue());
        });
}
inline void decision(EditorSessionPresenter *s, QWidget *host, ed::Subject subject, std::string observation) {
    auto b = s->backend();
    auto r = s->revision();
    background(
        host, [=] { return b->explanation(r, subject); },
        [=](auto result) {
            if (r != s->revision() || !report(s, result))
                return;
            QDialog dialog(host);
            dialog.setWindowTitle(QObject::tr("Item interpretation and explanation"));
            dialog.resize(650, 470);
            auto *l = new QVBoxLayout(&dialog);
            auto *history = new QPlainTextEdit(&dialog);
            history->setReadOnly(true);
            QStringList lines{q(observation)};
            for (const auto &e : result.value().evidence)
                lines << q(e.description);
            for (const auto &d : result.value().decisions)
                lines << QObject::tr("Revision %1 — %2: %3")
                             .arg(d.revision)
                             .arg(d.decision == ed::InterpretationDecision::Accepted   ? "Accepted"
                                  : d.decision == ed::InterpretationDecision::Rejected ? "Rejected"
                                                                                       : "Superseded")
                             .arg(q(d.explanation));
            history->setPlainText(lines.join('\n'));
            l->addWidget(history);
            auto *reason = new QLineEdit(&dialog);
            reason->setPlaceholderText(QObject::tr("Your explanation (required)"));
            l->addWidget(reason);
            auto *bar = new QToolBar(&dialog);
            l->addWidget(bar);
            for (auto d : {ed::InterpretationDecision::Accepted, ed::InterpretationDecision::Rejected,
                           ed::InterpretationDecision::Superseded})
                bar->addAction(
                    d == ed::InterpretationDecision::Accepted   ? QObject::tr("Accept interpretation")
                    : d == ed::InterpretationDecision::Rejected ? QObject::tr("Reject interpretation")
                                                                : QObject::tr("Supersede interpretation"),
                    [&, d] {
                        if (reason->text().trimmed().isEmpty())
                            return;
                        ed::InterpretationRecord record{subject, r.revision, d, reason->text().toStdString(),
                                                        result.value().evidence};
                        record.evidence.push_back({core::SctEvidenceKind::Inference, observation});
                        dialog.accept();
                        s->execute(QObject::tr("Record interpretation"),
                                   ed::StoryEdit{ed::DecideInterpretation{record}}, r);
                    });
            dialog.exec();
        });
}
inline void alias(EditorSessionPresenter *s, QWidget *host, core::SctScriptId script, ed::Subject subject) {
    auto r = s->revision();
    auto b = s->backend();
    background(
        host, [=] { return b->organization(r, script); },
        [=](auto result) {
            if (r != s->revision() || !report(s, result))
                return;
            ed::ItemNote note{subject};
            for (auto &n : result.value().notes)
                if (n.subject == subject)
                    note = n;
            if (auto label = name(host, QObject::tr("Item alias"), note.alias)) {
                note.alias = *label;
                s->execute(QObject::tr("Set item alias"), ed::WriteNote{note}, r);
            }
        });
}
inline std::optional<core::SctActionId> pickAction(EditorSessionPresenter *s, QWidget *host,
                                                   core::SctScriptId script) {
    ed::BrowseRequest q;
    q.kind = ed::BrowseKind::Actions;
    q.script = script;
    auto item = pickReference(s, q, host);
    return item && std::holds_alternative<core::SctActionId>(*item)
               ? std::optional(std::get<core::SctActionId>(*item))
               : std::nullopt;
}
inline void editCondition(EditorSessionPresenter *s, QWidget *host, const ed::ConditionItem &condition,
                          ed::Revision revision) {
    bool ok;
    auto text = QInputDialog::getMultiLineText(host, QObject::tr("Edit complete condition"),
                                               QObject::tr("Expression"), q(condition.raw), &ok);
    if (!ok)
        return;
    auto parsed = core::SctExpressionLanguage::parse(text.toStdString());
    if (!parsed.succeeded()) {
        QMessageBox::information(host, QObject::tr("Condition"),
                                 QObject::tr("The expression could not be parsed."));
        return;
    }
    if (condition.route)
        s->execute(QObject::tr("Edit route requirement"),
                   ed::StoryEdit{ed::WriteRouteRequirement{*condition.route, *parsed.expression}}, revision);
    else
        s->execute(QObject::tr("Edit condition"),
                   ed::SetExpression{condition.subject,
                                     std::holds_alternative<core::SctActionId>(condition.subject)
                                         ? std::optional(ed::ParameterKey{0, {}})
                                         : std::nullopt,
                                     *parsed.expression},
                   revision);
}
} // namespace salsa::qt::story_ui
