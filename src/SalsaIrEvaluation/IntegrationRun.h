#pragma once
#include "Integration.h"
#include "Connectivity.h"
#include <SalsaCore/Foundation/Hashing.h>

namespace salsa::evaluation {
struct IntegrationCancelled : std::runtime_error { IntegrationCancelled():std::runtime_error("Integration cancelled at phase boundary"){} };
inline std::string integrationHash(std::string_view value) {
    auto digest=core::sha256(std::as_bytes(std::span(value.data(),value.size())));
    if(!digest) throw std::runtime_error(digest.diagnostics().front().message); return digest.value().toHex();
}
template<class Storage> void runIntegration(const ProgramHost<Storage>& host,Json& report,Controls controls,std::string_view persistence,bool sharedOrder) {
    auto& m=report["integration"]; m=Json::object();
    const auto stop=[&](){ if(controls.stopRequested()) throw IntegrationCancelled{}; };
    const auto timed=[&](const char* phase,auto&& action) {
        stop(); report["phase"]=phase; const auto start=Clock::now(); auto result=action();
        m[phase]=std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count()); stop(); return result;
    };
    auto saved=timed("checkpoint_capture_ns",[&](){return host.checkpoint();});
    const auto captured=host.capture(); NativeOutput output;
    const auto native=std::ranges::any_of(saved.scripts,[](const Script& s){return s.native.has_value();});
    if(native) {
        output=timed("fresh_native_build_validate_export_ns",[&](){return exportNative(saved);});
        m["native_output_mapping_count"]=std::to_string(output.locations.size());
        m["after_native_output_memory"]=processMemory();
    }
    if(persistence!="none") {
        const bool binary=persistence=="binary";
        m["payload_format"]=binary?"binary-tables-v1":"json-v1";
        std::vector<std::byte> packed;
        if(binary) {
            auto bytes=timed("binary_table_encode_ns",[&](){return encodeBinaryProgram(saved);});
            m["semantic_bytes"]=std::to_string(bytes.size());
            packed=timed("compress_pack_checksum_ns",[&](){return packBinaryProgram(std::move(bytes));});
        } else {
            auto json=timed("semantic_json_encode_ns",[&](){return encodeProgram(saved);});
            auto text=timed("semantic_json_dump_ns",[&](){return json.dump();}); json=Json{};
            m["semantic_bytes"]=std::to_string(text.size());
            packed=timed("compress_pack_checksum_ns",[&](){return packProgram(text);});
        }
        m["container_bytes"]=std::to_string(packed.size()); m["after_compression_memory"]=processMemory();
        stop(); report["phase"]="container-file-io"; auto read=fileRoundTrip(packed,m); stop(); std::vector<std::byte>().swap(packed);
        const auto reloadStart=Clock::now();
        Context reloadContext; ProgramHost<Storage> reloaded(reloadContext,sharedOrder);
        if(binary) {
            auto bytes=timed("unpack_decompress_checksum_ns",[&](){return unpackBinaryProgram(read);}); std::vector<std::byte>().swap(read);
            auto view=timed("binary_table_view_ns",[&](){return viewBinaryProgram(bytes);});
            timed("model_validation_and_load_ns",[&](){auto loaded=reloaded.load(view); if(!loaded) throw std::runtime_error(loaded.diagnostics().front().message); return true;});
            m["load_input_policy"]="borrowed binary rows/order/identity roles; cold metadata only; no JSON or temporary Fixture";
        } else {
            auto unpacked=timed("unpack_decompress_checksum_ns",[&](){return unpackProgram(read);}); std::vector<std::byte>().swap(read);
            auto decodedJson=timed("semantic_json_parse_ns",[&](){return Json::parse(unpacked);}); std::string().swap(unpacked);
            auto decoded=timed("semantic_model_decode_ns",[&](){return decodeProgram(decodedJson);}); decodedJson=Json{};
            timed("model_validation_and_load_ns",[&](){auto loaded=reloaded.load(decoded); if(!loaded) throw std::runtime_error(loaded.diagnostics().front().message); return true;});
            m["load_input_policy"]="JSON tree then temporary Fixture then host";
        } // Release every borrowed buffer before derived queries or editing.
        m["reload_shared_validation_values_order_ns"]=std::to_string(reloaded.loadPhases()[0]);
        m["reload_candidate_storage_ns"]=std::to_string(reloaded.loadPhases()[1]);
        m["reload_direct_index_ns"]=std::to_string(reloaded.loadPhases()[2]);
        StoryConnectivity rebuilt(reloadContext);
        timed("derived_rebuild_ns",[&](){rebuilt.load(reloaded); auto ready=rebuilt.prepare(reloaded); expect(rebuilt.install(reloaded,std::move(ready)),"Reloaded derived state became stale"); return true;});
        m["reload_to_ready_ns"]=std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-reloadStart).count());
        m["after_reload_memory"]=processMemory();
        for(size_t i=0;i<reloadContext.memory.size();++i) {
            const auto& a=reloadContext.memory[i].totals;
            m["reload_pmr"][DomainNames[i]]={{"current_bytes",std::to_string(a.current)},{"peak_bytes",std::to_string(a.peak)},{"allocations",std::to_string(a.allocations)}};
        }
        // Final observation after timed work; no extra validation pass before edits.
        const auto expectedHash=integrationHash(encodeProgram(saved).dump());
        auto restored=reloaded.checkpoint();
        expect(integrationHash(encodeProgram(restored).dump())==expectedHash,"Container reload changed identities, values, metadata or allocator state");
        expect(restored.checkpoint->revision==saved.checkpoint->revision && reloaded.capture().instance!=captured.instance,"Reload revision/instance contract failed");
        m["checkpoint_sha256"]=expectedHash;
        if(native) {
            auto reexported=exportNative(restored); expect(reexported.files.size()==output.files.size(),"Reloaded native script count changed");
            for(size_t i=0;i<output.files.size();++i) expect(reexported.files[i].bytes==output.files[i].bytes,"Container reload changed fresh native output");
        }
        m["derived_cache_policy"]="rebuild; no serialized postings or module summaries";
        m["history_policy"]="not saved; revision and identity high-water retained";
        m["file_io_scope"]="temporary file write/close and immediate read; OS cache uncontrolled, no durability/transaction claim";
    }
    if(native) {
        // Inspect freshly generated bytes, never infer successful native coverage from fixture names.
        const auto start=Clock::now(); auto reparsed=importNative(output.files,saved.project);
        const auto meaning=nativeMeaning(saved);
        expect(nativeMeaning(reparsed.fixture)==meaning,"Fresh SCT changed the supported semantic program");
        m["output_observation_ns"]=std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count());
        m["native_meaning_sha256"]=integrationHash(meaning);
    }
    expect(host.accepts(captured),"Integration result belongs to a stale host revision"); stop();
    m["after_observation_memory"]=processMemory();
}
} // namespace salsa::evaluation
