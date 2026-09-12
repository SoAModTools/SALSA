#pragma once
#include "SctDocumentController.h"
#include <functional>

namespace salsa::qt {
void showSctModuleBrowser(SctDocumentController& controller, const core::AssetLocator& locator,
    std::function<void(core::SctNavigationTarget)> navigate, QWidget* parent);
void showSctSequenceEditor(SctDocumentController& controller, const core::AssetLocator& locator,
    std::function<void(core::SctNavigationTarget)> navigate, QWidget* parent);
}
