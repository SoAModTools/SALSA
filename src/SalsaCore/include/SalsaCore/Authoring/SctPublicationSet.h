#pragma once
#include "SalsaCore/Authoring/SctAuthoringSession.h"
#include "SalsaCore/Persistence/WorkspaceTransaction.h"
#include "SalsaCore/Sct/SctPublication.h"

namespace salsa::core {
struct SctPublicationTarget final {
    SctScriptId script;
    std::filesystem::path relativePath;
    SctPublicationOptions options;
    // Host context only; never persisted as authored identity.
    std::optional<std::filesystem::path> originalSource;
    bool allowSourceReplacement = false;
};
struct SctPublicationSetRequest final {
    std::shared_ptr<const SctAuthoringState> captured;
    RevisionId expectedRevision;
    std::filesystem::path root;
    std::vector<SctScriptId> seeds;
    std::vector<SctPublicationTarget> targets;
};
struct SctPublicationSetReceipt final {
    SctAuthoringProjectId project;
    RevisionId revision;
    std::string transactionId;
    std::filesystem::path root;
    std::vector<SctConnectionId> routes;
    struct Output final {
        SctPublicationTarget target;
        Sha256Digest digest;
        SctRealizationMap realization;
    };
    std::vector<Output> outputs;
};
class SctPreparedPublicationSet final {
  public:
    [[nodiscard]] const SctPublicationSetReceipt &review() const {
        return receipt_;
    }
    [[nodiscard]] const SctAuthoringMaterializationResult &materialization() const {
        return outputs_;
    }

  private:
    friend class SctPublicationSet;
    SctPublicationSetReceipt receipt_;
    SctAuthoringMaterializationResult outputs_;
    WorkspaceTransactionRequest transaction_;
};
struct SctPublicationSetResult final {
    std::optional<SctPublicationSetReceipt> receipt;
    WorkspaceTransactionStatus status = WorkspaceTransactionStatus::RecoveryBlocked;
    std::vector<Diagnostic> diagnostics;
    bool cancelled = false;
};
class SctPublicationSet final {
  public:
    // Preparation performs recovery/preflight and encodes everything before installation.
    [[nodiscard]] static Result<SctPreparedPublicationSet> prepare(const SctPublicationSetRequest &,
                                                                   std::stop_token = {});
    [[nodiscard]] static SctPublicationSetResult install(const SctPreparedPublicationSet &, std::stop_token = {},
                                                         const WorkspaceTransactionHooks & = {});
    [[nodiscard]] static std::vector<WorkspaceTransactionResult> recover(const std::filesystem::path &);
    [[nodiscard]] static Result<void> verifyReceipt(const SctPublicationSetReceipt &);
};
} // namespace salsa::core
