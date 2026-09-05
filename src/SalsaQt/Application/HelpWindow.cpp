#include "Application/HelpWindow.h"

#include <QDesktopServices>
#include <QFile>
#include <QHBoxLayout>
#include <QListWidget>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QUrl>

namespace salsa::qt {

HelpWindow::HelpWindow(QWidget* parent)
    : QWidget(parent, Qt::Window), browser_(new QTextBrowser(this)),
      topics_(new QListWidget(this)) {
    setWindowTitle(tr("SALSA Help"));
    resize(900, 650);
    topics_->setMaximumWidth(220);
    const std::pair<const char*, const char*> entries[] = {
        {"User Guide", "README.md"},
        {"Legacy Import", "LegacyImport.md"},
        {"Exporting SCT", "Exporting.md"},
        {"Parameters and SCPT", "Parameters.md"},
        {"Keyboard Shortcuts", "KeyboardShortcuts.md"},
    };
    for (const auto& [label, resource] : entries) {
        auto* item = new QListWidgetItem(tr(label), topics_);
        item->setData(Qt::UserRole, QString::fromLatin1(resource));
    }
    auto* layout = new QHBoxLayout(this);
    layout->addWidget(topics_);
    layout->addWidget(browser_, 1);
    browser_->setOpenLinks(false);
    browser_->setOpenExternalLinks(false);
    connect(topics_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        showTopic(item->data(Qt::UserRole).toString());
    });
    connect(topics_, &QListWidget::currentItemChanged, this,
        [this](QListWidgetItem* item) {
            if (item != nullptr) showTopic(item->data(Qt::UserRole).toString());
        });
    connect(browser_, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
        if (url.scheme() == QStringLiteral("qrc") || url.scheme().isEmpty()) {
            showTopic(url.fileName());
            return;
        }
        (void)QDesktopServices::openUrl(url);
    });
    topics_->setCurrentRow(0);
}

void HelpWindow::showTopic(const QString& resourceName) {
    const auto resource = QStringLiteral(":/help/") + resourceName;
    QFile file(resource);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        browser_->setPlainText(tr("This help topic is not available."));
        return;
    }
    browser_->document()->setBaseUrl(QUrl(QStringLiteral("qrc:/help/")));
    browser_->setMarkdown(QString::fromUtf8(file.readAll()));
    browser_->moveCursor(QTextCursor::Start);
}

}  // namespace salsa::qt
