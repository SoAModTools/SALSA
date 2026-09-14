#include "NativeCollection.h"
#include "ProgramInternal.h"
#include <algorithm>
#include <mutex>

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
Result<std::shared_ptr<const Program>> loadNativeCollection(NativeLoadRequest request, std::stop_token stop,
                                                            std::stop_token shutdown, std::function<bool()> current) {
    std::vector<Diagnostic> diagnostics;
    try {
        require(request.project.valid() && !request.items.empty(),
                "Native loading needs a project identity and an imported manifest");
        require(request.workers > 0 && request.workers <= 64 && request.workingByteBudget > 0,
                "Invalid native worker limits");
        require(request.items.size() < UINT32_MAX, "Native manifest exceeds identity partition range");
        std::stop_source combined;
        std::stop_callback cancel(stop, [&] { combined.request_stop(); });
        std::stop_callback close(shutdown, [&] { combined.request_stop(); });
        std::mutex observerMutex;
        std::uint64_t completed = 0;
        auto check = [&] { require(!combined.stop_requested() && current(), "Native load cancelled or superseded"); };
        auto progress = [&](SctLoadingProgress p) {
            std::lock_guard lock(observerMutex);
            p.completed = completed;
            p.total = request.items.size();
            if (request.progress)
                request.progress(p);
        };
        auto d = std::make_unique<ProgramData>();
        d->id = request.project;
        std::size_t next = 0;
        while (next < request.items.size()) {
            check();
            std::uint64_t reserved = 0;
            std::vector<std::future<Result<std::shared_ptr<const Program>>>> jobs;
            // A bounded wave reserves memory before invoking any source reader.
            // Completed jobs retain normalized IR, never a waiting SPICE graph.
            while (next < request.items.size() && jobs.size() < request.workers) {
                auto &item = request.items[next];
                require(item.read && item.workingBytes >= item.baseline.source.byteSize && item.workingBytes > 0 &&
                            item.workingBytes <= request.workingByteBudget,
                        "Missing reader or invalid per-script working-memory reservation");
                if (item.workingBytes > request.workingByteBudget - reserved)
                    break;
                require(!item.baseline.id.valid() && !item.context.id.valid() && !item.context.baseline.valid(),
                        "Native manifest identities are assigned by its ordinal");
                reserved += item.workingBytes;
                auto ordinal = next++;
                auto moved = std::move(item);
                jobs.push_back(std::async(std::launch::async, [&, ordinal, item = std::move(moved)]() mutable {
                    try {
                        check();
                        progress({"Reading SCT", 0, 0, item.context.name});
                        auto bytes = item.read(combined.get_token());
                        if (!bytes)
                            return Result<std::shared_ptr<const Program>>::failure(bytes.diagnostics());
                        check();
                        require(bytes.value().size() == item.baseline.source.byteSize,
                                "Source reader returned a different byte count");
                        const auto first = static_cast<std::uint64_t>(ordinal) << 32;
                        ProgramBuilder builder(request.project, 0, first);
                        auto imported =
                            appendNative(builder, bytes.value(), std::move(item.baseline), std::move(item.context),
                                         item.options, combined.get_token(), progress);
                        if (!imported)
                            return Result<std::shared_ptr<const Program>>::failure(imported.diagnostics());
                        bytes = Result<std::vector<std::uint8_t>>::success({});
                        check();
                        auto built = builder.finish();
                        if (!built)
                            return built;
                        require(built.value()->highWater() - first < UINT32_MAX,
                                "Script exceeds its authored-ID partition");
                        auto ds = imported.diagnostics();
                        ds.insert(ds.end(), built.diagnostics().begin(), built.diagnostics().end());
                        check();
                        return Result<std::shared_ptr<const Program>>::success(std::move(built).takeValue(),
                                                                               std::move(ds));
                    } catch (const std::exception &e) {
                        combined.request_stop();
                        return Result<std::shared_ptr<const Program>>::failure(failure(e.what()));
                    }
                }));
            }
            for (auto &job : jobs) {
                auto result = job.get();
                if (!result) {
                    combined.request_stop();
                    diagnostics.insert(diagnostics.end(), result.diagnostics().begin(), result.diagnostics().end());
                    return Result<std::shared_ptr<const Program>>::failure(std::move(diagnostics));
                }
                check();
                diagnostics.insert(diagnostics.end(), result.diagnostics().begin(), result.diagnostics().end());
                NativeCollection::append(*d, std::move(result).takeValue());
                {
                    std::lock_guard lock(observerMutex);
                    ++completed;
                }
                progress({"Script ready"});
            }
        }
        check();
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
