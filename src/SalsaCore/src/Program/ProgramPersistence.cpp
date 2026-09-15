#include "SalsaCore/Program/ProgramPersistence.h"
#include "ProgramInternal.h"
#include <zstd.h>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <set>
#include <tuple>

namespace salsa::core::program {
#include "BinaryArchive.inl"
namespace {
constexpr std::uint32_t ChunkBytes = 1u << 20;
void checkStop(std::stop_token stop) {
    require(!stop.stop_requested(), "Program persistence cancelled");
}
void report(const SctLoadingObserver &observer, std::string phase, std::uint64_t n = 0, std::uint64_t total = 0) {
    if (observer)
        observer({std::move(phase), n, total, {}});
}
void zcheck(std::size_t n) {
    if (ZSTD_isError(n))
        throw std::runtime_error(ZSTD_getErrorName(n));
}
struct Frames {
    std::vector<std::byte> bytes, raw, compressed;
    std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> context{ZSTD_createCCtx(), ZSTD_freeCCtx};
    std::stop_token stop;
    std::uint64_t decoded = 0;
    SctLoadingObserver observer;
    explicit Frames(std::stop_token s, SctLoadingObserver o) : stop(s), observer(std::move(o)) {
        require(bool(context), "Cannot allocate zstd compressor");
        raw.reserve(ChunkBytes);
        compressed.resize(ZSTD_compressBound(ChunkBytes));
        binary::Writer w{[&](auto b) { bytes.insert(bytes.end(), b.begin(), b.end()); }};
        w.raw("SALSAPRJ", 8);
        w.put(std::uint32_t{2});
        w.put(std::uint32_t{1});
        w.put(std::uint32_t{6});
        zcheck(ZSTD_CCtx_setParameter(context.get(), ZSTD_c_compressionLevel, 1));
        zcheck(ZSTD_CCtx_setParameter(context.get(), ZSTD_c_checksumFlag, 1));
        zcheck(ZSTD_CCtx_setParameter(context.get(), ZSTD_c_nbWorkers, 0));
    }
    void flush() {
        if (raw.empty())
            return;
        checkStop(stop);
        decoded += raw.size();
        require(decoded <= PersistenceLimits{}.decodedBytes, "Program exceeds decoded limit");
        auto n = ZSTD_compress2(context.get(), compressed.data(), compressed.size(), raw.data(), raw.size());
        zcheck(n);
        binary::Writer w{[&](auto b) { bytes.insert(bytes.end(), b.begin(), b.end()); }};
        w.put(count32(raw.size()));
        w.put(count32(n));
        w.raw(compressed.data(), n);
        raw.clear();
        require(bytes.size() < PersistenceLimits{}.encodedBytes, "Project container exceeds encoded limit");
        report(observer, "Compressing program records", bytes.size());
    }
    void append(std::span<const std::byte> b) {
        while (!b.empty()) {
            auto n = std::min(b.size(), ChunkBytes - raw.size());
            raw.insert(raw.end(), b.begin(), b.begin() + n);
            b = b.subspan(n);
            if (raw.size() == ChunkBytes)
                flush();
        }
    }
    void finish() {
        flush();
        binary::Writer w{[&](auto b) { bytes.insert(bytes.end(), b.begin(), b.end()); }};
        w.put(std::uint32_t{0});
        w.put(std::uint32_t{0});
    }
};
struct Unframes {
    std::span<const std::byte> bytes;
    struct Frame {
        std::size_t offset;
        std::uint32_t raw, compressed;
    };
    std::vector<Frame> frames;
    std::vector<std::byte> current;
    std::size_t frame = 0, pos = 0;
    std::uint64_t total = 0;
    std::stop_token stop;
    SctLoadingObserver observer;
    std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> context{ZSTD_createDCtx(), ZSTD_freeDCtx};
    Unframes(std::span<const std::byte> b, const PersistenceLimits &limits, std::stop_token s, SctLoadingObserver o)
        : bytes(b), stop(s), observer(std::move(o)) {
        require(context && b.size() <= limits.encodedBytes, "Encoded project exceeds limit");
        std::size_t offset = 0;
        binary::Reader r{[&](auto out) {
                             std::memcpy(out.data(), bytes.data() + offset, out.size());
                             offset += out.size();
                         },
                         b.size(), limits.collectionItems};
        char magic[8];
        r.raw(magic, 8);
        require(std::memcmp(magic, "SALSAPRJ", 8) == 0, "Not a SALSA project container");
        require(r.get<std::uint32_t>() == 2, "Unsupported project container version; original file is unchanged");
        require(r.get<std::uint32_t>() == 1, "Unsupported project compression codec");
        require(r.get<std::uint32_t>() == 6, "Unsupported program binary schema");
        for (;;) {
            checkStop(stop);
            auto n = r.get<std::uint32_t>(), c = r.get<std::uint32_t>();
            if (!n) {
                require(!c && !r.remaining, "Malformed project terminator");
                break;
            }
            require(n <= ChunkBytes && c && c <= ZSTD_compressBound(n) && c <= r.remaining,
                    "Invalid compressed record bounds");
            require(n <= limits.decodedBytes - total, "Decoded project exceeds limit");
            total += n;
            frames.push_back({offset, n, c});
            offset += c;
            r.remaining -= c;
        }
        require(total > 0, "Empty program container");
    }
    void read(std::span<std::byte> out) {
        while (!out.empty()) {
            if (pos == current.size()) {
                checkStop(stop);
                require(frame < frames.size(), "Truncated record stream");
                auto f = frames[frame++];
                current.resize(f.raw);
                require(ZSTD_findFrameCompressedSize(bytes.data() + f.offset, f.compressed) == f.compressed,
                        "Invalid zstd frame extent");
                auto n = ZSTD_decompressDCtx(context.get(), current.data(), current.size(), bytes.data() + f.offset,
                                             f.compressed);
                zcheck(n);
                require(n == f.raw, "Wrong decoded record size");
                pos = 0;
                report(observer, "Decompressing program records", frame, frames.size());
            }
            auto n = std::min(out.size(), current.size() - pos);
            std::memcpy(out.data(), current.data() + pos, n);
            pos += n;
            out = out.subspan(n);
        }
    }
};
bool live(const ProgramData &d, std::uint64_t id, EntityKind kind) {
    return id && d.directory.find(id).kind == kind;
}
// Loading checks occur once at the boundary, before any unsafe cross-table use.
void linkLoaded(ProgramData &d, std::vector<Diagnostic> &diagnostics, std::stop_token stop) {
    require(d.id.valid(), "Invalid project identity");
    auto claim = [&](std::uint64_t id, EntityKind k, std::uint32_t i) {
        require(id && id <= d.highWater, "Invalid persisted identity allocator");
        d.directory.insert(id, {k, i});
    };
    for (std::uint32_t i = 0; i < d.baselines.size(); ++i) {
        auto &b = d.baselines[i];
        claim(b.id.value, EntityKind::Baseline, i);
        require(b.importedDocument.valid(), "Invalid imported-document identity");
    }
    std::set<SctImportedDocumentId> documents;
    for (auto &b : d.baselines)
        require(documents.insert(b.importedDocument).second, "Duplicate imported document");
    for (std::uint32_t i = 0; i < d.scripts.size(); ++i) {
        auto &s = d.scripts[i];
        claim(s.context.id.value, EntityKind::Script, i);
        require(live(d, s.context.baseline.value, EntityKind::Baseline) && s.context.contentUses.empty(),
                "Missing baseline or unsupported physical content reference");
    }
    for (std::uint32_t i = 0; i < d.sections.size(); ++i)
        claim(d.sections[i].id.value, EntityKind::Section, i);
    for (std::uint32_t i = 0; i < d.sequences.size(); ++i)
        claim(d.sequences[i].id.value, EntityKind::Sequence, i);
    for (std::uint32_t i = 0; i < d.actions.size(); ++i)
        claim(d.actions.find(i)->id.value, EntityKind::Action, i);
    for (std::uint32_t i = 0; i < d.arms.size(); ++i)
        claim(d.arms[i].id.value, EntityKind::Arm, i);
    for (std::uint32_t i = 0; i < d.conditions.size(); ++i)
        claim(d.conditions[i].id.value, EntityKind::Condition, i);
    for (std::uint32_t i = 0; i < d.texts.size(); ++i)
        claim(d.texts[i].id.value, EntityKind::Text, i);
    std::vector<std::uint8_t> sections(d.sections.size()), ownedSequences(d.sequences.size()),
        ownedActions(d.actions.size()), ownedText(d.texts.size()), checks(d.conditions.size());
    auto once = [&](auto &flags, auto id) {
        auto index = d.directory.find(id.value).index;
        require(index < flags.size(), "Invalid ownership index");
        return !std::exchange(flags[index], 1);
    };
    auto complete = [](const auto &flags) { return std::ranges::all_of(flags, [](auto flag) { return flag != 0; }); };
    auto seq = [&](SctSequenceId id, SctScriptId script, std::optional<SequenceOwner> owner, SequenceRole role) {
        auto *s = d.sequence(id);
        require(s && s->script == script && s->owner == owner && s->role == role && once(ownedSequences, id),
                "Invalid or duplicate sequence ownership");
    };
    for (auto &script : d.scripts) {
        checkStop(stop);
        for (auto id : script.sections) {
            auto *s = d.lookup(id.value, EntityKind::Section, d.sections);
            require(s && s->script == script.context.id && once(sections, id), "Invalid section order/owner");
            if (s->body.valid()) {
                require(!s->content.valid(), "Section has two bodies");
                seq(s->body, s->script, {}, SequenceRole::SectionBody);
            } else {
                auto *v = d.values.find(s->content.value);
                require(v && (v->kind == ValueKind::IndexedSection || v->kind == ValueKind::StringGroup ||
                              v->kind == ValueKind::Unavailable),
                        "Missing section content");
                if (v->kind == ValueKind::IndexedSection) {
                    auto *t = d.lookup(v->data, EntityKind::Text, d.texts);
                    require(t && t->script == s->script && !t->supplementary && once(ownedText, t->id),
                            "Invalid indexed text ownership");
                }
            }
        }
        for (auto id : script.supplementaryText) {
            auto *t = d.lookup(id.value, EntityKind::Text, d.texts);
            require(t && t->script == script.context.id && t->supplementary && once(ownedText, id),
                    "Invalid supplementary text ownership");
        }
    }
    require(complete(sections) && complete(ownedText), "Unowned section/text");
    for (auto &t : d.texts)
        require(d.values.find(t.value.value) != nullptr, "Missing text value");
    auto check = [&](ConditionId id, const Action &a, std::optional<ArmId> arm) {
        auto *c = d.lookup(id.value, EntityKind::Condition, d.conditions);
        require(c && c->owner == a.id && c->arm == arm && once(checks, id), "Invalid condition ownership");
        auto *v = d.values.find(c->result.value);
        require(v && v->kind == ValueKind::Expression, "Missing condition result");
        seq(c->preparation, d.sequence(a.owner)->script, id, SequenceRole::ConditionPreparation);
    };
    for (auto &s : d.sequences) {
        require(live(d, s.script.value, EntityKind::Script) && s.order, "Invalid sequence script/order");
        for (auto id : *s.order) {
            auto *a = d.action(id);
            require(a && a->owner == s.id && once(ownedActions, id), "Invalid action order/owner");
        }
    }
    require(complete(ownedActions), "Unowned action");
    for (std::uint32_t i = 0; i < d.actions.size(); ++i) {
        checkStop(stop);
        auto &a = *d.actions.find(i);
        auto script = d.sequence(a.owner)->script;
        if (a.kind == ActionKind::Instruction) {
            auto *v = d.values.find(a.value.value);
            require(v && v->kind == ValueKind::Instruction, "Missing instruction payload");
        } else {
            require(!a.value.valid(), "Structured action has native payload");
            if (a.kind == ActionKind::Conditional || a.kind == ActionKind::Switch ||
                a.kind == ActionKind::Dispatch) {
                require(a.armCount > 0, "Empty structured arms");
                std::set<std::int32_t> labels;
                bool seenDefault = false;
                if (a.kind == ActionKind::Switch) {
                    auto &sw = d.switches.at(a.detail);
                    require(sw.owner == a.id, "Invalid switch owner");
                    check(sw.selector, a, {});
                }
                for (std::uint32_t j = 0; j < a.armCount; ++j) {
                    auto &arm = d.arms.at(a.firstArm + j);
                    require(arm.owner == a.id, "Invalid arm owner");
                    seq(arm.body, script, arm.id, SequenceRole::ArmBody);
                    if (a.kind == ActionKind::Conditional || a.kind == ActionKind::Dispatch) {
                        require(!arm.labelCount && !arm.includesDefault && arm.caseExit == CaseExit::ExitSwitch,
                                "Conditional has switch labels");
                        if (arm.condition.valid())
                            check(arm.condition, a, arm.id);
                        else
                            require(j && j + 1 == a.armCount, "Invalid Else position");
                    } else {
                        require(!arm.condition.valid() && (arm.labelCount || arm.includesDefault),
                                "Invalid switch arm");
                        for (std::uint32_t k = 0; k < arm.labelCount; ++k)
                            require(labels.insert(d.caseLabels.at(arm.firstLabel + k)).second,
                                    "Duplicate switch label");
                        require(!arm.includesDefault || !seenDefault, "Duplicate Default");
                        seenDefault |= arm.includesDefault;
                        require(j + 1 < a.armCount || arm.caseExit != CaseExit::Fallthrough,
                                "Final case cannot fall through");
                    }
                }
                if (a.kind == ActionKind::Dispatch) {
                    const auto &region = d.dispatches.at(a.detail);
                    require(region.owner == a.id && region.routes.size() == a.armCount,
                            "Invalid dispatch owner/routes");
                    for (const auto &sample : region.samples)
                        check(sample.condition, a, {});
                }
            } else if (a.kind == ActionKind::Loop) {
                auto &l = d.loops.at(a.detail);
                require(l.owner == a.id, "Invalid loop owner");
                check(l.condition, a, {});
                seq(l.body, script, a.id, SequenceRole::LoopBody);
                if (l.kind == LoopKind::For) {
                    seq(l.initialize, script, a.id, SequenceRole::LoopInitialize);
                    seq(l.update, script, a.id, SequenceRole::LoopUpdate);
                } else
                    require(!l.initialize.valid() && !l.update.valid(), "Unexpected loop phases");
            } else {
                auto &t = d.transfers.at(a.detail);
                require(t.owner == a.id && d.action(t.target), "Invalid transfer");
            }
        }
    }
    require(complete(ownedSequences) && complete(checks), "Unowned sequence/condition");
    // Color ownership chains once; deep nesting does not revisit every ancestor.
    std::vector<std::uint8_t> colors(d.sequences.size());
    std::vector<std::uint32_t> chain;
    for (std::uint32_t i = 0; i < d.sequences.size(); ++i) {
        checkStop(stop);
        chain.clear();
        auto index = i;
        while (colors[index] != 2) {
            require(colors[index] != 1, "Cyclic structured ownership");
            colors[index] = 1;
            chain.push_back(index);
            const auto &cur = d.sequences[index];
            if (!cur.owner)
                break;
            auto *parent = d.action(d.parentAction(cur));
            require(parent, "Missing parent action");
            index = d.directory.find(parent->owner.value).index;
        }
        for (auto item : chain)
            colors[item] = 2;
    }
    for (auto &t : d.transfers) {
        auto *target = d.action(t.target);
        require((t.kind == TransferKind::Break &&
                 (target->kind == ActionKind::Switch || target->kind == ActionKind::Loop || target->kind == ActionKind::Conditional)) ||
                    (t.kind == TransferKind::Continue && target->kind == ActionKind::Loop),
                "Wrong transfer target kind");
        auto *cur = d.sequence(d.action(t.owner)->owner);
        bool enclosed = false;
        while (cur->owner) {
            auto id = d.parentAction(*cur);
            if (id == t.target) {
                require(t.kind != TransferKind::Continue || cur->role == SequenceRole::LoopBody,
                        "Continue outside loop body");
                enclosed = true;
                break;
            }
            cur = d.sequence(d.action(id)->owner);
        }
        require(enclosed, "Transfer target is not enclosing");
    }
    auto metadataIds = [&](const auto &table, EntityKind k) {
        for (std::uint32_t i = 0; i < table.size(); ++i)
            claim(table[i].id.value, k, i);
    };
    metadataIds(d.metadata.modules, EntityKind::Module);
    metadataIds(d.metadata.entrypoints, EntityKind::Entry);
    metadataIds(d.metadata.ports, EntityKind::Port);
    metadataIds(d.metadata.languages, EntityKind::Language);
    metadataIds(d.metadata.connections, EntityKind::Connection);
    metadataIds(d.metadata.logicalTexts, EntityKind::LogicalText);
    metadataIds(d.metadata.meanings, EntityKind::Meaning);
    finishMetadata(d, diagnostics, false);
    for (auto &t : d.texts)
        for (auto id : t.languages)
            require(live(d, id.value, EntityKind::Language), "Missing text language");
    buildIndexes(d, diagnostics, false);
    d.successors.resize(d.actions.size());
    for (auto &s : d.sequences)
        for (std::size_t i = 1; i < s.order->size(); ++i)
            d.successors[d.directory.find((*s.order)[i - 1].value).index] = (*s.order)[i];
}
void presentationShape(const ProgramData &d, const Presentation &p) {
    auto subject = [&](const Subject &s) {
        std::visit(
            [&](auto id) {
                using T = decltype(id);
                constexpr auto kind = []() {
                    if constexpr (std::is_same_v<T, SctScriptId>)
                        return EntityKind::Script;
                    else if constexpr (std::is_same_v<T, SectionId>)
                        return EntityKind::Section;
                    else if constexpr (std::is_same_v<T, SctSequenceId>)
                        return EntityKind::Sequence;
                    else if constexpr (std::is_same_v<T, SctActionId>)
                        return EntityKind::Action;
                    else if constexpr (std::is_same_v<T, ArmId>)
                        return EntityKind::Arm;
                    else if constexpr (std::is_same_v<T, ConditionId>)
                        return EntityKind::Condition;
                    else if constexpr (std::is_same_v<T, SctModuleId>)
                        return EntityKind::Module;
                    else if constexpr (std::is_same_v<T, SctEntrypointId>)
                        return EntityKind::Entry;
                    else
                        return EntityKind::Text;
                }();
                require(live(d, id.value, kind), "Missing presentation subject");
            },
            s);
    };
    for (auto &x : p.placements) {
        subject(x.subject);
        require(std::isfinite(x.x) && std::isfinite(x.y), "Invalid position");
    }
    for (auto &x : p.selection)
        subject(x);
    std::set<SctScriptId> tabs;
    for (auto &t : p.tabs) {
        require(live(d, t.script.value, EntityKind::Script) && tabs.insert(t.script).second,
                "Invalid or duplicate saved tab");
        if (t.selection)
            subject(*t.selection);
    }
    require(!p.activeTab || tabs.contains(*p.activeTab), "Active tab is not open");
}
} // namespace

Result<std::vector<std::byte>> ProgramPersistence::encode(const Program &program, const Presentation &presentation,
                                                          std::stop_token stop, const SctLoadingObserver &observer) {
    try {
        const auto &d = *program.data_;
        checkStop(stop);
        presentationShape(d, presentation);
        report(observer, "Preparing binary program tables");
        std::vector<std::uint8_t> marked(d.values.size());
        std::vector<ValueId> remap(d.values.size());
        auto mark = [&](ValueId v) {
            if (v.valid()) {
                require(v.value < marked.size(), "Invalid value root");
                marked[v.value] = 1;
            }
        };
        for (std::uint32_t i = 0; i < d.actions.size(); ++i)
            if (d.actions.find(i)->id.valid())
                mark(d.actions.find(i)->value);
        for (auto &x : d.conditions)
            if (live(d, x.id.value, EntityKind::Condition))
                mark(x.result);
        for (auto &x : d.sections)
            if (live(d, x.id.value, EntityKind::Section))
                mark(x.content);
        for (auto &x : d.texts)
            if (live(d, x.id.value, EntityKind::Text))
                mark(x.value);
        for (std::uint32_t i = d.values.size(); i > 0; --i)
            if (marked[i - 1]) {
                for (auto child : d.child(ValueId{i - 1})) {
                    require(child.value < i - 1, "Non-topological value pool");
                    marked[child.value] = 1;
                }
            }
        std::uint32_t total = 0;
        for (std::uint32_t i = 0; i < marked.size(); ++i)
            if (marked[i])
                remap[i] = ValueId{total++};
        auto value = [&](ValueId v) { return v.valid() ? remap.at(v.value) : v; };
        Frames frames(stop, observer);
        binary::Writer w{[&](auto b) { frames.append(b); }};
        w.put(d.id);
        w.put(d.revision);
        w.put(d.highWater);
        w.put(d.stats.excludedSourceBytes);
        w.put(d.stats.unavailableItems);
        w.put(d.baselines);
        w.put(d.scripts);
        w.put(total);
        for (std::uint32_t i = 0; i < marked.size(); ++i)
            if (marked[i]) {
                auto &v = *d.values.find(i);
                w.put(v.kind);
                if (v.kind == ValueKind::PlainText || v.kind == ValueKind::TextChunk) {
                    auto text = program.text(ValueId{i});
                    w.put(count32(text.size()));
                    w.raw(text.data(), text.size());
                } else {
                    w.put(v.data);
                    w.put(v.detail);
                    auto children = d.child(ValueId{i});
                    w.put(count32(children.size()));
                    for (auto child : children)
                        w.put(value(child));
                }
            }
        auto rows = [&](const auto &table, EntityKind kind, auto write) {
            std::uint32_t n = 0;
            for (auto &x : table)
                if (live(d, x.id.value, kind))
                    ++n;
            w.put(n);
            for (auto &x : table)
                if (live(d, x.id.value, kind))
                    write(x);
        };
        rows(d.sections, EntityKind::Section, [&](auto x) {
            x.content = value(x.content);
            w.put(x);
        });
        rows(d.texts, EntityKind::Text, [&](auto x) {
            x.value = value(x.value);
            w.put(x);
        });
        rows(d.sequences, EntityKind::Sequence, [&](const auto &x) {
            w.put(x.id);
            w.put(x.script);
            w.put(x.owner);
            w.put(x.role);
            w.put(*x.order);
        });
        std::uint32_t actionCount = 0;
        for (std::uint32_t i = 0; i < d.actions.size(); ++i)
            if (d.actions.find(i)->id.valid())
                ++actionCount;
        w.put(actionCount);
        for (std::uint32_t i = 0; i < d.actions.size(); ++i) {
            const auto &a = *d.actions.find(i);
            if (!a.id.valid())
                continue;
            w.put(a.id);
            w.put(a.owner);
            w.put(a.kind);
            w.put(value(a.value));
            w.put(a.sourceInstruction);
            if (a.kind == ActionKind::Conditional || a.kind == ActionKind::Switch ||
                a.kind == ActionKind::Dispatch) {
                w.put(a.armCount);
                for (auto arm : program.arms(a.id)) {
                    auto labels = program.caseLabels(arm.id);
                    arm.firstLabel = 0;
                    w.put(arm);
                    for (auto label : labels)
                        w.put(label);
                }
                if (a.kind == ActionKind::Switch)
                    w.put(d.switches.at(a.detail));
                if (a.kind == ActionKind::Dispatch)
                    w.put(d.dispatches.at(a.detail));
            } else if (a.kind == ActionKind::Loop)
                w.put(d.loops.at(a.detail));
            else if (a.kind == ActionKind::Transfer)
                w.put(d.transfers.at(a.detail));
        }
        rows(d.conditions, EntityKind::Condition, [&](auto x) {
            x.result = value(x.result);
            w.put(x);
        });
        w.put(d.metadata);
        frames.flush(); // Presentation starts in a separate compression record.
        w.put(presentation);
        frames.finish();
        checkStop(stop);
        report(observer, "Program checkpoint encoded", 1, 1);
        return Result<std::vector<std::byte>>::success(std::move(frames.bytes));
    } catch (const std::exception &e) {
        return Result<std::vector<std::byte>>::failure(failure(e.what()));
    }
}
Result<ProjectFile> ProgramPersistence::decode(std::span<const std::byte> bytes, const PersistenceLimits &limits,
                                               std::stop_token stop, const SctLoadingObserver &observer) {
    std::vector<Diagnostic> diagnostics;
    try {
        checkStop(stop);
        Unframes frames(bytes, limits, stop, observer);
        binary::Reader r{[&](auto b) { frames.read(b); }, frames.total, limits.collectionItems};
        auto d = std::make_unique<ProgramData>();
        d->id = r.get<SctAuthoringProjectId>();
        d->revision = r.get<std::uint64_t>();
        d->highWater = r.get<std::uint64_t>();
        d->stats.excludedSourceBytes = r.get<std::uint64_t>();
        d->stats.unavailableItems = r.get<std::uint64_t>();
        d->baselines = r.get<std::vector<SctAuthoringBaseline>>();
        d->scripts = r.get<std::vector<Script>>();
        auto count = [&]() {
            auto n = r.get<std::uint32_t>();
            require(n <= limits.collectionItems && n <= r.remaining, "Invalid table size");
            return n;
        };
        auto n = count();
        for (std::uint32_t i = 0; i < n; ++i) {
            auto kind = r.get<ValueKind>();
            if (kind == ValueKind::PlainText || kind == ValueKind::TextChunk) {
                auto size = r.get<std::uint32_t>();
                require(size <= r.remaining, "Invalid text byte range");
                auto first = count32(d->characters.size());
                count32(d->characters.size() + size);
                d->characters.resize(d->characters.size() + size);
                if (size)
                    r.raw(d->characters.data() + first, size);
                d->add(kind, first, size);
            } else {
                auto data = r.get<std::uint64_t>();
                auto detail = r.get<std::uint32_t>();
                auto size = count();
                require(size <= r.remaining / 4, "Invalid child index range");
                auto first = count32(d->children.size());
                count32(d->children.size() + size);
                for (std::uint32_t j = 0; j < size; ++j) {
                    auto child = r.get<ValueId>();
                    require(child.value < i, "Invalid or forward child index");
                    d->children.push_back(child);
                }
                d->values.append({data, first, size, detail, kind});
            }
        }
        for (std::uint32_t i = 0; i < d->values.size(); ++i) {
            checkStop(stop);
            valueShape(*d, ValueId{i});
        }
        d->sections = r.get<std::vector<Section>>();
        d->texts = r.get<std::vector<Text>>();
        n = count();
        for (std::uint32_t i = 0; i < n; ++i) {
            Sequence s;
            s.id = r.get<SctSequenceId>();
            s.script = r.get<SctScriptId>();
            s.owner = r.get<std::optional<SequenceOwner>>();
            s.role = r.get<SequenceRole>();
            s.order = std::make_shared<const std::vector<SctActionId>>(r.get<std::vector<SctActionId>>());
            d->sequences.push_back(std::move(s));
        }
        n = count();
        for (std::uint32_t i = 0; i < n; ++i) {
            Action a;
            a.id = r.get<SctActionId>();
            a.owner = r.get<SctSequenceId>();
            a.kind = r.get<ActionKind>();
            a.value = r.get<ValueId>();
            a.sourceInstruction = r.get<std::uint64_t>();
            if (a.kind == ActionKind::Conditional || a.kind == ActionKind::Switch ||
                a.kind == ActionKind::Dispatch) {
                a.firstArm = count32(d->arms.size());
                a.armCount = count();
                for (std::uint32_t j = 0; j < a.armCount; ++j) {
                    auto arm = r.get<Arm>();
                    require(arm.firstLabel == 0 && arm.labelCount <= limits.collectionItems &&
                                arm.labelCount <= r.remaining / 4,
                            "Invalid case labels");
                    arm.firstLabel = count32(d->caseLabels.size());
                    for (std::uint32_t k = 0; k < arm.labelCount; ++k)
                        d->caseLabels.push_back(r.get<std::int32_t>());
                    d->arms.push_back(arm);
                }
                if (a.kind == ActionKind::Switch) {
                    a.detail = count32(d->switches.size());
                    d->switches.push_back(r.get<Switch>());
                }
                if (a.kind == ActionKind::Dispatch) {
                    a.detail = count32(d->dispatches.size());
                    d->dispatches.push_back(r.get<Dispatch>());
                }
            } else if (a.kind == ActionKind::Loop) {
                a.detail = count32(d->loops.size());
                d->loops.push_back(r.get<Loop>());
            } else if (a.kind == ActionKind::Transfer) {
                a.detail = count32(d->transfers.size());
                d->transfers.push_back(r.get<Transfer>());
            }
            d->actions.append(a);
        }
        d->conditions = r.get<std::vector<ConditionRegion>>();
        d->metadata = r.get<Metadata>();
        auto presentation = r.get<Presentation>();
        require(!r.remaining, "Unexpected binary program data");
        checkStop(stop);
        report(observer, "Loading program relationships");
        linkLoaded(*d, diagnostics, stop);
        presentationShape(*d, presentation);
        checkStop(stop);
        ProjectFile out{std::shared_ptr<const Program>(new Program(std::move(d))), std::move(presentation)};
        report(observer, "Program project ready", 1, 1);
        return Result<ProjectFile>::success(std::move(out), diagnostics);
    } catch (const std::exception &e) {
        diagnostics.push_back(failure(e.what()));
        return Result<ProjectFile>::failure(diagnostics);
    }
}
Result<ProjectFile> ProgramPersistence::load(const std::filesystem::path &path, const PersistenceLimits &limits,
                                             std::stop_token stop, const SctLoadingObserver &observer) {
    try {
        checkStop(stop);
        report(observer, "Reading program project");
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        require(bool(file), "Cannot open program project");
        auto size = file.tellg();
        require(size >= 0 && static_cast<std::uint64_t>(size) <= limits.encodedBytes, "Invalid project file size");
        std::vector<std::byte> bytes(static_cast<std::size_t>(size));
        file.seekg(0);
        for (std::size_t at = 0; at < bytes.size();) {
            checkStop(stop);
            auto n = std::min<std::size_t>(ChunkBytes, bytes.size() - at);
            require(bool(file.read(reinterpret_cast<char *>(bytes.data() + at), n)), "Truncated project file");
            at += n;
        }
        auto decoded=decode(bytes, limits, stop, observer);
        if (!decoded) return decoded;
        auto digest=sha256(bytes);
        if (!digest) return Result<ProjectFile>::failure(digest.diagnostics());
        decoded.value().encodedDigest=digest.value();
        return decoded;
    } catch (const std::exception &e) {
        return Result<ProjectFile>::failure(failure(e.what()));
    }
}
Result<WorkspaceArtifactMutation> ProgramPersistence::mutation(
    const Checkpoint &checkpoint, const Presentation &presentation, std::filesystem::path relativePath,
    std::optional<std::string> expected, std::stop_token stop, const SctLoadingObserver &observer) {
    if (!checkpoint.program || checkpoint.program->id() != checkpoint.status.current.project ||
        checkpoint.program->revision() != checkpoint.status.current.revision)
        return Result<WorkspaceArtifactMutation>::failure(failure("Invalid checkpoint identity/revision"));
    auto locator = AssetLocator::fromRelativePath(relativePath);
    if (!locator)
        return Result<WorkspaceArtifactMutation>::failure(locator.diagnostics());
    auto encoded = encode(*checkpoint.program, presentation, stop, observer);
    if (!encoded)
        return Result<WorkspaceArtifactMutation>::failure(encoded.diagnostics());
    return Result<WorkspaceArtifactMutation>::success(
        {std::move(relativePath), expected.has_value(), std::move(expected), std::move(encoded).takeValue()});
}

Result<std::vector<std::byte>> ProgramPersistence::encodeFragment(const ProgramFragment &f) {
    try {
        std::vector<std::byte> bytes;
        binary::Writer w{[&](auto b){require(bytes.size()+b.size()<=64u*1024u*1024u,"Fragment exceeds 64 MiB");bytes.insert(bytes.end(),b.begin(),b.end());}};
        w.raw("SALSAFRG",8);w.put(std::uint32_t{1});w.put(f.project);w.put(f.script);w.put(f.roots);
        const auto &s=f.content;w.put(s.actions);w.put(s.sections);w.put(s.arms);w.put(s.caseLabels);w.put(s.conditions);w.put(s.loops);w.put(s.switches);w.put(s.dispatches);w.put(s.transfers);w.put(s.payload);w.put(f.annotations);
        w.put(count32(s.sequences.size()));for(auto &seq:s.sequences){w.put(seq.id);w.put(seq.script);w.put(seq.owner);w.put(seq.role);require(bool(seq.order),"Missing fragment body");w.put(*seq.order);}
        return Result<std::vector<std::byte>>::success(std::move(bytes));
    } catch(const std::exception &e){return Result<std::vector<std::byte>>::failure(failure(e.what()));}
}
Result<std::shared_ptr<const ProgramFragment>> ProgramPersistence::decodeFragment(std::span<const std::byte> bytes) {
    try {
        require(bytes.size()<=64u*1024u*1024u,"Fragment exceeds 64 MiB");std::size_t offset=0;
        binary::Reader r{[&](auto out){std::memcpy(out.data(),bytes.data()+offset,out.size());offset+=out.size();},bytes.size(),1000000};
        char magic[8];r.raw(magic,8);require(std::memcmp(magic,"SALSAFRG",8)==0 && r.get<std::uint32_t>()==1,"Unsupported SALSA fragment");
        auto f=std::make_shared<ProgramFragment>();f->project=r.get<SctAuthoringProjectId>();f->script=r.get<SctScriptId>();f->roots=r.get<std::vector<SctActionId>>();auto &s=f->content;
        s.actions=r.get<std::vector<Action>>();s.sections=r.get<std::vector<Section>>();s.arms=r.get<std::vector<Arm>>();s.caseLabels=r.get<std::vector<std::int32_t>>();s.conditions=r.get<std::vector<ConditionRegion>>();s.loops=r.get<std::vector<Loop>>();s.switches=r.get<std::vector<Switch>>();s.dispatches=r.get<std::vector<Dispatch>>();s.transfers=r.get<std::vector<Transfer>>();s.payload=r.get<ValueBuffer>();f->annotations=r.get<std::vector<Annotation>>();
        auto count=r.get<std::uint32_t>();require(count<=r.maxItems && count<=r.remaining,"Invalid fragment sequence count");
        for(std::uint32_t i=0;i<count;++i){Sequence seq;seq.id=r.get<SctSequenceId>();seq.script=r.get<SctScriptId>();seq.owner=r.get<std::optional<SequenceOwner>>();seq.role=r.get<SequenceRole>();seq.order=std::make_shared<const std::vector<SctActionId>>(r.get<std::vector<SctActionId>>());s.sequences.push_back(std::move(seq));}
        require(!r.remaining && (!f->roots.empty() != !s.sections.empty()),"Malformed fragment roots or trailing data");
        return Result<std::shared_ptr<const ProgramFragment>>::success(std::move(f));
    } catch(const std::exception &e){return Result<std::shared_ptr<const ProgramFragment>>::failure(failure(e.what()));}
}
} // namespace salsa::core::program
