#pragma once

#include "SalsaCore/Sct/SctDocumentLoader.h"

#include <QPoint>
#include <QString>

#include <optional>

namespace salsa::qt {

struct InteractionNotice final {
    QString code;
    QString message;
    QString documentIdentity;
    std::optional<core::SctNavigationTarget> target{};
    std::optional<QPoint> globalPosition{};
    bool prominent = false;
};

}  // namespace salsa::qt
