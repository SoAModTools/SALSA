#include "NativeCollection.h"
#include "ProgramInternal.h"
#include <algorithm>
#include <mutex>
#include <condition_variable>
#include <thread>

namespace salsa::core::program {
struct NativeCollection {
    static void append(ProgramData &d, std::shared_ptr<const Program> program) {
        require(program && program.use_count() == 1, "Collection merge needs exclusive script ownership");
        auto source = std::move(const_cast<Program *>(program.get())->data_);
        auto &s = *source;
        require(s.scripts.size() == 1 && s.metadata.modules.empty() && s.metadata.annotations.empty(),
                "Collection worker did not produce one native script");
        for (auto &b : s.baselines)
            for (auto &existing : d.baselines)
                require(b.importedDocument != existing.importedDocument,
                        "Duplicate imported-document identity in manifest");
        const auto valueBase = d.values.size(), childBase = count32(d.children.size()),
                   characterBase = count32(d.characters.size());
        count32(static_cast<std::size_t>(valueBase) + s.values.size());
        count32(d.children.size() + s.children.size());
        count32(d.characters.size() + s.characters.size());
        auto remap = [&](ValueId id) { return id.valid() ? ValueId{id.value + valueBase} : id; };
        for (std::uint32_t i = 0; i < s.values.size(); ++i) {
            auto v = *s.values.find(i);
            v.first += childBase;
            if (v.kind == ValueKind::PlainText || v.kind == ValueKind::TextChunk)
                v.data += characterBase;
            d.values.append(v);
        }
        for (auto id : s.children)
            d.children.push_back(remap(id));
        d.characters.insert(d.characters.end(), s.characters.begin(), s.characters.end());
        for (auto &b : s.baselines) {
            d.claim(b.id.value, EntityKind::Baseline, count32(d.baselines.size()));
            d.baselines.push_back(std::move(b));
        }
        for (auto &script : s.scripts) {
            d.claim(script.context.id.value, EntityKind::Script, count32(d.scripts.size()));
            d.scripts.push_back(std::move(script));
        }
        for (auto &section : s.sections) {
            section.content = remap(section.content);
            d.claim(section.id.value, EntityKind::Section, count32(d.sections.size()));
            d.sections.push_back(std::move(section));
        }
        for (auto &seq : s.sequences) {
            d.claim(seq.id.value, EntityKind::Sequence, count32(d.sequences.size()));
            d.sequences.push_back(std::move(seq));
        }
        for (auto &text : s.texts) {
            text.value = remap(text.value);
            d.claim(text.id.value, EntityKind::Text, count32(d.texts.size()));
            d.texts.push_back(std::move(text));
        }
        // Native normalization produces instruction leaves; structured recognition
        // remains WP3. Do not silently merge unknown construct-index layouts.
        require(s.arms.empty() && s.conditions.empty() && s.loops.empty() && s.switches.empty() && s.transfers.empty(),
                "Unexpected structured native worker output");
        for (std::uint32_t i = 0; i < s.actions.size(); ++i) {
            auto a = *s.actions.find(i);
            a.value = remap(a.value);
            d.claim(a.id.value, EntityKind::Action, d.actions.size());
            d.actions.append(a);
        }
        for (auto &entry : s.metadata.entrypoints) {
            d.claim(entry.id.value, EntityKind::Entry, count32(d.metadata.entrypoints.size()));
            d.metadata.entrypoints.push_back(std::move(entry));
        }
        for (auto f : s.accesses) {
            f.expression = remap(f.expression);
            d.accesses.push_back(f);
        }
        for (auto f : s.computed) {
            f.expression = remap(f.expression);
            d.computed.push_back(f);
        }
        d.targetUses.insert(d.targetUses.end(), s.targetUses.begin(), s.targetUses.end());
        d.successors.insert(d.successors.end(), s.successors.begin(), s.successors.end());
        d.highWater = std::max(d.highWater, s.highWater);
        d.stats.excludedSourceBytes += s.stats.excludedSourceBytes;
        d.stats.unavailableItems += s.stats.unavailableItems;
    }
    static std::shared_ptr<const Program> finish(std::unique_ptr<ProgramData> d) {
        finalizePostings(*d);
        return std::shared_ptr<const Program>(new Program(std::move(d)));
    }
};
Result<void> NativeDatasetPreparation::append(std::size_t ordinal, const CapturedSource &source,
                                              SctScriptContext context,
                                              SctImportedDocumentId importedDocument,
                                              std::optional<spice::sct::SctKnownTextConvention> convention,
                                              const spice::sct::SctDocumentImportOptions &options,
                                              std::stop_token stop, SctLoadingObserver observer) {
    try {
        require(ordinal < UINT32_MAX, "Native manifest exceeds identity partition range");
        const auto first = static_cast<std::uint64_t>(ordinal) << 32;
        ProgramBuilder builder(project_, 0, first);
        // The fingerprint is deliberately pending inside this detached preparation.
        SctAuthoringBaseline baseline{
            {}, {Sha256Digest({})},       source.descriptor(), std::move(importedDocument), convention,
            {}, {context.platform, false}};
        NativeTextEvidence evidence;
        auto imported = appendNative(builder, source, std::move(baseline), std::move(context), options, stop,
                                     std::move(observer), &evidence);
        if (!imported)
            return Result<void>::failure(imported.diagnostics());
        auto built = builder.finish();
        if (!built)
            return Result<void>::failure(built.diagnostics());
        require(built.value()->highWater() - first < UINT32_MAX, "Script exceeds its authored-ID partition");
        auto diagnostics = imported.diagnostics();
        diagnostics.insert(diagnostics.end(), built.diagnostics().begin(), built.diagnostics().end());
        std::lock_guard lock(mutex_);
        require(!parts_.contains(ordinal), "Duplicate script ordinal in dataset preparation");
        parts_.emplace(ordinal,
                       Part{std::move(built).takeValue(), std::move(evidence), std::move(diagnostics)});
        return Result<void>::success();
    } catch (const std::exception &e) {
        return Result<void>::failure(failure(e.what()));
    }
}
Result<std::shared_ptr<const Program>> NativeDatasetPreparation::finish(const DatasetFingerprint &fingerprint,
                                                                        std::stop_token stop,
                                                                        SctLoadingObserver observer) {
    try {
        auto d = std::make_unique<ProgramData>();
        d->id = project_;
        std::vector<NativeTextEvidence> evidence;
        std::vector<Diagnostic> diagnostics;
        std::lock_guard lock(mutex_);
        std::size_t ordinal = 0;
        for (auto &[index, part] : parts_) {
            require(!stop.stop_requested(), "Dataset import cancelled");
            require(index == ordinal++, "Incomplete dataset preparation");
            NativeCollection::append(*d, std::move(part.program));
            evidence.push_back(std::move(part.text));
            diagnostics.insert(diagnostics.end(), part.diagnostics.begin(), part.diagnostics.end());
        }
        parts_.clear();
        for (auto &baseline : d->baselines)
            baseline.datasetFingerprint = fingerprint;
        if (observer)
            observer({"Resolving dataset text convention"});
        resolveNativeText(*d, evidence, stop);
        if (observer)
            observer({"Building collection postings"});
        auto program = NativeCollection::finish(std::move(d));
        require(!stop.stop_requested(), "Dataset import cancelled");
        return Result<std::shared_ptr<const Program>>::success(std::move(program), std::move(diagnostics));
    } catch (const std::exception &e) {
        return Result<std::shared_ptr<const Program>>::failure(failure(e.what()));
    }
}
Result<std::shared_ptr<const Program>> loadNativeCollection(NativeLoadRequest request, std::stop_token stop,
                                                            std::stop_token shutdown,
                                                            std::function<bool()> current) {
    std::vector<Diagnostic> diagnostics;
    try {
        require(request.project.valid() && !request.items.empty(),
                "Native loading needs a project identity and an imported manifest");
        require(request.workingByteBudget > 0, "Invalid native worker limits");
        require(request.items.size() < UINT32_MAX, "Native manifest exceeds identity partition range");
        std::stop_source combined;
        std::stop_callback cancel(stop, [&] { combined.request_stop(); });
        std::stop_callback close(shutdown, [&] { combined.request_stop(); });
        std::mutex observerMutex;
        std::uint64_t completed = 0;
        auto check = [&] {
            require(!combined.stop_requested() && current(), "Native load cancelled or superseded");
        };
        auto progress = [&](SctLoadingProgress p) {
            std::lock_guard lock(observerMutex);
            p.completed = completed;
            p.total = request.items.size();
            if (request.progress)
                request.progress(p);
        };
        auto d = std::make_unique<ProgramData>();
        d->id = request.project;
        std::vector<NativeTextEvidence> textEvidence(request.items.size());
        for (const auto &item : request.items) {
            require(item.read && item.workingBytes >= item.baseline.source.byteSize &&
                        item.workingBytes > 0 && item.workingBytes <= request.workingByteBudget,
                    "Missing reader or invalid per-script working-memory reservation");
            require(!item.baseline.id.valid() && !item.context.id.valid() && !item.context.baseline.valid(),
                    "Native manifest identities are assigned by its ordinal");
        }
        auto budget = request.concurrency ? request.concurrency : backgroundConcurrency();
        const auto workerCount =
            std::min(request.items.size(),
                     request.workers ? std::min(request.workers, static_cast<std::size_t>(budget->limit()))
                                     : static_cast<std::size_t>(budget->limit()));
        using ScriptResult = Result<std::shared_ptr<const Program>>;
        std::vector<std::optional<ScriptResult>> ready(request.items.size());
        std::mutex queueMutex;
        std::condition_variable_any changed;
        std::size_t next = 0;
        std::uint64_t reserved = 0;
        std::optional<std::vector<Diagnostic>> failed;
        auto run = [&](std::size_t ordinal) -> ScriptResult {
            auto &item = request.items[ordinal];
            try {
                check();
                progress({"Reading SCT", 0, 0, item.context.name});
                const auto first = static_cast<std::uint64_t>(ordinal) << 32;
                ProgramBuilder builder(request.project, 0, first);
                auto imported = [&]() -> Result<SctScriptId> {
                    auto bytes = item.read(combined.get_token());
                    if (!bytes)
                        return Result<SctScriptId>::failure(bytes.diagnostics());
                    check();
                    return appendNative(builder, bytes.value(), std::move(item.baseline),
                                        std::move(item.context), item.options, combined.get_token(), progress,
                                        &textEvidence[ordinal]);
                }();
                if (!imported)
                    return ScriptResult::failure(imported.diagnostics());
                check();
                auto built = builder.finish();
                if (!built)
                    return built;
                require(built.value()->highWater() - first < UINT32_MAX,
                        "Script exceeds its authored-ID partition");
                auto ds = imported.diagnostics();
                ds.insert(ds.end(), built.diagnostics().begin(), built.diagnostics().end());
                return ScriptResult::success(std::move(built).takeValue(), std::move(ds));
            } catch (const std::exception &e) {
                return ScriptResult::failure(failure(e.what()));
            }
        };
        // The stop guard runs before joining threads on every exit, including exceptions.
        // Jobs retain only normalized IR after releasing their source and SPICE graphs.
        std::vector<std::jthread> workers;
        struct StopOnExit {
            std::stop_source &stop;
            ~StopOnExit() { stop.request_stop(); }
        } stopOnExit{combined};
        for (std::size_t worker = 0; worker < workerCount; ++worker)
            workers.emplace_back([&] {
                try {
                    for (;;) {
                        std::size_t ordinal;
                        std::uint64_t weight;
                        {
                            std::unique_lock lock(queueMutex);
                            changed.wait(lock, combined.get_token(), [&] {
                                return next == request.items.size() ||
                                       request.items[next].workingBytes <=
                                           request.workingByteBudget - reserved;
                            });
                            if (combined.stop_requested() || next == request.items.size())
                                return;
                            ordinal = next++;
                            weight = request.items[ordinal].workingBytes;
                            reserved += weight;
                        }
                        auto permit = budget->acquire(combined.get_token());
                        if (!permit)
                            return;
                        auto result = run(ordinal);
                        permit.reset();
                        const bool ok = static_cast<bool>(result);
                        {
                            std::lock_guard lock(queueMutex);
                            reserved -= weight;
                            if (!ok && !failed)
                                failed = result.diagnostics();
                            ready[ordinal].emplace(std::move(result));
                        }
                        if (!ok)
                            combined.request_stop();
                        changed.notify_all();
                        if (!ok)
                            return;
                        {
                            std::lock_guard lock(observerMutex);
                            ++completed;
                        }
                        progress({"Script ready"});
                    }
                } catch (const std::exception &e) {
                    {
                        std::lock_guard lock(queueMutex);
                        if (!failed)
                            failed = std::vector<Diagnostic>{failure(e.what())};
                    }
                    combined.request_stop();
                    changed.notify_all();
                }
            });
        for (std::size_t ordinal = 0; ordinal < request.items.size(); ++ordinal) {
            std::unique_lock lock(queueMutex);
            changed.wait(lock, combined.get_token(),
                         [&] { return ready[ordinal].has_value() || failed.has_value(); });
            if (failed) {
                diagnostics.insert(diagnostics.end(), failed->begin(), failed->end());
                return ScriptResult::failure(std::move(diagnostics));
            }
            check();
            auto result = std::move(*ready[ordinal]);
            ready[ordinal].reset();
            lock.unlock();
            diagnostics.insert(diagnostics.end(), result.diagnostics().begin(), result.diagnostics().end());
            NativeCollection::append(*d, std::move(result).takeValue());
        }
        workers.clear();
        check();
        if (hasErrors(diagnostics))
            return Result<std::shared_ptr<const Program>>::failure(std::move(diagnostics));
        progress({"Resolving dataset text convention"});
        resolveNativeText(*d, textEvidence, combined.get_token());
        textEvidence.clear();
        progress({"Building collection postings"});
        auto result = NativeCollection::finish(std::move(d));
        check();
        progress({"Collection prepared"});
        return Result<std::shared_ptr<const Program>>::success(std::move(result), std::move(diagnostics));
    } catch (const std::exception &e) {
        diagnostics.push_back(failure(e.what()));
        return Result<std::shared_ptr<const Program>>::failure(std::move(diagnostics));
    }
}
} // namespace salsa::core::program
