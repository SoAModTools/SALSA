#pragma once

#include "SalsaCore/Project/AssetLocator.h"
#include "SalsaCore/Sct/SctSemanticOperation.h"
#include "SpiceSCT/SctDocument.h"

#include <QWidget>

#include <functional>
#include <optional>
#include <vector>

class QLabel;
class QLineEdit;
class QTableWidget;
class QPushButton;

namespace salsa::qt {

class SctScptEditorWidget final : public QWidget {
    Q_OBJECT

public:
    using CommitHandler = std::function<bool(const core::AssetLocator&,
        const spice::sct::SctParameterSite&,
        const spice::sct::SctCanonicalExpression&)>;

    explicit SctScptEditorWidget(QWidget* parent = nullptr);
    void setCommitHandler(CommitHandler handler);
    [[nodiscard]] bool bind(const core::AssetLocator& locator,
        const spice::sct::SctParameterSite& site,
        const spice::sct::SctCanonicalExpression& expression);
    [[nodiscard]] bool prepareToClear();
    void clear();
    [[nodiscard]] const std::optional<core::AssetLocator>& boundLocator() const noexcept {
        return locator_;
    }
    [[nodiscard]] const std::optional<spice::sct::SctParameterSite>&
        boundSite() const noexcept { return site_; }
    void refreshOrMarkStale(const core::AssetLocator& locator,
        const core::SctEditChangeSet& changes,
        std::optional<spice::sct::SctCanonicalExpression> current);

private:
    void refresh();
    void setDirty(bool dirty);
    void convertToConventionalExpression();
    void addOperation(int kind);
    void addOperator(std::uint32_t encodingWord);
    void editSelected();
    void removeSelected();
    void moveSelected(int direction);
    void apply();
    void revert();

    CommitHandler commitHandler_{};
    std::optional<core::AssetLocator> locator_{};
    std::optional<spice::sct::SctParameterSite> site_{};
    std::optional<spice::sct::SctCanonicalExpression> original_{};
    std::vector<spice::sct::SctScptOperation> operations_{};
    spice::sct::SctExpressionTermination termination_ =
        spice::sct::SctExpressionTermination::StopCode;
    bool dirty_ = false;
    bool stale_ = false;
    bool committing_ = false;
    QLabel* heading_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* preview_ = nullptr;
    QLineEdit* expressionEdit_ = nullptr;
    QTableWidget* operationsTable_ = nullptr;
    QPushButton* applyButton_ = nullptr;
    QPushButton* revertButton_ = nullptr;
};

} // namespace salsa::qt
