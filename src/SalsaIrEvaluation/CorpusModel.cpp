#include "Corpus.h"
#include <unordered_map>

namespace salsa::evaluation::corpus {
uint32_t Pool::add(Kind kind,uint64_t value,uint32_t aux,std::span<const uint32_t> child) {
    const auto index=slotCount(nodes.size()),first=slotCount(children.size());
    slotCount(children.size()+child.size()); children.insert(children.end(),child.begin(),child.end());
    nodes.push_back({value,first,slotCount(child.size()),aux,kind}); return index;
}
uint32_t Pool::string(Kind kind,std::string_view value) {
    const auto first=slotCount(text.size()); slotCount(text.size()+value.size()); text.insert(text.end(),value.begin(),value.end());
    return add(kind,first,slotCount(value.size()));
}
std::span<const uint32_t> Pool::child(uint32_t i) const {
    expect(i<nodes.size(),"Invalid corpus node"); const auto& n=nodes[i];
    expect(n.first<=children.size() && n.count<=children.size()-n.first,"Invalid corpus child range");
    return std::span<const uint32_t>(children).subspan(n.first,n.count);
}
uint32_t Pool::clone(uint32_t root,Pool& destination) const {
    const auto n=nodes.at(root); std::vector<uint32_t> copied;
    // Same-pool cloning may reallocate; copy child handles before appending.
    const auto view=child(root); std::vector<uint32_t> original(view.begin(),view.end()); copied.reserve(original.size());
    for(auto i:original) copied.push_back(clone(i,destination));
    if(n.kind==Kind::Plain || n.kind==Kind::Chunk) {
        expect(n.value<=text.size() && n.aux<=text.size()-n.value,"Invalid corpus text range");
        const std::string value(text.data()+n.value,n.aux); return destination.string(n.kind,value);
    }
    return destination.add(n.kind,n.value,n.aux,copied);
}
uint32_t Pool::atPath(uint32_t root,std::span<const uint32_t> path) const {
    for(auto ordinal:path) { auto c=child(root); expect(ordinal<c.size(),"Stale corpus edit path"); root=c[ordinal]; } return root;
}
namespace {
namespace sct=spice::sct;
struct Normalizer {
    Model& m; Counts counts; std::vector<uint32_t> expressionChildren;
    std::unordered_map<uint64_t,uint64_t> instructions,strings,footer;
    uint32_t list(Kind kind,std::span<const uint32_t> words,uint32_t aux=0) {
        std::vector<uint32_t> c; c.reserve(words.size()); for(auto w:words) c.push_back(m.pool.add(Kind::Word,w)); return m.pool.add(kind,0,aux,c);
    }
    uint32_t expression(const sct::SctCanonicalExpression& e) {
        ++counts.expressions; auto& c=expressionChildren;c.clear();
        if(auto* p=std::get_if<sct::SctTypedScptProgram>(&e.body)) {
            c.reserve(p->operations.size());
            for(const auto& op:p->operations) c.push_back(std::visit([&](const auto& v)->uint32_t {
                using T=std::decay_t<decltype(v)>;
                if constexpr(std::is_same_v<T,sct::SctScptValueOperation>) {
                    std::vector<uint32_t> payload; for(auto w:v.payloadWords) payload.push_back(m.pool.add(Kind::Word,w));
                    return m.pool.add(Kind::Value,v.encodingWord,static_cast<uint32_t>(v.kind),payload);
                } else if constexpr(std::is_same_v<T,sct::SctScptBinaryOperation>) return m.pool.add(Kind::Binary,v.encodingWord,static_cast<uint32_t>(v.kind));
                else if constexpr(std::is_same_v<T,sct::SctScptInertOperation>) return m.pool.add(Kind::Inert,v.encodingWord);
                else return m.pool.add(Kind::Stack,v.encodingWord);
            },op));
            return m.pool.add(Kind::Expression,static_cast<uint64_t>(e.termination),0,c);
        }
        ++counts.executionGaps; return m.pool.add(Kind::Gap,std::get<sct::SctOpaqueExpression>(e.body).words.size(),1);
    }
    uint32_t parameter(const sct::SctDocumentParameter& p) {
        ++counts.parameters;
        const auto child=std::visit([&](const auto& v)->uint32_t {
            using T=std::decay_t<decltype(v)>;
            if constexpr(std::is_same_v<T,sct::SctEncodedWordValue>) return m.pool.add(Kind::Word,v.value);
            else if constexpr(std::is_same_v<T,sct::SctCanonicalExpression>) return expression(v);
            else if constexpr(std::is_same_v<T,sct::SctTerminatedWordSequenceValue>) return list(Kind::Words,v.words);
            else if constexpr(std::is_same_v<T,sct::SctInstructionReference>) return m.pool.add(Kind::InstructionRef,instructions.at(v.target.value()));
            else if constexpr(std::is_same_v<T,sct::SctStringReference>) return m.pool.add(Kind::StringRef,strings.at(v.target.value()));
            else if constexpr(std::is_same_v<T,sct::SctSupplementaryTextReference>) return m.pool.add(Kind::FooterRef,footer.at(v.target.value()));
            else if constexpr(std::is_same_v<T,sct::SctUnresolvedReferenceValue>) {
                ++counts.unresolved; auto root=list(Kind::Unresolved,v.encodedWords,v.expectedTarget.textKind?1+static_cast<uint32_t>(*v.expectedTarget.textKind):0);
                m.pool.nodes[root].value=static_cast<uint64_t>(v.expectedTarget.storage); return root;
            } else { ++counts.executionGaps; return m.pool.add(Kind::Gap,v.words.size(),2); }
        },p.value);
        return m.pool.add(Kind::Parameter,p.schemaIndex,0,std::span(&child,1));
    }
    uint32_t text(const sct::SctTextValue& t) {
        ++counts.texts;
        return std::visit([&](const auto& value)->uint32_t {
            using T=std::decay_t<decltype(value)>;
            if constexpr(std::is_same_v<T,sct::SctPlainText>) return m.pool.string(Kind::Plain,value.utf8);
            else if constexpr(std::is_same_v<T,sct::SctMessage>) {
                std::vector<uint32_t> c; if(value.headerUtf8) c.push_back(m.pool.string(Kind::Plain,*value.headerUtf8));
                for(const auto& element:value.body.elements) c.push_back(std::visit([&](const auto& v)->uint32_t {
                    using E=std::decay_t<decltype(v)>;
                    if constexpr(std::is_same_v<E,sct::SctTextChunk>) return m.pool.string(Kind::Chunk,v.utf8);
                    else {
                        std::vector<uint32_t> args;
                        std::visit([&](const auto& a){using A=std::decay_t<decltype(a)>;
                            if constexpr(std::is_same_v<A,sct::SctDecimalCommandArgument>) { if(a.value) args.push_back(m.pool.add(Kind::Word,*a.value)); }
                            else if constexpr(std::is_same_v<A,sct::SctByteListCommandArgument>) for(auto byte:a.values) args.push_back(m.pool.add(Kind::Word,byte));
                        },v.argument);
                        return m.pool.add(Kind::Command,static_cast<uint64_t>(v.code),static_cast<uint32_t>(v.argument.index()),args);
                    }
                },element));
                return m.pool.add(Kind::Message,value.headerUtf8.has_value(),0,c);
            } else if constexpr(std::is_same_v<T,sct::SctEmptyIndexedText>) return m.pool.add(Kind::Empty);
            else { counts.sourceOpaqueBytes+=value.bytes.size(); ++counts.sourceOpaqueItems; return m.pool.add(Kind::Gap,value.bytes.size(),3); }
        },t);
    }
};
}
Counts appendDocument(Model& m,const spice::sct::SctDocument& doc,Script script) {
    Normalizer n{m}; script.id=ScriptId{++m.highWater}; const auto owner=script.id; m.scripts.push_back(std::move(script));
    for(const auto& s:doc.sections) {
        if(const auto* code=std::get_if<sct::SctScriptSectionContent>(&s.content))
            for(const auto& i:code->instructions) expect(n.instructions.emplace(i.id.value(),++m.highWater).second,"Duplicate native instruction ID");
        if(const auto* text=std::get_if<sct::SctStringSectionContent>(&s.content)) expect(n.strings.emplace(text->string.id.value(),++m.highWater).second,"Duplicate native string ID");
    }
    for(const auto& text:doc.supplementaryText) expect(n.footer.emplace(text.id.value(),++m.highWater).second,"Duplicate footer ID");
    std::vector<uint32_t> instructionChildren,groupChildren;
    for(const auto& s:doc.sections) {
        SectionInfo section{SectionId{++m.highWater},owner,s.nameBytes,{}};
        if(const auto* code=std::get_if<sct::SctScriptSectionContent>(&s.content)) {
            for(const auto& i:code->instructions) {
                auto& c=instructionChildren;c.clear();
                if(i.scheduledExpression) c.push_back(n.expression(*i.scheduledExpression));
                for(const auto& p:i.fixedParameters) c.push_back(n.parameter(p));
                for(const auto& group:i.repeatedParameterGroups) {
                    auto& parameters=groupChildren;parameters.clear(); for(const auto& p:group.parameters) parameters.push_back(n.parameter(p));
                    c.push_back(m.pool.add(Kind::Group,0,0,parameters));
                }
                const auto root=m.pool.add(Kind::Instruction,i.opcode,(i.skipRefresh?1u:0u)|(i.scheduledExpression?2u:0u),c);
                const InstructionId id{n.instructions.at(i.id.value())}; section.order.push_back(id);
                m.records.push_back({id,section.id,Op::Unknown,root,0}); ++n.counts.instructions;
            }
        } else if(const auto* value=std::get_if<sct::SctStringSectionContent>(&s.content)) {
            auto root=n.text(value->string.value); m.texts.push_back({n.strings.at(value->string.id.value()),owner,root,static_cast<uint32_t>(value->string.kind)});
            const auto preamble=n.list(Kind::Words,value->preambleWords);
            section.content=m.pool.add(Kind::Indexed,n.strings.at(value->string.id.value()),0,std::span(&preamble,1));
        } else if(const auto* marker=std::get_if<sct::SctStringGroupMarkerSectionContent>(&s.content)) section.content=n.list(Kind::Marker,marker->preambleWords);
        else { ++n.counts.executionGaps; section.content=m.pool.add(Kind::Gap,0,4); }
        m.sections.push_back(std::move(section));
    }
    for(const auto& value:doc.supplementaryText) m.texts.push_back({n.footer.at(value.id.value()),owner,n.text(value.value),static_cast<uint32_t>(value.kind)});
    for(const auto& a:doc.opaqueAttachments) { n.counts.sourceOpaqueBytes+=a.bytes.size(); ++n.counts.sourceOpaqueItems; }
    return n.counts;
}
} // namespace salsa::evaluation::corpus
