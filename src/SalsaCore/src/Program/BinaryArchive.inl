// Explicit schema fields; no ABI dumps, reflection library, or JSON tree.
namespace binary {
template <class T> struct Fields;
#define F(T, ...)                                                                                                      \
    template <> struct Fields<T> {                                                                                     \
        static auto tie(T &v) { return std::tie(__VA_ARGS__); }                                                        \
    };
F(Value, v.data, v.first, v.count, v.detail, v.kind)
F(Action, v.id, v.owner, v.kind, v.value, v.firstArm, v.armCount, v.sourceInstruction, v.detail)
F(Arm, v.id, v.owner, v.condition, v.body, v.firstLabel, v.labelCount, v.includesDefault, v.caseExit)
F(ConditionRegion, v.id, v.owner, v.arm, v.preparation, v.result, v.skipRefresh)
F(Loop, v.owner, v.kind, v.condition, v.initialize, v.body, v.update)
F(DispatchSample, v.condition, v.beforeRoute)
F(DispatchRoute, v.arm, v.samples)
F(Dispatch, v.owner, v.mode, v.samples, v.routes)
F(Switch, v.owner, v.selector)
F(Transfer, v.owner, v.target, v.kind, v.skipRefresh)
F(Section, v.id, v.script, v.name, v.body, v.content, v.sourceSection)
F(Text, v.id, v.script, v.value, v.kind, v.supplementary, v.sourceText, v.sourceName, v.sourceGroup,
  v.correspondenceKey, v.languages, v.explicitAssignment)
F(Script, v.context, v.sections, v.supplementaryText)
F(Site, v.action, v.parameter, v.arm, v.operation, v.condition)
F(Module, v.id, v.script, v.name, v.sequences, v.evidence)
F(Entrypoint, v.id, v.script, v.name, v.kind, v.body, v.enabled, v.activation, v.response, v.responseSite, v.predicate,
  v.provider, v.evidence)
F(Annotation, v.subject, v.alias, v.evidence, v.note, v.bookmark, v.color)
F(VariableMeaning, v.id, v.script, v.variable, v.value, v.name, v.evidence, v.storyState)
F(editor::InterpretationRecord, v.subject, v.revision, v.decision, v.explanation, v.evidence)
F(Metadata, v.modules, v.entrypoints, v.ports, v.languages, v.connections, v.logicalTexts, v.annotations, v.meanings, v.folders, v.opcodes, v.narrative, v.interpretations)
F(SctAuthoringEvidence, v.kind, v.description)
F(SctUnresolvedBinding, v.originalEvidence, v.explanation, v.evidence)
F(SctLegacyOrigin, v.capsuleId, v.scriptKey, v.scriptOrdinal)
F(SctAuthoringImportRecipe, v.platform, v.trustSelectedTextEncoding)
F(SctAuthoringBaseline, v.id, v.datasetFingerprint, v.source, v.importedDocument, v.textConvention, v.importEvidence,
  v.recipe)
F(SctAuthoredOutputProfile, v.bigEndian, v.compressed, v.header)
F(SctScriptContext::ResourceAssociation, v.resource, v.role)
F(SctScriptContext, v.id, v.name, v.baseline, v.platform, v.region, v.legacyOrigin, v.contentUses, v.outputProfile,
  v.resources)
F(SctNamedActivation, v.externalName, v.body)
F(SctLocalActivation, v.source, v.operand, v.start, v.continuation, v.lifecycleEntry)
F(Port, v.id, v.owner, v.name, v.action, v.sequence, v.role, v.boundary, v.operand)
F(RouteRequirement, v.site, v.match, v.labels)
F(RoutePath, v.requirements, v.preparation, v.consumption)
F(RouteDefinition, v.method, v.outcome, v.paths, v.entrypoint, v.arrival, v.implementation, v.boundTarget,
  v.targetOperand, v.boundFilename, v.implementationOpcode, v.dispatchArm)
F(SctRouteObservation, v.script, v.action, v.role, v.fingerprint)
F(SctExecutableRoute, v.kind, v.sourceScript, v.destinationScript, v.sourceAction, v.sourceOperand, v.arrival,
  v.arrivalCase, v.sourceStem, v.targetFilename, v.discriminatorBit, v.discriminatorValue, v.observations, v.unresolved)
F(Connection, v.id, v.source, v.destination, v.evidence, v.route, v.cutscene)
F(SctLanguageGroup, v.id, v.name, v.recognitionKey)
F(SctLogicalText, v.id, v.script, v.name, v.variants, v.module)
F(SctVariableKey, v.kind, v.index)
F(SctOpcodeColor, v.opcode, v.colorRgb)
F(SourceRevision, v.digest)
F(DatasetFingerprint, v.digest)
F(AssetDescriptor, v.locator, v.byteSize, v.revision)
F(spice::sct::SctHeaderValues, v.sourceYear, v.sourceMonth, v.sourceDay, v.fourthValue)
F(spice::sct::SctParameterAddress, v.schemaIndex, v.repeatedGroupOrdinal)
F(smores::script::ResourceIdentity, v.dataset, v.asset, v.revision, v.document)
F(smores::script::Entry, v.resource, v.tableIndex, v.localId, v.entryId, v.tableId, v.function, v.parameters)
F(Placement, v.subject, v.x, v.y, v.collapsed)
F(DocumentTab, v.script, v.semanticView, v.selection)
F(editor::SectionFolder, v.script, v.name, v.sections, v.parent)
using ParameterName = std::pair<std::uint32_t, std::string>;
F(ParameterName, v.first, v.second)
using ParameterDefault = std::pair<std::uint32_t, std::uint32_t>;
F(ParameterDefault, v.first, v.second)
F(editor::OpcodeCustomization, v.opcode, v.name, v.description, v.color, v.parameters, v.note, v.category, v.defaults)
F(ValueBuffer, v.values, v.children, v.characters, v.root)
F(Presentation, v.placements, v.selection, v.tabs, v.activeTab)
#undef F
// Enum ordinals are explicit binary schema contracts, independent of enum width.
template <class T> constexpr std::uint32_t enumMax();
#define E(T, L)                                                                                                        \
    template <> constexpr std::uint32_t enumMax<T>() {                                                                 \
        return static_cast<std::uint32_t>(T::L);                                                                       \
    }
E(editor::InterpretationDecision, Superseded)
E(ValueKind, StringGroup)
E(ActionKind, Dispatch)
E(DispatchMode, FirstMatch) E(CaseExit, Fallthrough) E(LoopKind, For) E(TransferKind, Continue)
    E(SequenceRole, LoopUpdate) E(SctEvidenceKind, UserAssertion) E(GamePlatform, Dreamcast) E(GameRegion, Japan)
        E(spice::sct::SctKnownTextConvention, ShiftJis8140) E(SctEntrypointKind, NamedSection) E(SlotRole, Exit)
            E(SlotBoundary, SequenceEnd) E(EntryMethod, NamedEntry) E(RouteOutcome, ScheduledCall)
                E(RequirementMatch, Default) E(SctTransitionKind, ShipBattleExit)
                    E(SctRouteObservationRole, Continuation) E(SctVariableKind, Float)
                        E(spice::sct::SctTextKind, SctString) E(smores::script::ResourceRole, StreamedTile)
#undef E
                            template <class T>
                            T empty() {
    return T{};
}
template <> AssetLocator empty() {
    return AssetLocator::fromRelativePath("placeholder").value();
}
template <> Sha256Digest empty() {
    return Sha256Digest{{}};
}
template <> SourceRevision empty() {
    return {empty<Sha256Digest>()};
}
template <> DatasetFingerprint empty() {
    return {empty<Sha256Digest>()};
}
template <> AssetDescriptor empty() {
    return {empty<AssetLocator>(), 0, empty<SourceRevision>()};
}
template <> SctAuthoringBaseline empty() {
    return {{}, empty<DatasetFingerprint>(), empty<AssetDescriptor>(), {}};
}
template <class T> struct Optional : std::false_type {};
template <class T> struct Optional<std::optional<T>> : std::true_type {
    using Type = T;
};
template <class T> struct Vector : std::false_type {};
template <class T> struct Vector<std::vector<T>> : std::true_type {
    using Type = T;
};
template <class T> struct Variant : std::false_type {};
template <class... T> struct Variant<std::variant<T...>> : std::true_type {};
struct Writer {
    std::function<void(std::span<const std::byte>)> emit;
    void raw(const void *p, std::size_t n) { emit({static_cast<const std::byte *>(p), n}); }
    template <class T> void put(const T &v) {
        if constexpr (std::is_same_v<T, bool>)
            put(std::uint8_t(v));
        else if constexpr (std::is_enum_v<T>) {
            auto n = static_cast<std::uint32_t>(v);
            require(n <= enumMax<T>(), "Invalid binary enum");
            put(n);
        } else if constexpr (std::is_integral_v<T>) {
            using U = std::make_unsigned_t<T>;
            U x = std::bit_cast<U>(v);
            std::array<std::byte, sizeof(T)> b;
            for (auto &c : b) {
                c = std::byte(x & 255);
                x >>= 8;
            }
            raw(b.data(), b.size());
        } else if constexpr (std::is_same_v<T, double>) {
            require(std::isfinite(v), "Nonfinite presentation coordinate");
            put(std::bit_cast<std::uint64_t>(v));
        } else if constexpr (std::is_same_v<T, std::string>) {
            put(count32(v.size()));
            raw(v.data(), v.size());
        } else if constexpr (std::is_same_v<T, AssetLocator>) {
            auto s = v.path().generic_u8string();
            put(std::string(s.begin(), s.end()));
        } else if constexpr (std::is_same_v<T, Sha256Digest>)
            raw(v.bytes().data(), 32);
        else if constexpr (Optional<T>::value) {
            put(v.has_value());
            if (v)
                put(*v);
        } else if constexpr (Vector<T>::value) {
            put(count32(v.size()));
            for (auto &x : v)
                put(x);
        } else if constexpr (Variant<T>::value) {
            put(static_cast<std::uint32_t>(v.index()));
            std::visit([&](const auto &x) { put(x); }, v);
        } else if constexpr (requires { Fields<T>::tie(const_cast<T &>(v)); })
            std::apply([&](const auto &...x) { (put(x), ...); }, Fields<T>::tie(const_cast<T &>(v)));
        else if constexpr (requires { v.value; })
            put(v.value); // UUIDs and typed IDs
        else
            static_assert(sizeof(T) == 0, "Missing binary schema");
    }
};
struct Reader {
    std::function<void(std::span<std::byte>)> consume;
    std::uint64_t remaining;
    std::uint32_t maxItems;
    void raw(void *p, std::size_t n) {
        require(n <= remaining, "Truncated binary record");
        consume({static_cast<std::byte *>(p), n});
        remaining -= n;
    }
    template <class V, std::size_t I = 0> V variant(std::uint32_t tag) {
        if constexpr (I == std::variant_size_v<V>)
            throw std::runtime_error("Unsupported binary variant");
        else {
            if (tag == I)
                return V(std::in_place_index<I>, get<std::variant_alternative_t<I, V>>());
            return variant<V, I + 1>(tag);
        }
    }
    template <class T> T get() {
        if constexpr (std::is_same_v<T, bool>) {
            auto n = get<std::uint8_t>();
            require(n < 2, "Invalid binary boolean");
            return n != 0;
        } else if constexpr (std::is_enum_v<T>) {
            auto n = get<std::uint32_t>();
            require(n <= enumMax<T>(), "Unsupported binary enum");
            return static_cast<T>(n);
        } else if constexpr (std::is_integral_v<T>) {
            using U = std::make_unsigned_t<T>;
            std::array<std::byte, sizeof(T)> b;
            raw(b.data(), b.size());
            U x = 0;
            for (std::size_t i = 0; i < b.size(); ++i)
                x |= static_cast<U>(std::to_integer<unsigned>(b[i])) << (8 * i);
            return std::bit_cast<T>(x);
        } else if constexpr (std::is_same_v<T, double>) {
            auto v = std::bit_cast<double>(get<std::uint64_t>());
            require(std::isfinite(v), "Nonfinite presentation coordinate");
            return v;
        } else if constexpr (std::is_same_v<T, std::string>) {
            auto n = get<std::uint32_t>();
            require(n <= remaining, "Invalid string length");
            std::string s(n, '\0');
            raw(s.data(), n);
            return s;
        } else if constexpr (std::is_same_v<T, AssetLocator>) {
            auto s = get<std::string>();
            auto r = AssetLocator::fromRelativePath(std::filesystem::path(std::u8string(s.begin(), s.end())));
            require(bool(r), "Invalid asset locator");
            return r.value();
        } else if constexpr (std::is_same_v<T, Sha256Digest>) {
            std::array<std::byte, 32> b;
            raw(b.data(), b.size());
            return Sha256Digest(b);
        } else if constexpr (Optional<T>::value) {
            if (get<bool>())
                return get<typename Optional<T>::Type>();
            return {};
        } else if constexpr (Vector<T>::value) {
            auto n = get<std::uint32_t>();
            require(n <= maxItems && n <= remaining, "Invalid collection length");
            T v;
            v.reserve(std::min(n, 4096u));
            for (std::uint32_t i = 0; i < n; ++i)
                v.push_back(get<typename Vector<T>::Type>());
            return v;
        } else if constexpr (Variant<T>::value)
            return variant<T>(get<std::uint32_t>());
        else {
            auto v = empty<T>();
            if constexpr (requires { Fields<T>::tie(v); })
                std::apply([&](auto &...x) { ((x = get<std::remove_cvref_t<decltype(x)>>()), ...); },
                           Fields<T>::tie(v));
            else if constexpr (requires { v.value; })
                v.value = get<decltype(v.value)>();
            else
                static_assert(sizeof(T) == 0, "Missing binary schema");
            return v;
        }
    }
};
} // namespace binary
