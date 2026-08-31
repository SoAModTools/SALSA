#include "Application/MainWindow.h"

#include "SalsaCore/Application/ApplicationInfo.h"

#include <QApplication>
#include <QCoreApplication>
#include <QStringList>

#include <cstdlib>

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);

    const auto name = salsa::core::applicationName();
    QCoreApplication::setApplicationName(
        QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())));

    const auto version = salsa::core::applicationVersion();
    QCoreApplication::setApplicationVersion(
        QString::fromUtf8(version.data(), static_cast<qsizetype>(version.size())));

    salsa::qt::MainWindow mainWindow;
    if (QCoreApplication::arguments().contains(QStringLiteral("--smoke-test"))) {
        return EXIT_SUCCESS;
    }

    mainWindow.show();
    return application.exec();
}
