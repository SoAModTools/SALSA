#pragma once
#include "SctDocumentController.h"
#include <functional>
namespace salsa::qt {
void showSctRouteEditor(SctDocumentController&, QWidget*);
void showSctStoryStateEditor(SctDocumentController &, const core::AssetLocator &,
    std::function<void(core::AssetLocator, core::SctNavigationTarget)>, QWidget *);
void showSctInterpretationEditor(SctDocumentController &, const core::AssetLocator &,
                                 std::function<void(core::AssetLocator, core::SctNavigationTarget)>, QWidget *);
}
