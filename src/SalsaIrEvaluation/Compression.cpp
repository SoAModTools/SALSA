#include "Compression.h"
#include "Integration.h"
#include "StorageSupport.h"
#include <SalsaCore/Foundation/Hashing.h>
#include <lodepng.h>
#include <fstream>
#include <set>
#ifdef SALSA_IR_CODECS
#include <zstd.h>
#include <lz4.h>
#include <libdeflate.h>
#endif
namespace salsa::evaluation::compression {
namespace {
std::string digest(std::span<const std::byte> data){auto h=core::sha256(data);expect(h.ok(),"Cannot hash compression input");return h.value().toHex();}
uint64_t elapsed(Clock::time_point start){return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count();}
void outsideRepository(const std::filesystem::path& path){for(auto p=std::filesystem::weakly_canonical(path);!p.empty();){expect(!std::filesystem::exists(p/".git"),"Binary captures must be outside repositories");auto parent=p.parent_path();if(parent==p)break;p=parent;}}
struct Codec {
    std::string name; int level{};
#ifdef SALSA_IR_CODECS
    ZSTD_CCtx* zc{};ZSTD_DCtx* zd{};libdeflate_compressor* dc{};libdeflate_decompressor* dd{};
#endif
    explicit Codec(std::string_view selected):name(selected){
        if(name=="lodepng")return;
#ifdef SALSA_IR_CODECS
        if(name=="zstd-1" || name=="zstd-3"){level=name=="zstd-1"?1:3;zc=ZSTD_createCCtx();zd=ZSTD_createDCtx();if(!zc || !zd){ZSTD_freeCCtx(zc);ZSTD_freeDCtx(zd);throw std::bad_alloc();}return;}
        if(name=="lz4")return;
        if(name=="libdeflate-1" || name=="libdeflate-6"){level=name=="libdeflate-1"?1:6;dc=libdeflate_alloc_compressor(level);dd=libdeflate_alloc_decompressor();if(!dc || !dd){libdeflate_free_compressor(dc);libdeflate_free_decompressor(dd);throw std::bad_alloc();}return;}
#endif
        throw std::runtime_error("Codec unavailable: build optional dependencies with tools/build_ir_codecs.py and rebuild Release");
    }
    ~Codec(){
#ifdef SALSA_IR_CODECS
        ZSTD_freeCCtx(zc);ZSTD_freeDCtx(zd);libdeflate_free_compressor(dc);libdeflate_free_decompressor(dd);
#endif
    }
    Codec(const Codec&)=delete;
    std::string version()const{
        if(name=="lodepng")return LODEPNG_VERSION_STRING;
#ifdef SALSA_IR_CODECS
        if(zc)return ZSTD_versionString();if(name=="lz4")return LZ4_versionString();return LIBDEFLATE_VERSION_STRING;
#else
        return "unavailable";
#endif
    }
    std::vector<unsigned char> compress(std::span<const std::byte> input){
        std::vector<unsigned char> out;
        if(name=="lodepng"){expect(lodepng::compress(out,reinterpret_cast<const unsigned char*>(input.data()),input.size())==0,"LodePNG compression failed");return out;}
#ifdef SALSA_IR_CODECS
        size_t used=0;
        if(zc){out.resize(ZSTD_compressBound(input.size()));used=ZSTD_compressCCtx(zc,out.data(),out.size(),input.data(),input.size(),level);expect(!ZSTD_isError(used),"Zstd compression failed");}
        else if(name=="lz4"){expect(input.size()<=LZ4_MAX_INPUT_SIZE,"LZ4 chunk too large");out.resize(LZ4_compressBound(static_cast<int>(input.size())));auto size=LZ4_compress_default(reinterpret_cast<const char*>(input.data()),reinterpret_cast<char*>(out.data()),static_cast<int>(input.size()),static_cast<int>(out.size()));expect(size>0,"LZ4 compression failed");used=static_cast<size_t>(size);}
        else{out.resize(libdeflate_zlib_compress_bound(dc,input.size()));used=libdeflate_zlib_compress(dc,input.data(),input.size(),out.data(),out.size());expect(used!=0,"libdeflate compression failed");}
        out.resize(used);return out;
#else
        throw std::runtime_error("Codec not compiled");
#endif
    }
    std::vector<unsigned char> decompress(std::span<const unsigned char> input,size_t size){
        std::vector<unsigned char> out;
        if(name=="lodepng"){auto settings=lodepng_default_decompress_settings;settings.max_output_size=std::max<size_t>(size,1);expect(lodepng::decompress(out,input.data(),input.size(),settings)==0 && out.size()==size,"LodePNG decompression failed");return out;}
#ifdef SALSA_IR_CODECS
        out.resize(size);
        if(zd){auto used=ZSTD_decompressDCtx(zd,out.data(),out.size(),input.data(),input.size());expect(!ZSTD_isError(used) && used==size,"Zstd decompression failed");}
        else if(name=="lz4"){expect(size<=LZ4_MAX_INPUT_SIZE && input.size()<=INT_MAX,"LZ4 chunk too large");auto used=LZ4_decompress_safe(reinterpret_cast<const char*>(input.data()),reinterpret_cast<char*>(out.data()),static_cast<int>(input.size()),static_cast<int>(size));expect(used>=0 && static_cast<size_t>(used)==size,"LZ4 decompression failed");}
        else {size_t used=0;expect(libdeflate_zlib_decompress(dd,input.data(),input.size(),out.data(),size,&used)==LIBDEFLATE_SUCCESS && used==size,"libdeflate decompression failed");}
        return out;
#else
        throw std::runtime_error("Codec not compiled");
#endif
    }
};
}
Json capture(const std::filesystem::path& directory,const Tables& tables,const Json& source){
    outsideRepository(directory);expect(std::filesystem::create_directory(directory),"Capture directory must be new");
    Json manifest={{"format","jahorta.salsa.ir-compression-capture"},{"version",1},{"source",source},{"chunks",Json::array()}};
    size_t ordinal=0;constexpr size_t chunkBytes=64u<<20;
    for(const auto& [name,bytes]:tables){size_t offset=0;do{const auto length=std::min(chunkBytes,bytes.size()-offset);auto chunk=bytes.subspan(offset,length);auto file=std::to_string(ordinal++)+".bin";
        std::ofstream out(directory/file,std::ios::binary);out.write(reinterpret_cast<const char*>(chunk.data()),chunk.size());out.close();expect(bool(out),"Cannot write compression capture");
        manifest["chunks"].push_back({{"file",file},{"table",name},{"offset",std::to_string(offset)},{"bytes",std::to_string(length)},{"sha256",digest(chunk)}});offset+=length;}while(offset<bytes.size());}
    auto result=writeNewReport(directory/"capture.json",manifest);expect(result.ok(),"Cannot write capture manifest");return manifest;
}
Json run(const std::filesystem::path& manifestPath,std::string_view selected,Controls controls){
    Json report={{"format","jahorta.salsa.ir-compression-evaluation"},{"version",1},{"complete",false},{"disposition","operation-failed"},{"codec",selected},{"build",buildIdentity()}};
    try{
        expect(report["build"]["configuration"]=="Release","Compression experiment requires Release");
        const auto stop=[&](){if(controls.stopRequested())throw std::runtime_error("Compression experiment cancelled");};
        outsideRepository(manifestPath.parent_path());auto loaded=readJson(manifestPath);expect(loaded.ok(),"Cannot read compression capture");const auto& manifest=loaded.value();
        expect(manifest.at("format")=="jahorta.salsa.ir-compression-capture" && manifest.at("version")==1,"Unsupported capture manifest");
        const auto& chunks=manifest.at("chunks");expect(chunks.is_array() && !chunks.empty() && chunks.size()<=4096,"Invalid capture chunks");
        const auto canonicalManifest=manifest.dump();report["capture_sha256"]=digest(std::as_bytes(std::span(canonicalManifest)));report["source"]=manifest.at("source");report["baseline_memory"]=processMemory();
        std::vector<std::vector<std::byte>> inputs;uint64_t inputBytes=0;std::set<std::string> seen;auto start=Clock::now();
        for(const auto& chunk:chunks){stop();auto name=chunk.at("file").get<std::string>();expect(!name.empty() && name.find_first_not_of("0123456789.bin")==std::string::npos && name.find("..") == std::string::npos && seen.insert(name).second,"Invalid capture file");
            auto path=std::filesystem::canonical(manifestPath.parent_path()/name);expect(path.parent_path()==std::filesystem::canonical(manifestPath.parent_path()),"Capture path escapes directory");
            auto size=std::stoull(chunk.at("bytes").get<std::string>());expect(size<=(64u<<20) && inputBytes+size<=(1u<<30) && std::filesystem::file_size(path)==size,"Invalid capture size");inputBytes+=size;
            std::vector<std::byte> bytes(static_cast<size_t>(size));std::ifstream in(path,std::ios::binary);in.read(reinterpret_cast<char*>(bytes.data()),bytes.size());expect(bool(in) && digest(bytes)==chunk.at("sha256").get<std::string>(),"Changed compression capture");inputs.push_back(std::move(bytes));}
        report["input_load_and_hash_ns"]=std::to_string(elapsed(start));report["input_memory"]=processMemory();report["input_bytes"]=std::to_string(inputBytes);
        start=Clock::now();Codec codec(selected);report["codec_setup_ns"]=std::to_string(elapsed(start));report["codec_version"]=codec.version();
        std::vector<std::vector<unsigned char>> compressed;compressed.reserve(inputs.size());uint64_t compressionNs=0,decompressionNs=0,outputBytes=0;Json samples=Json::array();
        for(size_t i=0;i<inputs.size();++i){stop();start=Clock::now();auto bytes=codec.compress(inputs[i]);auto ns=elapsed(start);compressionNs+=ns;outputBytes+=bytes.size();samples.push_back({{"input_bytes",std::to_string(inputs[i].size())},{"compressed_bytes",std::to_string(bytes.size())},{"compress_ns",std::to_string(ns)}});compressed.push_back(std::move(bytes));}
        uint64_t capacity=0;for(const auto& bytes:compressed)capacity+=bytes.capacity();report["compressed_capacity_bytes"]=std::to_string(capacity);report["compressed_memory"]=processMemory();
        for(size_t i=0;i<inputs.size();++i){stop();start=Clock::now();auto decoded=codec.decompress(compressed[i],inputs[i].size());auto ns=elapsed(start);decompressionNs+=ns;samples[i]["decompress_ns"]=std::to_string(ns);expect(std::ranges::equal(std::as_bytes(std::span(decoded)),std::span(inputs[i])),"Compression roundtrip changed checkpoint bytes");}
        report["final_memory"]=processMemory();report["samples"]=std::move(samples);report["compress_ns"]=std::to_string(compressionNs);report["decompress_ns"]=std::to_string(decompressionNs);report["compressed_bytes"]=std::to_string(outputBytes);
        report["scope"]="Single thread, no dictionary; exact 64 MiB checkpoint chunks; fresh process per codec/repetition. Timed calls include output allocation, exclude input reads/hashes, byte comparison, JSON, SALSA framing and disk writes. Inputs and compressed outputs remain resident; decoded output lives one chunk at a time. Process peaks include allocator retention and transient buffers, not exact codec scratch bytes.";
        report["complete"]=true;report["disposition"]="passed";
    }catch(const std::exception& e){report["error"]=e.what();report["disposition"]=controls.stopRequested()?"cancelled":"operation-failed";}return report;
}
void smoke(){
    std::vector<std::byte> bytes(4096);for(size_t i=0;i<bytes.size();++i)bytes[i]=std::byte((i/16)%13);
    std::vector<std::string> names={"lodepng"};
#ifdef SALSA_IR_CODECS
    names.insert(names.end(),{"zstd-1","zstd-3","lz4","libdeflate-1","libdeflate-6"});
#endif
    for(const auto& name:names){Codec codec(name);for(auto input:{std::span<const std::byte>(bytes),std::span<const std::byte>(bytes).first(0)}){auto packed=codec.compress(input);auto decoded=codec.decompress(packed,input.size());expect(std::ranges::equal(std::as_bytes(std::span(decoded)),input),"Codec smoke changed bytes");}}
}
}
