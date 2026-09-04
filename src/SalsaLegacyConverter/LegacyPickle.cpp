#include "LegacyPickle.h"

#include <algorithm>
#include <array>
#include <bit>
#include <fstream>
#include <limits>
#include <cstring>
#include <ranges>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace salsa::legacy {
namespace {

class ParseError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

template <typename T>
void writePod(std::ofstream& output, const T& value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

template <typename T>
[[nodiscard]] bool readPod(std::ifstream& input, T& value) {
    input.read(reinterpret_cast<char*>(&value), sizeof(value));
    return input.good();
}

}  // namespace

class ValueStore::Impl final {
public:
    struct Node final {
        // Primitive scalar, payload offset, or container mutation head.
        std::uint64_t data = 0;
        // Payload size or admitted object-class code.
        std::uint64_t auxiliary = 0;
        ValueKind kind = ValueKind::Null;
    };
    static_assert(sizeof(Node) == 24);

    explicit Impl(std::filesystem::path spoolPath) : path(std::move(spoolPath)) {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) { failure = "pickle spool directory could not be created"; return; }
        writer.open(path, std::ios::binary | std::ios::trunc);
        if (!writer) { failure = "pickle spool could not be created"; return; }
        // Offset zero terminates mutation chains.
        writer.put('\0');
        writerOffset = 1;
        dirty = true;
        syncForRead();
        reader.open(path, std::ios::binary);
        if (!reader) failure = "pickle spool could not be opened for reading";
    }

    ~Impl() {
        reader.close();
        writer.close();
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }

    [[nodiscard]] Node& node(const ValueId id) {
        if (id == InvalidValueId || id > nodeCount)
            throw std::out_of_range("invalid pickle spool node");
        const auto index = id - 1u;
        return nodeBlocks[static_cast<std::size_t>(index / NodesPerBlock)]
            [static_cast<std::size_t>(index % NodesPerBlock)];
    }
    [[nodiscard]] const Node& node(const ValueId id) const {
        if (id == InvalidValueId || id > nodeCount)
            throw std::out_of_range("invalid pickle spool node");
        const auto index = id - 1u;
        return nodeBlocks[static_cast<std::size_t>(index / NodesPerBlock)]
            [static_cast<std::size_t>(index % NodesPerBlock)];
    }

    [[nodiscard]] ValueId appendNode(const ValueKind kind) {
        if ((nodeCount % NodesPerBlock) == 0)
            nodeBlocks.push_back(std::make_unique<Node[]>(NodesPerBlock));
        ++nodeCount;
        auto& value = node(nodeCount);
        value = {}; value.kind = kind;
        return nodeCount;
    }

    [[nodiscard]] std::uint64_t appendPayload(const void* data,
        const std::uint64_t size) {
        ensureWritable();
        const auto position = writerOffset;
        if (size != 0) writer.write(static_cast<const char*>(data),
            static_cast<std::streamsize>(size));
        if (!writer) throw std::runtime_error("pickle spool write failed");
        writerOffset += size; dirty = true;
        return position;
    }

    [[nodiscard]] std::uint64_t appendItemBlock(const std::uint64_t previous,
        const std::vector<ValueId>& items) {
        ensureWritable();
        const auto position = writerOffset;
        writePod(writer, previous);
        const auto count = static_cast<std::uint64_t>(items.size());
        writePod(writer, count);
        if (!items.empty()) writer.write(reinterpret_cast<const char*>(items.data()),
            static_cast<std::streamsize>(items.size() * sizeof(ValueId)));
        if (!writer) throw std::runtime_error("pickle spool write failed");
        writerOffset += sizeof(previous) + sizeof(count)
            + items.size() * sizeof(ValueId);
        dirty = true; return position;
    }

    [[nodiscard]] std::uint64_t appendEntryBlock(const std::uint64_t previous,
        const std::vector<std::pair<ValueId, ValueId>>& entries) {
        ensureWritable();
        const auto position = writerOffset;
        writePod(writer, previous);
        const auto count = static_cast<std::uint64_t>(entries.size());
        writePod(writer, count);
        for (const auto& [key, value] : entries) {
            writePod(writer, key);
            writePod(writer, value);
        }
        if (!writer) throw std::runtime_error("pickle spool write failed");
        writerOffset += sizeof(previous) + sizeof(count)
            + entries.size() * sizeof(ValueId) * 2u;
        dirty = true; return position;
    }

    void ensureWritable() const {
        if (sealed) throw std::runtime_error("pickle spool is sealed");
    }

    void syncForRead() const {
        if (!dirty) return;
        writer.flush();
        if (!writer) throw std::runtime_error("pickle spool flush failed");
        dirty = false;
    }

    void seal() {
        syncForRead();
        writer.close();
        sealed = true;
    }

    [[nodiscard]] std::vector<std::byte> readPayload(const std::uint64_t offset,
        const std::uint64_t size) const {
        syncForRead();
        reader.clear();
        reader.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!reader || size > std::numeric_limits<std::size_t>::max())
            throw std::runtime_error("pickle spool read seek failed");
        std::vector<std::byte> result(static_cast<std::size_t>(size));
        if (!result.empty()) reader.read(reinterpret_cast<char*>(result.data()),
            static_cast<std::streamsize>(result.size()));
        if (!reader) throw std::runtime_error("pickle spool read failed");
        return result;
    }

    [[nodiscard]] std::vector<std::vector<ValueId>> readItemBlocks(
        std::uint64_t offset) const {
        syncForRead();
        std::vector<std::vector<ValueId>> blocks;
        while (offset != 0) {
            reader.clear(); reader.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
            std::uint64_t previous = 0, count = 0;
            if (!readPod(reader, previous) || !readPod(reader, count)
                || count > std::numeric_limits<std::size_t>::max())
                throw std::runtime_error("pickle spool item block is malformed");
            std::vector<ValueId> block(static_cast<std::size_t>(count));
            if (!block.empty()) reader.read(reinterpret_cast<char*>(block.data()),
                static_cast<std::streamsize>(block.size() * sizeof(ValueId)));
            if (!reader) throw std::runtime_error("pickle spool item block is truncated");
            blocks.push_back(std::move(block)); offset = previous;
        }
        return blocks;
    }

    [[nodiscard]] std::vector<std::vector<std::pair<ValueId, ValueId>>>
    readEntryBlocks(std::uint64_t offset) const {
        syncForRead();
        std::vector<std::vector<std::pair<ValueId, ValueId>>> blocks;
        while (offset != 0) {
            reader.clear(); reader.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
            std::uint64_t previous = 0, count = 0;
            if (!readPod(reader, previous) || !readPod(reader, count)
                || count > std::numeric_limits<std::size_t>::max())
                throw std::runtime_error("pickle spool entry block is malformed");
            std::vector<std::pair<ValueId, ValueId>> block;
            block.reserve(static_cast<std::size_t>(count));
            for (std::uint64_t index = 0; index < count; ++index) {
                ValueId key = 0, value = 0;
                if (!readPod(reader, key) || !readPod(reader, value))
                    throw std::runtime_error("pickle spool entry block is truncated");
                block.emplace_back(key, value);
            }
            blocks.push_back(std::move(block)); offset = previous;
        }
        return blocks;
    }

    [[nodiscard]] static std::uint64_t classCode(const std::string_view name) {
        static constexpr std::array names{
            "SALSA.Project.project_container.SCTProject",
            "SALSA.Project.project_container.SCTScript",
            "SALSA.Project.project_container.SCTSection",
            "SALSA.Project.project_container.SCTInstruction",
            "SALSA.Project.project_container.SCTParameter",
            "SALSA.Project.project_container.SCTLink",
        };
        const auto found = std::ranges::find(names, name);
        if (found == names.end()) throw std::runtime_error("unknown legacy object class");
        return static_cast<std::uint64_t>(std::distance(names.begin(), found) + 1u);
    }

    [[nodiscard]] static std::string_view className(const std::uint64_t code) {
        static constexpr std::array names{
            "SALSA.Project.project_container.SCTProject",
            "SALSA.Project.project_container.SCTScript",
            "SALSA.Project.project_container.SCTSection",
            "SALSA.Project.project_container.SCTInstruction",
            "SALSA.Project.project_container.SCTParameter",
            "SALSA.Project.project_container.SCTLink",
        };
        if (code == 0 || code > names.size()) throw std::runtime_error("invalid legacy object class");
        return names[static_cast<std::size_t>(code - 1u)];
    }

    [[nodiscard]] std::string fieldName(const ValueId value) const {
        if (const auto found = fieldNames.find(value); found != fieldNames.end())
            return found->second;
        const auto& descriptor = node(value);
        const auto bytes = readPayload(descriptor.data, descriptor.auxiliary);
        auto name = std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        fieldNames.emplace(value, name);
        return name;
    }

    std::filesystem::path path{};
    mutable std::ifstream reader{};
    mutable std::ofstream writer{};
    std::uint64_t writerOffset = 0;
    mutable bool dirty = false;
    bool sealed = false;
    static constexpr std::uint64_t NodesPerBlock = 65'536;
    std::vector<std::unique_ptr<Node[]>> nodeBlocks{};
    std::uint64_t nodeCount = 0;
    mutable std::unordered_map<ValueId, std::string> fieldNames{};
    std::string failure{};
};

ValueStore::ValueStore(std::filesystem::path path)
    : impl_(std::make_unique<Impl>(std::move(path))) {}
ValueStore::~ValueStore() = default;
bool ValueStore::valid() const noexcept { return impl_ && impl_->failure.empty(); }
const std::string& ValueStore::error() const noexcept { return impl_->failure; }
const std::filesystem::path& ValueStore::path() const noexcept { return impl_->path; }
std::uint64_t ValueStore::nodeCount() const noexcept { return impl_->nodeCount; }
void ValueStore::seal() { impl_->seal(); }

ValueId ValueStore::create(const ValueKind kind) {
    return impl_->appendNode(kind);
}
ValueId ValueStore::createBoolean(const bool value) {
    const auto id = create(ValueKind::Boolean); impl_->node(id).data = value; return id;
}
ValueId ValueStore::createFloat(const std::uint64_t bits) {
    const auto id = create(ValueKind::Float); impl_->node(id).data = bits; return id;
}
ValueId ValueStore::createText(const ValueKind kind, const std::string_view value) {
    const auto id = create(kind); auto& node = impl_->node(id);
    node.data = impl_->appendPayload(value.data(), value.size());
    node.auxiliary = value.size(); return id;
}
ValueId ValueStore::createBytes(const std::vector<std::byte>& value) {
    const auto id = create(ValueKind::Bytes); auto& node = impl_->node(id);
    node.data = impl_->appendPayload(value.data(), value.size());
    node.auxiliary = value.size(); return id;
}
ValueId ValueStore::createObject(const std::string_view className) {
    const auto id = create(ValueKind::Object);
    impl_->node(id).auxiliary = Impl::classCode(className);
    return id;
}
void ValueStore::appendItems(const ValueId target, const std::vector<ValueId>& items) {
    if (items.empty()) return; auto& node = impl_->node(target);
    node.data = impl_->appendItemBlock(node.data, items);
    node.auxiliary += items.size();
}
void ValueStore::appendEntries(const ValueId target,
    const std::vector<std::pair<ValueId, ValueId>>& entries) {
    if (entries.empty()) return; auto& node = impl_->node(target);
    node.data = impl_->appendEntryBlock(node.data, entries);
    if (node.kind != ValueKind::Object) node.auxiliary += entries.size();
}
void ValueStore::replaceItems(const ValueId target, const std::vector<ValueId>& items) {
    auto& node = impl_->node(target); node.data = 0; node.auxiliary = 0;
    appendItems(target, items);
}
void ValueStore::replaceEntries(const ValueId target,
    const std::vector<std::pair<ValueId, ValueId>>& entries) {
    auto& node = impl_->node(target); node.data = 0;
    if (node.kind != ValueKind::Object) node.auxiliary = 0;
    appendEntries(target, entries);
}
void ValueStore::replaceText(const ValueId target, const std::string_view value) {
    auto& node = impl_->node(target);
    node.data = impl_->appendPayload(value.data(), value.size());
    node.auxiliary = value.size();
}
ValueKind ValueStore::kind(const ValueId value) const { return impl_->node(value).kind; }
bool ValueStore::boolean(const ValueId value) const { return impl_->node(value).data != 0; }
std::uint64_t ValueStore::floatBits(const ValueId value) const { return impl_->node(value).data; }
std::string ValueStore::text(const ValueId value) const {
    const auto& node = impl_->node(value);
    const auto bytes = impl_->readPayload(node.data, node.auxiliary);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
std::vector<std::byte> ValueStore::bytes(const ValueId value) const {
    const auto& node = impl_->node(value);
    return impl_->readPayload(node.data, node.auxiliary);
}
std::vector<ValueId> ValueStore::items(const ValueId value) const {
    auto blocks = impl_->readItemBlocks(impl_->node(value).data);
    std::size_t count = 0; for (const auto& block : blocks) count += block.size();
    std::vector<ValueId> result; result.reserve(count);
    for (auto block = blocks.rbegin(); block != blocks.rend(); ++block)
        result.insert(result.end(), block->begin(), block->end());
    return result;
}
std::vector<std::pair<ValueId, ValueId>> ValueStore::entries(const ValueId value) const {
    auto blocks = impl_->readEntryBlocks(impl_->node(value).data);
    std::size_t count = 0; for (const auto& block : blocks) count += block.size();
    std::vector<std::pair<ValueId, ValueId>> result; result.reserve(count);
    for (auto block = blocks.rbegin(); block != blocks.rend(); ++block)
        result.insert(result.end(), block->begin(), block->end());
    return result;
}
StoredObject ValueStore::object(const ValueId value) const {
    const auto& descriptor = impl_->node(value);
    StoredObject result{std::string(Impl::className(descriptor.auxiliary)), {}};
    for (const auto& [key, child] : entries(value))
        result.attributes.emplace_back(impl_->fieldName(key), child);
    return result;
}
std::string_view ValueStore::objectClassName(const ValueId value) const {
    return Impl::className(impl_->node(value).auxiliary);
}
ValueId ValueStore::objectAttribute(const ValueId value, const std::string_view key) const {
    const auto& descriptor = impl_->node(value);
    if (descriptor.kind != ValueKind::Object) return InvalidValueId;
    const auto blocks = impl_->readEntryBlocks(descriptor.data);
    for (const auto& block : blocks)
        for (const auto& [candidate, child] : block)
            if (impl_->fieldName(candidate) == key) return child;
    return InvalidValueId;
}
std::uint64_t ValueStore::containerSize(const ValueId value) const {
    if (value == InvalidValueId) return 0;
    const auto& node = impl_->node(value);
    return node.kind == ValueKind::List || node.kind == ValueKind::Tuple
        || node.kind == ValueKind::Set || node.kind == ValueKind::FrozenSet
        || node.kind == ValueKind::Dictionary ? node.auxiliary : 0;
}

class ValueStore::ReadSession::Impl final {
public:
    explicit Impl(const ValueStore& valueStore) : store(*valueStore.impl_) {
        if (!store.sealed) throw std::logic_error("pickle spool is not sealed");
        reader.open(store.path, std::ios::binary);
        if (!reader) throw std::runtime_error("pickle spool read session could not be opened");
    }

    [[nodiscard]] std::vector<std::byte> readPayload(const std::uint64_t offset,
        const std::uint64_t size) const {
        reader.clear(); reader.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!reader || size > std::numeric_limits<std::size_t>::max())
            throw std::runtime_error("pickle spool read seek failed");
        std::vector<std::byte> result(static_cast<std::size_t>(size));
        if (!result.empty()) reader.read(reinterpret_cast<char*>(result.data()),
            static_cast<std::streamsize>(result.size()));
        if (!reader) throw std::runtime_error("pickle spool read failed");
        return result;
    }

    [[nodiscard]] std::vector<std::vector<ValueId>> readItemBlocks(
        std::uint64_t offset) const {
        std::vector<std::vector<ValueId>> blocks;
        while (offset != 0) {
            reader.clear(); reader.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
            std::uint64_t previous = 0, count = 0;
            if (!readPod(reader, previous) || !readPod(reader, count)
                || count > std::numeric_limits<std::size_t>::max())
                throw std::runtime_error("pickle spool item block is malformed");
            std::vector<ValueId> block(static_cast<std::size_t>(count));
            if (!block.empty()) reader.read(reinterpret_cast<char*>(block.data()),
                static_cast<std::streamsize>(block.size() * sizeof(ValueId)));
            if (!reader) throw std::runtime_error("pickle spool item block is truncated");
            blocks.push_back(std::move(block)); offset = previous;
        }
        return blocks;
    }

    [[nodiscard]] std::vector<std::vector<std::pair<ValueId, ValueId>>>
    readEntryBlocks(std::uint64_t offset) const {
        std::vector<std::vector<std::pair<ValueId, ValueId>>> blocks;
        while (offset != 0) {
            reader.clear(); reader.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
            std::uint64_t previous = 0, count = 0;
            if (!readPod(reader, previous) || !readPod(reader, count)
                || count > std::numeric_limits<std::size_t>::max())
                throw std::runtime_error("pickle spool entry block is malformed");
            std::vector<std::pair<ValueId, ValueId>> block;
            block.reserve(static_cast<std::size_t>(count));
            for (std::uint64_t index = 0; index < count; ++index) {
                ValueId key = 0, child = 0;
                if (!readPod(reader, key) || !readPod(reader, child))
                    throw std::runtime_error("pickle spool entry block is truncated");
                block.emplace_back(key, child);
            }
            blocks.push_back(std::move(block)); offset = previous;
        }
        return blocks;
    }

    [[nodiscard]] std::string fieldName(const ValueId value) const {
        if (const auto found = fieldNames.find(value); found != fieldNames.end())
            return found->second;
        const auto& descriptor = store.node(value);
        const auto bytes = readPayload(descriptor.data, descriptor.auxiliary);
        auto name = std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        fieldNames.emplace(value, name); return name;
    }

    const ValueStore::Impl& store;
    mutable std::ifstream reader{};
    mutable std::unordered_map<ValueId, std::string> fieldNames{};
};

ValueStore::ReadSession::ReadSession(const ValueStore& store)
    : impl_(std::make_unique<Impl>(store)) {}
ValueStore::ReadSession::~ReadSession() = default;
ValueStore::ReadSession::ReadSession(ReadSession&&) noexcept = default;
ValueStore::ReadSession& ValueStore::ReadSession::operator=(ReadSession&&) noexcept = default;
ValueStore::ReadSession ValueStore::openReadSession() const { return ReadSession(*this); }

ValueKind ValueStore::ReadSession::kind(const ValueId value) const {
    return impl_->store.node(value).kind;
}
bool ValueStore::ReadSession::boolean(const ValueId value) const {
    return impl_->store.node(value).data != 0;
}
std::uint64_t ValueStore::ReadSession::floatBits(const ValueId value) const {
    return impl_->store.node(value).data;
}
std::string ValueStore::ReadSession::text(const ValueId value) const {
    const auto& node = impl_->store.node(value);
    const auto bytes = impl_->readPayload(node.data, node.auxiliary);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
std::vector<std::byte> ValueStore::ReadSession::bytes(const ValueId value) const {
    const auto& node = impl_->store.node(value);
    return impl_->readPayload(node.data, node.auxiliary);
}
std::vector<ValueId> ValueStore::ReadSession::items(const ValueId value) const {
    auto blocks = impl_->readItemBlocks(impl_->store.node(value).data);
    std::size_t count = 0; for (const auto& block : blocks) count += block.size();
    std::vector<ValueId> result; result.reserve(count);
    for (auto block = blocks.rbegin(); block != blocks.rend(); ++block)
        result.insert(result.end(), block->begin(), block->end());
    return result;
}
std::vector<std::pair<ValueId, ValueId>> ValueStore::ReadSession::entries(
    const ValueId value) const {
    auto blocks = impl_->readEntryBlocks(impl_->store.node(value).data);
    std::size_t count = 0; for (const auto& block : blocks) count += block.size();
    std::vector<std::pair<ValueId, ValueId>> result; result.reserve(count);
    for (auto block = blocks.rbegin(); block != blocks.rend(); ++block)
        result.insert(result.end(), block->begin(), block->end());
    return result;
}
StoredObject ValueStore::ReadSession::object(const ValueId value) const {
    const auto& descriptor = impl_->store.node(value);
    StoredObject result{std::string(ValueStore::Impl::className(descriptor.auxiliary)), {}};
    for (const auto& [key, child] : entries(value))
        result.attributes.emplace_back(impl_->fieldName(key), child);
    return result;
}
std::string_view ValueStore::ReadSession::objectClassName(const ValueId value) const {
    return ValueStore::Impl::className(impl_->store.node(value).auxiliary);
}
ValueId ValueStore::ReadSession::objectAttribute(const ValueId value,
    const std::string_view key) const {
    const auto& descriptor = impl_->store.node(value);
    if (descriptor.kind != ValueKind::Object) return InvalidValueId;
    const auto blocks = impl_->readEntryBlocks(descriptor.data);
    for (const auto& block : blocks)
        for (const auto& [candidate, child] : block)
            if (impl_->fieldName(candidate) == key) return child;
    return InvalidValueId;
}
std::uint64_t ValueStore::ReadSession::containerSize(const ValueId value) const {
    if (value == InvalidValueId) return 0;
    const auto& node = impl_->store.node(value);
    return node.kind == ValueKind::List || node.kind == ValueKind::Tuple
        || node.kind == ValueKind::Set || node.kind == ValueKind::FrozenSet
        || node.kind == ValueKind::Dictionary ? node.auxiliary : 0;
}

namespace {

inline constexpr ValueId MarkerValueId = std::numeric_limits<ValueId>::max();

class Reader final {
public:
    Reader(const std::filesystem::path& inputPath, const std::uint64_t inputSize,
        std::shared_ptr<ValueStore> store, PickleLimits limits, const bool disabled,
        std::function<void(std::uint64_t, std::uint64_t)> progress,
        std::function<bool(std::span<const std::byte>)> inputChunk)
        : inputSize_(inputSize), store_(std::move(store)), limits_(limits),
          disabled_(disabled), progress_(std::move(progress)),
          inputChunk_(std::move(inputChunk)) {
        input_.open(inputPath, std::ios::binary);
    }

    [[nodiscard]] PickleResult read() {
        try {
            if (!input_) fail("input could not be opened");
            if (byte() != 0x80u) fail("pickle does not start with PROTO");
            protocol_ = byte();
            if (protocol_ != 4u) fail("only pickle protocol 4 is supported");
            for (;;) {
                const auto opcode = byte();
                switch (opcode) {
                case '.':
                    if (stack_.size() != 1u || stack_.back() == MarkerValueId)
                        fail("STOP requires one root value");
                    if (position_ != inputSize_) fail("trailing data after STOP");
                    return {store_, stack_.back(), {}, protocol_};
                case 0x95: frame(); break;
                case '(': push(MarkerValueId); break;
                case '0': (void)pop(); break;
                case '1': popMark(); break;
                case '2': push(top()); break;
                case 'N': push(store_->create(ValueKind::Null)); break;
                case 0x88: push(store_->createBoolean(true)); break;
                case 0x89: push(store_->createBoolean(false)); break;
                case 'J': pushInteger(std::to_string(readSigned32())); break;
                case 'K': pushInteger(std::to_string(byte())); break;
                case 'M': pushInteger(std::to_string(readUnsigned16())); break;
                case 0x8a: pushLong(byte()); break;
                case 0x8b: pushLong(readUnsigned32()); break;
                case 'G': pushFloat(); break;
                case 'X': pushString(readUnsigned32()); break;
                case 0x8c: pushString(byte()); break;
                case 0x8d: pushString(readUnsigned64()); break;
                case 'B': pushBytes(readUnsigned32()); break;
                case 'C': pushBytes(byte()); break;
                case 0x8e: pushBytes(readUnsigned64()); break;
                case ']': push(store_->create(ValueKind::List)); break;
                case ')': push(store_->create(ValueKind::Tuple)); break;
                case '}': push(store_->create(ValueKind::Dictionary)); break;
                case 0x8f: push(store_->create(ValueKind::Set)); break;
                case 'l': buildSequence(ValueKind::List); break;
                case 't': buildSequence(ValueKind::Tuple); break;
                case 0x91: buildSequence(ValueKind::FrozenSet); break;
                case 0x85: buildShortTuple(1); break;
                case 0x86: buildShortTuple(2); break;
                case 0x87: buildShortTuple(3); break;
                case 'a': append(); break;
                case 'e': appendMany(false); break;
                case 0x90: appendMany(true); break;
                case 'd': buildDictionary(); break;
                case 's': setItem(); break;
                case 'u': setItems(); break;
                case 'q': memoPut(byte()); break;
                case 'r': memoPut(readUnsigned32()); break;
                case 0x94: memoPut(static_cast<std::uint32_t>(memo_.size())); break;
                case 'h': memoGet(byte()); break;
                case 'j': memoGet(readUnsigned32()); break;
                case 'c': globalText(); break;
                case 0x93: stackGlobal(); break;
                case 0x81: newObject(false); break;
                case 0x92: newObject(true); break;
                case 'b': buildObject(); break;
                case 'R': reduce(); break;
                case 'P': case 'Q': case 0x82: case 0x83: case 0x84:
                    fail("persistent IDs and extension opcodes are not allowed");
                default: fail("unsupported pickle opcode " + std::to_string(opcode));
                }
            }
        } catch (const ParseError& error) {
            return {nullptr, 0, error.what(), protocol_};
        } catch (const std::bad_alloc&) {
            return {nullptr, 0, "memory allocation failed while reading pickle", protocol_};
        } catch (const std::exception& error) {
            return {nullptr, 0, error.what(), protocol_};
        }
    }

private:
    [[noreturn]] void fail(const std::string& message) const {
        throw ParseError(message + " at byte " + std::to_string(position_));
    }
    [[nodiscard]] std::uint8_t byte() {
        if (position_ >= inputSize_) fail("unexpected end of pickle");
        refill();
        const auto value = inputBuffer_[bufferPosition_++];
        ++position_; return static_cast<std::uint8_t>(static_cast<unsigned char>(value));
    }
    [[nodiscard]] std::uint16_t readUnsigned16() {
        const auto a = byte(); const auto b = byte();
        return static_cast<std::uint16_t>(a | (b << 8u));
    }
    [[nodiscard]] std::uint32_t readUnsigned32() {
        std::uint32_t value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8)
            value |= static_cast<std::uint32_t>(byte()) << shift;
        return value;
    }
    [[nodiscard]] std::int32_t readSigned32() {
        return std::bit_cast<std::int32_t>(readUnsigned32());
    }
    [[nodiscard]] std::uint64_t readUnsigned64() {
        std::uint64_t value = 0;
        for (unsigned shift = 0; shift < 64; shift += 8)
            value |= static_cast<std::uint64_t>(byte()) << shift;
        return value;
    }
    [[nodiscard]] std::vector<std::byte> rawUncounted(const std::uint64_t size) {
        if (size > inputSize_ - position_
            || size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())
            || size > std::numeric_limits<std::size_t>::max()) fail("payload exceeds pickle size");
        std::vector<std::byte> result(static_cast<std::size_t>(size));
        std::size_t written = 0;
        while (written < result.size()) {
            refill();
            const auto available = bufferSize_ - bufferPosition_;
            const auto count = std::min(available, result.size() - written);
            std::memcpy(result.data() + written, inputBuffer_.data() + bufferPosition_, count);
            bufferPosition_ += count; written += count; position_ += count;
        }
        return result;
    }

    void refill() {
        if (bufferPosition_ != bufferSize_) return;
        input_.read(inputBuffer_.data(), static_cast<std::streamsize>(inputBuffer_.size()));
        const auto count = input_.gcount();
        if (count <= 0) fail("unexpected end of pickle");
        bufferPosition_ = 0; bufferSize_ = static_cast<std::size_t>(count);
        if (inputChunk_ && !inputChunk_(std::as_bytes(std::span{
                inputBuffer_.data(), bufferSize_})))
            fail("input digest update failed");
    }
    [[nodiscard]] std::vector<std::byte> raw(const std::uint64_t size) {
        if (!disabled_ && (size > limits_.maxRawBytes
            || rawBytes_ > limits_.maxRawBytes - size)) fail("raw byte resource limit exceeded");
        rawBytes_ += size; return rawUncounted(size);
    }
    [[nodiscard]] std::string text(const std::uint64_t size) {
        if (!disabled_ && size > limits_.maxTextBytes) fail("text resource limit exceeded");
        const auto data = rawUncounted(size);
        return {reinterpret_cast<const char*>(data.data()), data.size()};
    }
    [[nodiscard]] std::string line() {
        std::string result;
        while (position_ < inputSize_) {
            const auto value = byte(); if (value == '\n') return result;
            result.push_back(static_cast<char>(value));
        }
        fail("unterminated GLOBAL name");
    }
    void push(const ValueId value) {
        if (!disabled_ && stack_.size() >= limits_.maxStackEntries)
            fail("pickle stack resource limit exceeded");
        stack_.push_back(value);
    }
    [[nodiscard]] ValueId pop() {
        if (stack_.empty()) fail("pickle stack underflow");
        const auto value = stack_.back(); stack_.pop_back(); return value;
    }
    [[nodiscard]] ValueId top() const {
        if (stack_.empty()) fail("pickle stack underflow"); return stack_.back();
    }
    [[nodiscard]] std::size_t mark() const {
        for (auto index = stack_.size(); index > 0; --index)
            if (stack_[index - 1u] == MarkerValueId) return index - 1u;
        fail("pickle MARK is missing");
    }
    void popMark() { stack_.resize(mark()); }
    void frame() {
        const auto length = readUnsigned64();
        if (length > inputSize_ - position_) fail("FRAME exceeds pickle size");
        if (progress_ && position_ >= nextProgress_) {
            progress_(position_, inputSize_); nextProgress_ = position_ + (4ull << 20);
        }
    }
    void pushInteger(std::string value) {
        push(store_->createText(ValueKind::Integer, value));
    }
    void pushLong(const std::uint64_t length) {
        const auto bytes = raw(length);
        if (bytes.empty()) { pushInteger("0"); return; }
        const bool negative = (std::to_integer<unsigned char>(bytes.back()) & 0x80u) != 0;
        if (bytes.size() <= 8u) {
            std::uint64_t value = 0;
            for (std::size_t i = 0; i < bytes.size(); ++i)
                value |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(bytes[i])) << (i * 8u);
            if (negative && bytes.size() < 8u) value |= (~0ull) << (bytes.size() * 8u);
            pushInteger(negative ? std::to_string(std::bit_cast<std::int64_t>(value))
                                 : std::to_string(value)); return;
        }
        static constexpr char digits[] = "0123456789abcdef";
        std::string encoded = negative ? "-0x" : "0x";
        for (auto iterator = bytes.rbegin(); iterator != bytes.rend(); ++iterator) {
            const auto value = std::to_integer<unsigned char>(*iterator);
            encoded.push_back(digits[value >> 4u]); encoded.push_back(digits[value & 0x0fu]);
        }
        pushInteger(std::move(encoded));
    }
    void pushFloat() {
        std::array<std::byte, 8> bytes{};
        for (auto& value : bytes) value = static_cast<std::byte>(byte());
        std::ranges::reverse(bytes);
        push(store_->createFloat(std::bit_cast<std::uint64_t>(bytes)));
    }
    void pushString(const std::uint64_t length) {
        push(store_->createText(ValueKind::String, text(length)));
    }
    void pushBytes(const std::uint64_t length) { push(store_->createBytes(raw(length))); }

    void buildSequence(const ValueKind kind) {
        const auto marker = mark();
        std::vector<ValueId> items(stack_.begin() + marker + 1u, stack_.end());
        containerEntries_ += items.size(); enforceContainers(); stack_.resize(marker);
        const auto value = store_->create(kind); store_->appendItems(value, items); push(value);
    }
    void buildShortTuple(const std::size_t count) {
        if (stack_.size() < count) fail("tuple stack underflow");
        std::vector<ValueId> items(stack_.end() - count, stack_.end());
        stack_.resize(stack_.size() - count); containerEntries_ += count; enforceContainers();
        const auto value = store_->create(ValueKind::Tuple); store_->appendItems(value, items); push(value);
    }
    void append() {
        const auto item = pop(); const auto target = top();
        if (target == MarkerValueId || store_->kind(target) != ValueKind::List)
            fail("APPEND target is not a list");
        store_->appendItems(target, {item}); ++containerEntries_; enforceContainers();
    }
    void appendMany(const bool set) {
        const auto marker = mark(); if (marker == 0u) fail("APPENDS target is missing");
        const auto target = stack_[marker - 1u];
        if (target == MarkerValueId
            || (!set && store_->kind(target) != ValueKind::List)
            || (set && store_->kind(target) != ValueKind::Set))
            fail("sequence append target has wrong type");
        std::vector<ValueId> items(stack_.begin() + marker + 1u, stack_.end());
        store_->appendItems(target, items); containerEntries_ += items.size();
        enforceContainers(); stack_.resize(marker);
    }
    void buildDictionary() {
        const auto marker = mark(); const auto count = stack_.size() - marker - 1u;
        if ((count & 1u) != 0u) fail("DICT requires key/value pairs");
        std::vector<std::pair<ValueId, ValueId>> entries; entries.reserve(count / 2u);
        for (auto i = marker + 1u; i < stack_.size(); i += 2u)
            entries.emplace_back(stack_[i], stack_[i + 1u]);
        containerEntries_ += entries.size(); enforceContainers(); stack_.resize(marker);
        const auto value = store_->create(ValueKind::Dictionary);
        store_->appendEntries(value, entries); push(value);
    }
    void setItem() {
        const auto value = pop(); const auto key = pop(); const auto target = top();
        if (target == MarkerValueId || store_->kind(target) != ValueKind::Dictionary)
            fail("SETITEM target is not a dictionary");
        store_->appendEntries(target, {{key, value}}); ++containerEntries_; enforceContainers();
    }
    void setItems() {
        const auto marker = mark(); if (marker == 0u) fail("SETITEMS target is missing");
        const auto target = stack_[marker - 1u]; const auto count = stack_.size() - marker - 1u;
        if (target == MarkerValueId || store_->kind(target) != ValueKind::Dictionary
            || (count & 1u) != 0u) fail("SETITEMS target or pair count is invalid");
        std::vector<std::pair<ValueId, ValueId>> entries; entries.reserve(count / 2u);
        for (auto i = marker + 1u; i < stack_.size(); i += 2u)
            entries.emplace_back(stack_[i], stack_[i + 1u]);
        store_->appendEntries(target, entries); containerEntries_ += entries.size();
        enforceContainers(); stack_.resize(marker);
    }
    void enforceContainers() const {
        if (!disabled_ && containerEntries_ > limits_.maxContainerEntries)
            fail("container entry resource limit exceeded");
    }
    void memoPut(const std::uint32_t key) {
        if (!disabled_ && key >= limits_.maxMemoEntries)
            fail("pickle memo resource limit exceeded");
        if (top() == MarkerValueId) fail("pickle marker cannot be memoized");
        if (key >= memo_.size()) memo_.resize(static_cast<std::size_t>(key) + 1u);
        memo_[key] = top();
    }
    void memoGet(const std::uint32_t key) {
        if (key >= memo_.size() || memo_[key] == InvalidValueId)
            fail("pickle memo reference is missing");
        push(memo_[key]);
    }
    [[nodiscard]] static bool allowedGlobal(const std::string_view value) {
        static constexpr std::array allowed{
            "SALSA.Project.project_container.SCTProject",
            "SALSA.Project.project_container.SCTScript",
            "SALSA.Project.project_container.SCTSection",
            "SALSA.Project.project_container.SCTInstruction",
            "SALSA.Project.project_container.SCTParameter",
            "SALSA.Project.project_container.SCTLink",
            "builtins.bytearray",
        };
        return std::ranges::find(allowed, value) != allowed.end();
    }
    void globalText() {
        auto value = line() + "." + line();
        if (!allowedGlobal(value)) fail("pickle global is not allowed: " + value);
        push(store_->createText(ValueKind::Global, value));
    }
    void stackGlobal() {
        const auto name = pop(); const auto module = pop();
        if (store_->kind(name) != ValueKind::String || store_->kind(module) != ValueKind::String)
            fail("STACK_GLOBAL requires string names");
        auto value = store_->text(module) + "." + store_->text(name);
        if (!allowedGlobal(value)) fail("pickle global is not allowed: " + value);
        push(store_->createText(ValueKind::Global, value));
    }
    void newObject(const bool withKeywords) {
        if (withKeywords) {
            const auto kwargs = pop();
            if (store_->kind(kwargs) != ValueKind::Dictionary || !store_->entries(kwargs).empty())
                fail("NEWOBJ_EX keyword arguments are not allowed");
        }
        const auto args = pop(); const auto global = pop();
        if (store_->kind(global) != ValueKind::Global || store_->kind(args) != ValueKind::Tuple
            || store_->text(global) == "builtins.bytearray")
            fail("invalid legacy object construction");
        push(store_->createObject(store_->text(global)));
    }
    void buildObject() {
        const auto state = pop(); const auto object = top();
        if (object == MarkerValueId || store_->kind(object) != ValueKind::Object
            || store_->kind(state) != ValueKind::Dictionary)
            fail("BUILD requires a legacy object and dictionary state");
        std::unordered_set<std::string> names;
        for (const auto& [name, _] : store_->object(object).attributes) names.insert(name);
        const auto fields = store_->entries(state);
        for (const auto& [key, _] : fields) {
            if (store_->kind(key) != ValueKind::String)
                fail("legacy object fields must be strings");
            if (!names.insert(store_->text(key)).second)
                fail("legacy object contains a duplicate field");
        }
        store_->appendEntries(object, fields);
    }
    void reduce() {
        const auto args = pop(); const auto callable = pop();
        const auto callableName = store_->kind(callable) == ValueKind::Global
            ? store_->text(callable) : std::string{};
        const auto arguments = store_->kind(args) == ValueKind::Tuple
            ? store_->items(args) : std::vector<ValueId>{};
        if (callableName != "builtins.bytearray" || store_->kind(args) != ValueKind::Tuple
            || arguments.size() > 1u
            || (!arguments.empty() && store_->kind(arguments.front()) != ValueKind::Bytes))
            fail("executable REDUCE operation is not allowed");
        push(arguments.empty() ? store_->createBytes({}) : arguments.front());
    }

    std::ifstream input_{};
    std::array<char, 64u << 10> inputBuffer_{};
    std::size_t bufferPosition_ = 0;
    std::size_t bufferSize_ = 0;
    std::uint64_t inputSize_ = 0;
    std::shared_ptr<ValueStore> store_{};
    PickleLimits limits_{};
    bool disabled_ = false;
    std::uint64_t position_ = 0;
    std::uint32_t protocol_ = 0;
    std::uint64_t rawBytes_ = 0;
    std::uint64_t containerEntries_ = 0;
    std::vector<ValueId> stack_{};
    std::vector<ValueId> memo_{};
    std::function<void(std::uint64_t, std::uint64_t)> progress_{};
    std::function<bool(std::span<const std::byte>)> inputChunk_{};
    std::uint64_t nextProgress_ = 0;
};

}  // namespace

PickleResult readLegacyPickle(const std::filesystem::path& path,
    const std::filesystem::path& spoolPath, const PickleLimits& limits,
    const bool disableResourceLimits,
    const std::function<void(std::uint64_t, std::uint64_t)>& progress,
    const std::function<bool(std::span<const std::byte>)>& inputChunk) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) return {nullptr, 0, "input size could not be read", 0};
    if (!disableResourceLimits && size > limits.maxInputBytes)
        return {nullptr, 0, "input resource limit exceeded", 0};
    auto store = std::make_shared<ValueStore>(spoolPath);
    if (!store->valid()) return {nullptr, 0, store->error(), 0};
    return Reader(path, size, std::move(store), limits, disableResourceLimits,
        progress, inputChunk).read();
}

ValueId dictionaryValue(const ValueStore& store, const ValueId dictionary,
    const std::string_view key) {
    if (dictionary == InvalidValueId || store.kind(dictionary) != ValueKind::Dictionary)
        return InvalidValueId;
    for (const auto& [candidate, value] : store.entries(dictionary))
        if (store.kind(candidate) == ValueKind::String && store.text(candidate) == key)
            return value;
    return InvalidValueId;
}

ValueId dictionaryValue(const ValueStore::ReadSession& store,
    const ValueId dictionary, const std::string_view key) {
    if (dictionary == InvalidValueId || store.kind(dictionary) != ValueKind::Dictionary)
        return InvalidValueId;
    for (const auto& [candidate, value] : store.entries(dictionary))
        if (store.kind(candidate) == ValueKind::String && store.text(candidate) == key)
            return value;
    return InvalidValueId;
}
ValueId attributeValue(const ValueStore& store, const ValueId object,
    const std::string_view key) {
    if (object == InvalidValueId || store.kind(object) != ValueKind::Object)
        return InvalidValueId;
    return store.objectAttribute(object, key);
}

ValueId attributeValue(const ValueStore::ReadSession& store,
    const ValueId object, const std::string_view key) {
    if (object == InvalidValueId || store.kind(object) != ValueKind::Object)
        return InvalidValueId;
    return store.objectAttribute(object, key);
}
std::string valueString(const ValueStore& store, const ValueId value) {
    if (value == InvalidValueId) return {};
    const auto kind = store.kind(value);
    return kind == ValueKind::String || kind == ValueKind::Integer
        ? store.text(value) : std::string{};
}

std::string valueString(const ValueStore::ReadSession& store, const ValueId value) {
    if (value == InvalidValueId) return {};
    const auto kind = store.kind(value);
    return kind == ValueKind::String || kind == ValueKind::Integer
        ? store.text(value) : std::string{};
}

}  // namespace salsa::legacy
