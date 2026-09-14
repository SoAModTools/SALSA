#pragma once
#include "SalsaCore/Program/ProgramValues.h"
#include <QDialog>
class QTreeWidget;
namespace salsa::qt {
class ProgramValueDialog final : public QDialog {
  public:
    using ChooseTarget = std::function<std::optional<core::program::ValueBuffer>(core::program::ValueKind)>;
    ProgramValueDialog(core::program::ValueBuffer, ChooseTarget, QWidget *);
    const core::program::ValueBuffer &value() const { return buffer_; }

  private:
    void rebuild();
    void edit();
    void groups();
    void replace(core::program::ValueId, core::program::ValueBuffer);
    core::program::ValueBuffer buffer_;
    ChooseTarget target_;
    QTreeWidget *tree_;
};
} // namespace salsa::qt
