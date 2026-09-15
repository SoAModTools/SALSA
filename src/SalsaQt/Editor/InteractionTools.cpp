#include "Editor/StoryUi.h"
#include <QHeaderView>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableWidget>
namespace salsa::qt {
using namespace story_ui;
namespace {
class InteractionWindow final : public QDialog {
    EditorSessionPresenter *session;
    core::SctScriptId script;
    ed::InteractionView data;
    std::stop_source stop;
    std::uint64_t generation = 0;
    QListWidget *entries;
    QTableWidget *conditions;
    QLabel *status;
    QTimer *timer;
    std::vector<ed::ConditionItem> visible;
    std::size_t shown = 0;
    std::optional<ed::ConditionItem> current() {
        int i = conditions->currentRow();
        return i >= 0 && i < int(visible.size()) ? std::optional(visible[i]) : std::nullopt;
    }
    void page() {
        auto end = std::min(shown + 200, visible.size());
        for (; shown < end; ++shown) {
            const auto &c = visible[shown];
            int n = conditions->rowCount();
            conditions->insertRow(n);
            conditions->setItem(n, 0, new QTableWidgetItem(q(c.friendly)));
            conditions->setItem(n, 1, new QTableWidgetItem(q(c.raw)));
            conditions->setItem(
                n, 2, new QTableWidgetItem(c.route ? tr("Route-owned") : tr("Shared script logic")));
        }
        status->setText(tr("Showing %1 of %2 conditions. Shared logic affects all its users; recover "
                           "independent routes before separating their requirements.")
                            .arg(shown)
                            .arg(visible.size()));
    }
    void filter() {
        visible.clear();
        conditions->setRowCount(0);
        shown = 0;
        int entry = entries->currentRow() - 1;
        for (auto &c : data.conditions)
            if (entry < 0 || std::ranges::find(c.entrypoints, data.entries[entry].id) != c.entrypoints.end())
                visible.push_back(c);
        page();
    }
    void load() {
        auto b = session->backend();
        auto r = session->revision();
        if (!b)
            return;
        auto selectedScript = script;
        stop.request_stop();
        stop = std::stop_source{};
        auto token = stop.get_token();
        auto serial = ++generation;
        background(
            this, [=] { return b->interaction(r, selectedScript, token); },
            [this, r, serial](auto result) {
                if (serial != generation || r != session->revision() || !report(session, result))
                    return;
                int old = entries->currentRow() - 1;
                auto oldEntry = old >= 0 && old < int(data.entries.size())
                                    ? std::optional(data.entries[old].id)
                                    : std::nullopt;
                auto oldCondition = current();
                data = std::move(result).takeValue();
                QSignalBlocker block(entries);
                entries->clear();
                entries->addItem(tr("All script conditions"));
                for (auto &e : data.entries)
                    entries->addItem(q(e.name) + (e.required() ? tr(" — required")
                                                  : e.enabled  ? tr(" — enabled")
                                                               : tr(" — disabled")));
                auto found = oldEntry ? std::ranges::find(data.entries, *oldEntry, &ed::EntryItem::id)
                                      : data.entries.end();
                entries->setCurrentRow(
                    found == data.entries.end() ? 0 : static_cast<int>(found - data.entries.begin()) + 1);
                filter();
                if (oldCondition)
                    for (int row = 0; row < conditions->rowCount(); ++row)
                        if (visible[row].subject == oldCondition->subject)
                            conditions->setCurrentCell(row, 0);
            });
    }

  public:
    ~InteractionWindow() override { stop.request_stop(); }
    InteractionWindow(EditorSessionPresenter *s, core::SctScriptId id, QWidget *host)
        : QDialog(host), session(s), script(id) {
        setAttribute(Qt::WA_DeleteOnClose);
        setWindowTitle(tr("Script interactions"));
        resize(1100, 650);
        auto *l = new QVBoxLayout(this);
        auto *bar = new QToolBar(this);
        l->addWidget(bar);
        auto *split = new QSplitter(this);
        entries = new QListWidget(split);
        conditions = new QTableWidget(0, 3, split);
        conditions->setHorizontalHeaderLabels({tr("Meaning"), tr("Underlying condition"), tr("Scope")});
        conditions->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        conditions->setEditTriggers(QAbstractItemView::NoEditTriggers);
        split->addWidget(entries);
        split->addWidget(conditions);
        split->setStretchFactor(1, 1);
        l->addWidget(split);
        status = new QLabel(this);
        status->setWordWrap(true);
        l->addWidget(status);
        bar->addAction(tr("Open entry body"), [this] {
            int i = entries->currentRow() - 1;
            if (i >= 0)
                session->navigate(data.entries[i].body);
        });
        bar->addAction(tr("Entry alias…"), [this] {
            int i = entries->currentRow() - 1;
            if (i >= 0)
                alias(session, this, script, data.entries[i].id);
        });
        bar->addAction(tr("Explain entry…"), [this] {
            int i = entries->currentRow() - 1;
            if (i >= 0)
                decision(session, this, data.entries[i].id, "Entrypoint binding and activation evidence");
        });
        bar->addAction(tr("Edit condition…"), [this] {
            if (auto c = current())
                editCondition(session, this, *c, data.revision);
        });
        bar->addAction(tr("Inspect instructions"), [this] {
            if (auto c = current())
                session->navigate(c->subject);
        });
        bar->addAction(tr("Interpretation…"), [this] {
            if (auto c = current())
                decision(session, this, c->subject, c->raw + " — " + c->friendly);
        });
        bar->addAction(tr("Recover independent routes"), [this] {
            if (auto c = current())
                session->execute(tr("Recover independent routes"),
                                 ed::StoryEdit{ed::RecoverDispatch{c->owner}}, data.revision);
        });
        bar->addAction(tr("Refresh"), [this] { load(); });
        bar->addAction(tr("More conditions"), [this] { page(); });
        connect(entries, &QListWidget::currentRowChanged, this, [this] { filter(); });
        connect(conditions, &QTableWidget::cellDoubleClicked, this, [this](int, int) {
            if (auto c = current())
                session->navigate(c->subject);
        });
        connect(entries, &QListWidget::itemDoubleClicked, this, [this] {
            int i = entries->currentRow() - 1;
            if (i >= 0)
                session->navigate(data.entries[i].body);
        });
        timer = new QTimer(this);
        timer->setSingleShot(true);
        timer->setInterval(80);
        connect(timer, &QTimer::timeout, this, [this] { load(); });
        connect(session, &EditorSessionPresenter::changed, this, [this] { timer->start(); });
        connect(session, &EditorSessionPresenter::sessionChanged, this, &QDialog::close);
        load();
    }
};
} // namespace
InteractionTools::InteractionTools(EditorSessionPresenter *s, QWidget *h)
    : QObject(h), session_(s), host_(h) {
    action = new QAction(tr("Script interactions…"), this);
    connect(action, &QAction::triggered, this, [this] { open(); });
    availability(action, s);
}
void InteractionTools::open() {
    if (session_->inspection())
        (new InteractionWindow(session_, session_->inspection()->script, host_))->show();
}
} // namespace salsa::qt
