#pragma once

#include "SalsaCore/Application/ExclusiveOperationFlow.h"

#include <QObject>
#include <QString>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

class QWidget;

namespace salsa::qt {

enum class ExclusiveOperationProgressUnit {
    None,
    Steps,
    Files,
    Scripts,
    Assets,
    Bytes,
};

enum class ExclusiveOperationActionRole {
    Secondary,
    Primary,
    Destructive,
};

enum class ExclusiveOperationActivityOutcome { Completed, Failed, Cancelled };

struct ExclusiveOperationAction final {
    QString label;
    ExclusiveOperationActionRole role = ExclusiveOperationActionRole::Secondary;
};

class ExclusiveOperationController : public QObject {
    Q_OBJECT

public:
    explicit ExclusiveOperationController(QObject* parent = nullptr);
    ~ExclusiveOperationController() override = default;

    [[nodiscard]] virtual QString title() const = 0;
    [[nodiscard]] virtual core::ExclusiveOperationFlowDefinition flowDefinition() const = 0;
    [[nodiscard]] virtual QWidget* createPage(std::string_view pageId, QWidget* parent) = 0;
    [[nodiscard]] virtual std::optional<ExclusiveOperationAction> actionForEdge(
        std::string_view edgeId) const;
    [[nodiscard]] virtual bool edgeEnabled(std::string_view edgeId) const;
    virtual void handleEvent(std::string_view event);
    virtual void pageEntered(std::string_view pageId);
    virtual void requestCancel();

    [[nodiscard]] bool cancellable() const noexcept;
    [[nodiscard]] bool finishing() const noexcept;

signals:
    void eventRaised(const QString& event);
    void presentationChanged();
    void progressChanged(const QString& phase, quint64 completed, quint64 total,
        int unit, const QString& currentItem);
    void diagnosticsChanged(const QString& text);
    void activityRaised(int outcome, const QString& code,
        const QString& message, const QString& location);
    void cancellationChanged(bool cancellable);
    void finishingChanged(bool finishing);
    void dismissalRequested();

protected:
    void raiseEvent(std::string_view event);
    void setCancellable(bool value);
    void setFinishing(bool value);
    void reportProgress(const QString& phase, std::uint64_t completed,
        std::uint64_t total, ExclusiveOperationProgressUnit unit,
        const QString& currentItem = {});
    void reportDiagnostics(const QString& text);
    void reportTerminalActivity(const QString& code, const QString& message,
        const QString& location = {});
    void requestDismissal();
    [[nodiscard]] std::string_view lastRaisedEvent() const noexcept;

private:
    bool cancellable_ = true;
    bool finishing_ = false;
    std::string lastRaisedEvent_{};
};

}  // namespace salsa::qt
