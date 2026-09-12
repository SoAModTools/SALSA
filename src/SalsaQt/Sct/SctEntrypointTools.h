#pragma once
#include "SctDocumentController.h"
class QWidget;
namespace salsa::qt {
void showSctNamedEntryEditor(SctDocumentController&, core::SctScriptId, QWidget*);
void showSctActivationEditor(SctDocumentController&, core::SctEntrypointId, QWidget*);
void showSctTileSelectorEditor(SctDocumentController&, core::SctScriptId, const core::GameProjectContext&, QWidget*);
}
