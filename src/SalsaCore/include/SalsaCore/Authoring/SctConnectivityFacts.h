#pragma once
#include "SalsaCore/Authoring/SctInterpretation.h"
#include <nlohmann/json.hpp>
namespace salsa::core {
// Immutable, dataset/document-qualified analysis facts shared by the survey and editor.
// They are derived observations, never authored execution state.
using SctConnectivityJson = nlohmann::json;
SctConnectivityJson sctConnectivityFacts(const spice::sct::SctDocument&, const SctConnectivityJson& identity,
    std::span<const SctConnectivityRoot> roots = {});
SctConnectivityJson sctResourceContext(const spice::sct::SctDocument&);
SctConnectivityJson sctResourceLinks(const SctConnectivityJson& resource, const SctConnectivityJson& script, bool details = false);
}
