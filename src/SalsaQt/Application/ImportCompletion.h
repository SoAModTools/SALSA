#pragma once
namespace salsa::qt {
class ProgramOperation;
class EditorSessionPresenter;
// Post-import choice and whole-project organization review within the same gate.
void offerImportCompletion(ProgramOperation &, EditorSessionPresenter *);
}
