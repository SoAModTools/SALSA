#pragma once
#include "SctDocumentController.h"
namespace salsa::qt {
void showSctEntrypointEditor(SctDocumentController&, const core::AssetLocator&,
    const core::GameProjectContext&, QWidget*);
}
