#pragma once
#include "Program.h"
#include <SpiceSCT/SctDocument.h>

namespace salsa::evaluation::corpus {
// Flat, typed copies of public SPICE values, not a second SCT parser. Nodes and
// child slots are private immutable payload handles; references carry authored IDs.
enum class Kind:uint32_t { Instruction,Parameter,Group,Word,Expression,Value,Binary,Stack,Inert,Words,
    InstructionRef,StringRef,FooterRef,Unresolved,Gap,Plain,Message,Chunk,Command,Arguments,Empty,Indexed,Marker };
struct Node { uint64_t value{}; uint32_t first{},count{},aux{}; Kind kind{}; auto operator<=>(const Node&) const=default; };
struct Pool {
    std::pmr::vector<Node> nodes; std::pmr::vector<uint32_t> children; std::pmr::vector<char> text;
    explicit Pool(std::pmr::memory_resource* r):nodes(r),children(r),text(r){}
    uint32_t add(Kind kind,uint64_t value=0,uint32_t aux=0,std::span<const uint32_t> child={});
    uint32_t string(Kind kind,std::string_view value);
    std::span<const uint32_t> child(uint32_t index) const;
    uint32_t clone(uint32_t root,Pool& destination) const;
    uint32_t atPath(uint32_t root,std::span<const uint32_t> path) const;
};
struct SectionInfo { SectionId id; ScriptId script; std::string name; std::vector<InstructionId> order; uint32_t content=NoSlot; };
struct TextInfo { uint64_t id{}; ScriptId script; uint32_t root{},kind{}; };
struct Model {
    Pool pool; std::vector<Script> scripts; std::vector<SectionInfo> sections; std::vector<TextInfo> texts;
    std::pmr::vector<StoredRecord> records; uint64_t highWater{},revision{};
    explicit Model(Context& c):pool(c.resource(Domain::Values)),records(c.resource(Domain::Preparation)){}
};
struct Counts { uint64_t instructions{},parameters{},expressions{},texts{},sourceOpaqueBytes{},sourceOpaqueItems{},executionGaps{},unresolved{}; };
Counts appendDocument(Model& model,const spice::sct::SctDocument& document,Script script);
// Reports contain only logical asset keys, counts, digests and numeric edit sites.
Json run(const Json& manifest,std::string_view candidate,uint64_t seed,uint64_t edits,Controls controls,const std::filesystem::path& captureDirectory={});
void smoke();
} // namespace salsa::evaluation::corpus
