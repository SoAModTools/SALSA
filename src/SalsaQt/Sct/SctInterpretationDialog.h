#pragma once
#include "SctDocumentController.h"
#include <functional>
namespace salsa::qt {
void showSctInterpretationEditor(SctDocumentController &, const core::AssetLocator &,
                                 std::function<void(core::AssetLocator, core::SctNavigationTarget)>, QWidget *);
}
