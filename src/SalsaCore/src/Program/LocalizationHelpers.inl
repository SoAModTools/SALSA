// Localization recognition uses authored names and existing structured IR only.
namespace {
using LocalizationLookup = std::unordered_map<std::uint64_t, const SctLogicalText *>;
LocalizationLookup localizationLookup(const ProgramData &d) {
    LocalizationLookup out;
    for (const auto &m : d.metadata.logicalTexts)
        for (auto id : m.variants) {
            auto [at, inserted] = out.emplace(id.value, &m);
            if (!inserted)
                at->second = nullptr;
        }
    return out;
}
const SctLogicalText *logicalFor(const ProgramData &d, SctTextResourceId id,
                                 const LocalizationLookup *lookup = nullptr) {
    if (lookup) {
        auto found = lookup->find(id.value);
        return found == lookup->end() ? nullptr : found->second;
    }
    const SctLogicalText *found = nullptr;
    for (const auto &m : d.metadata.logicalTexts)
        if (std::ranges::find(m.variants, id) != m.variants.end()) {
            if (found)
                return nullptr;
            found = &m;
        }
    return found;
}
std::optional<std::uint32_t> languageValue(ProgramData &d, ConditionId id) {
    auto *c = d.lookup(id.value, EntityKind::Condition, d.conditions);
    if (!c || !d.sequence(c->preparation)->order->empty())
        return {};
    ValueBuffer buffer;
    CopyValues copy{d, buffer};
    auto root = copy.copy(c->result);
    auto e = ProgramValues::expression(buffer, root);
    if (!e)
        return {};
    auto text = SctExpressionLanguage::project(e.value()).text;
    std::smatch m;
    static const std::regex compare(R"(^\(?ByteVar\[87\] == ([0-9]+)(?:\.0+f?)?\)?$)");
    if (!std::regex_match(text, m, compare))
        return {};
    auto n = std::stoul(m[1]);
    if (n > 255)
        return {};
    return static_cast<std::uint32_t>(n);
}
struct LocalizationBranch {
    SctActionId owner;
    Arm arm;
    std::uint32_t selector;
    SctLanguageGroupId language;
};
struct LocalizationRecognition {
    SctActionId header;
    bool sw = false, independent = false, refresh = true, single = false;
    std::vector<LocalizationBranch> branches;
    std::optional<Arm> emptyDefault;
    std::string reason;
};
std::optional<LocalizationRecognition> recognizeLocalization(ProgramData &d, SctActionId id,
                                                             const LocalizationLookup *lookup = nullptr) {
    auto *a = d.action(id);
    if (a && a->kind == ActionKind::Instruction) {
        if (!incoming(d, id).empty())
            return {};
        auto sequence = a->owner;
        while (auto *parent = d.sequence(sequence)) {
            if (std::ranges::any_of(d.metadata.localization.blocks,
                                    [&](auto &b) { return b.body == sequence; }))
                return {};
            if (!parent->owner)
                break;
            auto *owner = d.action(d.parentAction(*parent));
            if (!owner)
                break;
            if (owner->kind == ActionKind::Switch)
                return {};
            if (owner->kind == ActionKind::Conditional)
                for (std::uint32_t n = 0; n < owner->armCount; ++n)
                    if (languageValue(d, d.arms[owner->firstArm + n].condition))
                        return {};
            sequence = owner->owner;
        }
        std::optional<SctLanguageGroupId> language;
        bool valid = true;
        featureOperands(d, *a, [&](auto, ValueId value) {
            auto *v = d.values.find(value.value);
            if (v->kind != ValueKind::IndexedTextReference)
                return;
            auto *text = d.lookup(v->data, EntityKind::Text, d.texts);
            auto *message = logicalFor(d, SctTextResourceId{v->data}, lookup);
            if (!text || text->languages.size() != 1 || !message || message->variants.size() != 1) {
                valid = false;
                return;
            }
            if (language && *language != text->languages.front())
                valid = false;
            language = text->languages.front();
        });
        if (!valid || !language)
            return {};
        LocalizationRecognition r;
        r.header = id;
        r.single = true;
        r.branches.push_back({id, {}, 0, *language});
        r.reason = "Indexed message with legacy-linked variants; keep one editable operation and generate "
                   "runtime selection when multiple output languages are included.";
        return r;
    }
    if (!a || (a->kind != ActionKind::Conditional && a->kind != ActionKind::Switch))
        return {};
    if (std::ranges::find(d.metadata.localization.blocks, id, &editor::LocalizedBlock::action) !=
        d.metadata.localization.blocks.end())
        return {};
    LocalizationRecognition r;
    r.header = id;
    r.sw = a->kind == ActionKind::Switch;
    if (r.sw) {
        auto *c = d.lookup(d.switches[a->detail].selector.value, EntityKind::Condition, d.conditions);
        if (!c || !d.sequence(c->preparation)->order->empty())
            return {};
        ValueBuffer b;
        CopyValues cp{d, b};
        auto v = cp.copy(c->result);
        auto e = ProgramValues::expression(b, v);
        if (!e || SctExpressionLanguage::project(e.value()).text != "ByteVar[87]")
            return {};
        r.refresh = c->skipRefresh;
    }
    auto gather = [&](const Action &owner) -> bool {
        for (std::uint32_t i = 0; i < owner.armCount; ++i) {
            auto arm = d.arms[owner.firstArm + i];
            std::optional<std::uint32_t> selector;
            if (r.sw) {
                if (arm.includesDefault) {
                    if (arm.labelCount || arm.caseExit != CaseExit::ExitSwitch ||
                        !d.sequence(arm.body)->order->empty())
                        return false;
                    r.emptyDefault = arm;
                    continue;
                }
                if (arm.labelCount != 1 || arm.caseExit != CaseExit::ExitSwitch)
                    return false;
                auto n = d.caseLabels[arm.firstLabel];
                if (n < 0 || n > 255)
                    return false;
                selector = static_cast<std::uint32_t>(n);
            } else {
                selector = languageValue(d, arm.condition);
                if (!selector)
                    return false;
                auto refresh =
                    d.lookup(arm.condition.value, EntityKind::Condition, d.conditions)->skipRefresh;
                if (r.branches.empty())
                    r.refresh = refresh;
                else if (r.refresh != refresh)
                    return false;
            }
            r.branches.push_back({owner.id, arm, *selector, {}});
        }
        return true;
    };
    if (!gather(*a))
        return {};
    if (!r.sw && a->armCount == 1) {
        r.independent = true;
        auto &order = *d.sequence(a->owner)->order;
        auto at = std::ranges::find(order, id);
        // Do not organize just a suffix of a differing language block.
        auto previous = at;
        bool predecessor = false, repeated = false;
        while (previous != order.begin()) {
            auto *before = d.action(*--previous);
            if (before->kind != ActionKind::Conditional || before->armCount != 1)
                break;
            auto selector = languageValue(d, d.arms[before->firstArm].condition);
            if (!selector)
                break;
            predecessor = true;
            if (*selector == r.branches.front().selector) {
                repeated = true;
                break;
            }
        }
        if (predecessor && !repeated)
            return {};
        for (++at; at != order.end(); ++at) {
            auto *next = d.action(*at);
            if (next->kind != ActionKind::Conditional || next->armCount != 1 ||
                !languageValue(d, d.arms[next->firstArm].condition))
                break;
            auto selector = languageValue(d, d.arms[next->firstArm].condition);
            if (std::ranges::any_of(r.branches, [&](auto &b) { return b.selector == selector; }))
                break;
            if (!gather(*next))
                return {};
        }
    }
    if (r.branches.size() < 2)
        return {};
    std::set<std::uint32_t> values;
    std::vector<std::uint64_t> first;
    for (auto &branch : r.branches) {
        if (!values.insert(branch.selector).second)
            return {};
        auto *seq = d.sequence(branch.arm.body);
        if (seq->order->empty())
            return {};
        std::set<SctActionId> members(seq->order->begin(), seq->order->end());
        std::vector<std::uint64_t> signature;
        std::optional<SctLanguageGroupId> lang;
        bool foundText = false;
        std::function<bool(ValueId)> value = [&](ValueId id) {
            auto *v = d.values.find(id.value);
            signature.insert(signature.end(), {static_cast<std::uint64_t>(v->kind), v->detail, v->count});
            if (v->kind == ValueKind::IndexedTextReference) {
                auto *text = d.lookup(v->data, EntityKind::Text, d.texts);
                auto *message = logicalFor(d, SctTextResourceId{v->data}, lookup);
                if (!text || !message || text->languages.size() != 1)
                    return false;
                if (lang && *lang != text->languages.front())
                    return false;
                lang = text->languages.front();
                foundText = true;
                signature.push_back(message->id.value);
            } else if (v->kind == ValueKind::SupplementaryTextReference) {
                const auto *text = d.lookup(v->data, EntityKind::Text, d.texts);
                if (!text || d.values.find(text->value.value)->kind == ValueKind::Unavailable) return false;
                signature.push_back(static_cast<std::uint64_t>(text->kind));
                if (!value(text->value)) return false;
            } else if (v->kind == ValueKind::PlainText || v->kind == ValueKind::TextChunk) {
                for (std::uint32_t i = 0; i < v->detail; ++i)
                    signature.push_back(static_cast<unsigned char>(d.characters.at(v->data + i)));
            } else
                signature.push_back(v->data);
            for (auto c : d.child(id))
                if (!value(c))
                    return false;
            return true;
        };
        for (auto action : *seq->order) {
            auto *op = d.action(action);
            if (op->kind == ActionKind::Transfer) {
                auto tr = d.transfers[op->detail];
                if (tr.kind != TransferKind::Break || tr.target != branch.owner ||
                    action != seq->order->back())
                    return {};
                signature.insert(signature.end(), {UINT64_MAX, tr.skipRefresh});
                continue;
            }
            if (op->kind != ActionKind::Instruction)
                return {};
            for (const auto &use : incoming(d, action))
                if (!members.contains(use.site.action))
                    return {}; // preserve external entry points
            bool bad = false;
            featureOperands(d, *op, [&](auto, ValueId v) {
                auto *p = d.values.find(v.value);
                if (p->kind == ValueKind::ActionReference && members.contains(SctActionId{p->data}))
                    bad = true;
            });
            if (bad)
                return {}; // internal branch topology needs a later recognizer
            if (!value(op->value))
                return {};
        }
        if (!foundText || !lang)
            return {};
        branch.language = *lang;
        if (first.empty())
            first = signature;
        else if (first != signature)
            return {};
    }
    std::set<SctLanguageGroupId> languages;
    for (const auto &b : r.branches)
        if (!languages.insert(b.language).second)
            return {};
    r.reason = "Equivalent instruction sequences with legacy-linked text variants and explicit ByteVar[87] "
               "selection; original selector sampling and refresh retained.";
    return r;
}
} // namespace
