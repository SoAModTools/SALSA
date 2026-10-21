#pragma once
#include "Application/ProgramOperation.h"
#include "Legacy/LegacyConversionDialog.h"

namespace salsa::qt {
// Compose both import paths into one dialog/gate lifetime, including editor presentation.
class ImportOperation final : public ExclusiveOperationController {
  public:
    ImportOperation(std::unique_ptr<ProgramOperation> native, LegacyImportController::Completion converted,
                    bool legacy = false)
        : native_(std::move(native)), legacy_(std::make_unique<LegacyImportController>(std::move(converted))),
          startLegacy_(legacy) {
        forward(*native_, "native/");
        forward(*legacy_, "legacy/");
    }
    QString title() const override { return tr("Import Project"); }
    core::ExclusiveOperationFlowDefinition flowDefinition() const override {
        core::ExclusiveOperationFlowDefinition result;
        result.entry = startLegacy_ ? "legacy/configure" : "native/configure";
        auto append = [&](const auto &child, const std::string &prefix) {
            auto flow = child.flowDefinition();
            for (auto page : flow.pages) {
                page.id = prefix + page.id;
                result.pages.push_back(std::move(page));
            }
            for (auto edge : flow.edges) {
                edge.id = prefix + edge.id;
                edge.source = prefix + edge.source;
                edge.event = prefix + edge.event;
                edge.destination = prefix + edge.destination;
                if (edge.id == "legacy/committed")
                    edge.destination = "native/working";
                result.edges.push_back(std::move(edge));
            }
        };
        append(*native_, "native/");
        append(*legacy_, "legacy/");
        result.edges.push_back({"choose-legacy", "native/configure", "choose-legacy", "legacy/configure"});
        result.edges.push_back({"choose-native", "legacy/configure", "choose-native", "native/configure"});
        return result;
    }
    QWidget *createPage(std::string_view id, QWidget *parent) override {
        return child(id).createPage(local(id), parent);
    }
    void pageEntered(std::string_view id) override {
        active_ = &child(id);
        setCancellable(active_->cancellable());
        setFinishing(active_->finishing());
        active_->pageEntered(local(id));
    }
    std::optional<ExclusiveOperationAction> actionForEdge(std::string_view id) const override {
        if (id == "choose-legacy")
            return ExclusiveOperationAction{tr("Import v7 project…"), ExclusiveOperationActionRole::Secondary};
        if (id == "choose-native")
            return ExclusiveOperationAction{tr("Use extraction folder…"), ExclusiveOperationActionRole::Secondary};
        return child(id).actionForEdge(local(id));
    }
    bool edgeEnabled(std::string_view id) const override {
        return id.starts_with("choose-") || child(id).edgeEnabled(local(id));
    }
    void handleEvent(std::string_view event) override {
        if (event.starts_with("choose-"))
            raiseEvent(event);
        else
            child(event).handleEvent(local(event));
    }
    void requestCancel() override {
        if (active_)
            active_->requestCancel();
    }

  private:
    ExclusiveOperationController &child(std::string_view id) const {
        return id.starts_with("legacy/") ? static_cast<ExclusiveOperationController &>(*legacy_) : *native_;
    }
    static std::string_view local(std::string_view id) { return id.substr(id.find('/') + 1); }
    void forward(ExclusiveOperationController &child, std::string prefix) {
        connect(&child, &ExclusiveOperationController::eventRaised, this,
                [this, prefix](const QString &event) { raiseEvent(prefix + event.toStdString()); });
        connect(&child, &ExclusiveOperationController::presentationChanged, this,
                &ExclusiveOperationController::presentationChanged);
        connect(&child, &ExclusiveOperationController::progressChanged, this,
                &ExclusiveOperationController::progressChanged);
        connect(&child, &ExclusiveOperationController::diagnosticsChanged, this,
                &ExclusiveOperationController::diagnosticsChanged);
        connect(&child, &ExclusiveOperationController::activityRaised, this,
                &ExclusiveOperationController::activityRaised);
        connect(&child, &ExclusiveOperationController::cancellationChanged, this,
                [this](bool value) { setCancellable(value); });
        connect(&child, &ExclusiveOperationController::finishingChanged, this,
                [this](bool value) { setFinishing(value); });
        connect(&child, &ExclusiveOperationController::dismissalRequested, this,
                &ExclusiveOperationController::dismissalRequested);
    }
    std::unique_ptr<ProgramOperation> native_;
    std::unique_ptr<LegacyImportController> legacy_;
    ExclusiveOperationController *active_ = nullptr;
    bool startLegacy_;
};
} // namespace salsa::qt
