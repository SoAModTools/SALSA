#include "Application/ImportCompletion.h"
#include "Application/ProgramOperation.h"
#include "Editor/EditorSessionPresenter.h"
#include <QLabel>
#include <QListWidget>
#include <QPointer>
#include <QTimer>
#include <QVBoxLayout>
#include <map>
namespace salsa::qt {
namespace ed = core::editor;
namespace {
QWidget *explanation(QWidget *parent, QString text) {
    auto *page = new QWidget(parent);
    auto *layout = new QVBoxLayout(page);
    auto *label = new QLabel(std::move(text), page);
    label->setWordWrap(true);
    layout->addWidget(label);
    return page;
}
void reviewStory(ProgramOperation &op, QPointer<EditorSessionPresenter> session, ed::StoryView result) {
    auto data = std::make_shared<ed::StoryView>(std::move(result));
    auto choices = std::make_shared<QPointer<QListWidget>>();
    auto backend = session->backend();
    op.offerNext(QObject::tr("Enable story authoring"),
        [backend, data, choices] {
            ed::OrganizeStory organize;
            if (*choices)
                for (auto *item : (*choices)->selectedItems())
                    organize.modules.push_back(data->candidates.at(item->data(Qt::UserRole).toULongLong()));
            return [backend, revision = data->revision, organize = std::move(organize)]
                (std::stop_token stop, const core::SctLoadingObserver &progress) -> core::Result<std::any> {
                if (progress) progress({"Organizing story authoring", 0, 0, {}});
                auto changed = backend->execute(revision, ed::StoryEdit{organize}, stop);
                if (!changed) return core::Result<std::any>::failure(changed.diagnostics());
                return core::Result<std::any>::success(std::any(std::move(changed).takeValue()));
            };
        },
        [session, revision = data->revision](std::any value, ProgramOperation &operation) {
            if (!session || session->revision() != revision) {
                operation.fail(QObject::tr("The project changed before story organization was installed."));
                return;
            }
            session->accept(std::any_cast<ed::Change>(std::move(value)));
            operation.finish(QObject::tr("Story authoring is ready."));
        },
        [data, choices](QWidget *parent) {
            auto *page = explanation(parent, QObject::tr(
                "Analyzed all %1 scripts. Select sections to organize as modules, then enable story authoring. "
                "Section boundaries are suggestions; existing modules are retained. "
                "Finish keeps instruction mode and leaves organization unchanged.").arg(data->scripts.size()));
            auto *list = new QListWidget(page);
            list->setSelectionMode(QAbstractItemView::ExtendedSelection);
            *choices = list;
            page->layout()->addWidget(list);
            auto *status = new QLabel(page);
            page->layout()->addWidget(status);
            std::map<core::SctScriptId, QString> names;
            for (const auto &[id, name] : data->scripts) names[id] = QString::fromStdString(name);
            auto *timer = new QTimer(page);
            QObject::connect(timer, &QTimer::timeout, page,
                [timer, list, status, data, names = std::move(names), at = std::size_t{0}]() mutable {
                    auto end = std::min(at + 128, data->candidates.size());
                    for (; at < end; ++at) {
                        const auto &candidate = data->candidates[at];
                        auto *item = new QListWidgetItem(names[candidate.script] + ": " +
                                                        QString::fromStdString(candidate.name), list);
                        item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(at));
                    }
                    status->setText(QObject::tr("%1 of %2 suggested modules shown").arg(at).arg(data->candidates.size()));
                    if (at == data->candidates.size()) timer->stop();
                });
            timer->start(0);
            return page;
        }, false);
}
} // namespace
void offerImportCompletion(ProgramOperation &op, EditorSessionPresenter *editor) {
    QPointer<EditorSessionPresenter> session = editor;
    auto backend = editor->backend();
    auto revision = editor->revision();
    op.offerNext(QObject::tr("Analyze story and cutscenes"),
        [backend, revision] {
            return [backend, revision](std::stop_token stop, const core::SctLoadingObserver &progress)
                -> core::Result<std::any> {
                ed::StoryRequest request;
                request.analyze = true;
                request.progress = progress;
                auto result = backend->story(revision, request, stop);
                if (!result) return core::Result<std::any>::failure(result.diagnostics());
                return core::Result<std::any>::success(std::any(std::move(result).takeValue()));
            };
        },
        [session, revision](std::any value, ProgramOperation &operation) {
            if (!session || session->revision() != revision) {
                operation.fail(QObject::tr("The project changed during story analysis."));
                return;
            }
            reviewStory(operation, session, std::any_cast<ed::StoryView>(std::move(value)));
        },
        [](QWidget *parent) {
            return explanation(parent, QObject::tr(
                "Import and localization processing are complete.\n\n"
                "Finish to use instruction mode, including the semantic editor. "
                "Or analyze the whole project to discover story and cutscene relationships and review module organization. "
                "You can also run this analysis later from Story tools. Cancelling analysis keeps the imported project open."));
        });
}
} // namespace salsa::qt
