#include "uife/drivers/fs/BtrfsSendFsParser.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <iterator>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace uife {
namespace {

constexpr std::array<std::byte, 13> stream_magic{
    std::byte{'b'}, std::byte{'t'}, std::byte{'r'}, std::byte{'f'},
    std::byte{'s'}, std::byte{'-'}, std::byte{'s'}, std::byte{'t'},
    std::byte{'r'}, std::byte{'e'}, std::byte{'a'}, std::byte{'m'},
    std::byte{0}};
constexpr std::size_t stream_header_size = 17;
constexpr std::size_t command_header_size = 10;
constexpr std::uint32_t maximum_stream_version = 3;
constexpr std::uint32_t maximum_command_size = 64U * 1024U * 1024U;
constexpr std::uint32_t maximum_attribute_size = 16U * 1024U * 1024U;
constexpr std::uint32_t maximum_commands = 4U * 1024U * 1024U;

enum class Command : std::uint16_t {
    subvolume = 1,
    snapshot = 2,
    make_file = 3,
    make_directory = 4,
    make_node = 5,
    make_fifo = 6,
    make_socket = 7,
    symlink = 8,
    rename = 9,
    link = 10,
    unlink = 11,
    remove_directory = 12,
    set_xattr = 13,
    remove_xattr = 14,
    write = 15,
    clone = 16,
    truncate = 17,
    chmod = 18,
    chown = 19,
    utimes = 20,
    end = 21,
    update_extent = 22,
    fallocate = 23,
    set_file_attributes = 24,
    encoded_write = 25,
    enable_verity = 26
};

enum class Attribute : std::uint16_t {
    uuid = 1,
    transaction_id = 2,
    inode = 3,
    size = 4,
    mode = 5,
    uid = 6,
    gid = 7,
    rdev = 8,
    ctime = 9,
    mtime = 10,
    atime = 11,
    xattr_name = 13,
    xattr_data = 14,
    path = 15,
    path_to = 16,
    path_link = 17,
    file_offset = 18,
    data = 19
};

struct AttributeValue {
    std::vector<std::byte> value;
    std::uint64_t data_offset{};
    std::uint64_t data_size{};
    bool is_stream_data{};
};

using Attributes = std::map<Attribute, AttributeValue>;

std::uint16_t read_le16(std::span<const std::byte> bytes,
                        std::size_t offset) {
    return static_cast<std::uint16_t>(
        std::to_integer<unsigned char>(bytes[offset]) |
        (std::to_integer<unsigned char>(bytes[offset + 1]) << 8U));
}

std::uint32_t read_le32(std::span<const std::byte> bytes,
                        std::size_t offset) {
    std::uint32_t value = 0;
    for (unsigned int index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(
            std::to_integer<unsigned char>(bytes[offset + index])) <<
            (index * 8U);
    }
    return value;
}

std::uint64_t read_le64(std::span<const std::byte> bytes,
                        std::size_t offset) {
    std::uint64_t value = 0;
    for (unsigned int index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(
            std::to_integer<unsigned char>(bytes[offset + index])) <<
            (index * 8U);
    }
    return value;
}

std::uint32_t crc32c_update(std::uint32_t crc,
                            std::span<const std::byte> bytes) noexcept {
    for (const auto byte : bytes) {
        crc ^= std::to_integer<unsigned char>(byte);
        for (unsigned int bit = 0; bit < 8; ++bit) {
            const auto mask = static_cast<std::uint32_t>(
                -static_cast<std::int32_t>(crc & 1U));
            crc = (crc >> 1U) ^ (0x82f63b78U & mask);
        }
    }
    return crc;
}

std::string read_string(const AttributeValue& attribute,
                        std::string_view name) {
    if (attribute.value.empty() ||
        std::find(attribute.value.begin(), attribute.value.end(), std::byte{0}) !=
            attribute.value.end()) {
        throw std::runtime_error("Btrfs send 流中的 " + std::string(name) +
                                 " 路径属性无效");
    }
    return std::string(
        reinterpret_cast<const char*>(attribute.value.data()),
        attribute.value.size());
}

std::uint64_t read_integer(const Attributes& attributes, Attribute key,
                           std::size_t expected_size,
                           std::string_view label) {
    const auto found = attributes.find(key);
    if (found == attributes.end() ||
        found->second.value.size() != expected_size) {
        throw std::runtime_error("Btrfs send 流缺少有效的 " +
                                 std::string(label) + " 属性");
    }
    if (expected_size == 4) {
        return read_le32(found->second.value, 0);
    }
    if (expected_size == 8) {
        return read_le64(found->second.value, 0);
    }
    throw std::logic_error("unsupported Btrfs integer attribute size");
}

std::uint32_t read_u32_value(const Attributes& attributes, Attribute key,
                             std::string_view label) {
    const auto value = read_integer(attributes, key, 8, label);
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("Btrfs send " + std::string(label) +
                                 " 超出当前元数据模型范围");
    }
    return static_cast<std::uint32_t>(value);
}

std::string path_from_attribute(const Attributes& attributes, Attribute key,
                                std::string_view label) {
    const auto found = attributes.find(key);
    if (found == attributes.end()) {
        throw std::runtime_error("Btrfs send 流命令缺少 " +
                                 std::string(label) + " 属性");
    }
    const auto relative = read_string(found->second, label);
    if (relative.front() == '/' || relative.find('\\') != std::string::npos ||
        relative.find(':') != std::string::npos) {
        throw std::runtime_error("Btrfs send 流包含不安全的绝对或平台特定路径");
    }
    std::string path = "/";
    std::size_t start = 0;
    while (start < relative.size()) {
        const auto end = relative.find('/', start);
        const auto component = std::string_view(relative).substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (component.empty() || component == "." || component == "..") {
            throw std::runtime_error("Btrfs send 流包含不安全的路径分量");
        }
        if (path.size() > 1) {
            path.push_back('/');
        }
        path.append(component);
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return path;
}

std::string parent_path(std::string_view path) {
    const auto separator = path.find_last_of('/');
    return separator == 0 ? "/" : std::string(path.substr(0, separator));
}

std::string base_name(std::string_view path) {
    return std::string(path.substr(path.find_last_of('/') + 1));
}

std::chrono::system_clock::time_point read_time(
    const AttributeValue& attribute, std::string_view label) {
    if (attribute.value.size() != 12) {
        throw std::runtime_error("Btrfs send 流中的 " + std::string(label) +
                                 " 时间属性长度无效");
    }
    const auto seconds = std::bit_cast<std::int64_t>(
        read_le64(attribute.value, 0));
    const auto nanoseconds = read_le32(attribute.value, 8);
    const auto minimum_seconds =
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::duration::min()).count();
    const auto maximum_seconds =
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::duration::max()).count();
    if (nanoseconds >= 1'000'000'000U ||
        seconds <= minimum_seconds || seconds >= maximum_seconds) {
        throw std::runtime_error("Btrfs send 流时间属性超出支持范围");
    }
    return std::chrono::system_clock::time_point(
        std::chrono::seconds(seconds) +
        std::chrono::nanoseconds(nanoseconds));
}

} // namespace

struct BtrfsSendFsParser::Impl {
    struct Extent {
        std::uint64_t file_offset{};
        std::uint64_t data_offset{};
        std::uint64_t length{};
    };

    std::shared_ptr<IImageDriver> image;
    std::uint64_t source_offset{};
    std::uint64_t source_length{};
    std::uint32_t version{};
    bool ended{};
    std::map<std::string, VFSNode> nodes;
    std::map<std::string, std::vector<Extent>> extents;
};

BtrfsSendFsParser::BtrfsSendFsParser() = default;
BtrfsSendFsParser::~BtrfsSendFsParser() = default;

void BtrfsSendFsParser::mount(std::shared_ptr<IImageDriver> image,
                              std::uint64_t byte_offset,
                              std::uint64_t byte_length) {
    if (!image || byte_offset > image->get_size() ||
        byte_length > image->get_size() - byte_offset) {
        throw std::invalid_argument("Btrfs send 流挂载范围超出镜像边界");
    }
    if (byte_length < stream_header_size) {
        throw std::runtime_error("Btrfs send 流头部不完整");
    }
    std::array<std::byte, stream_header_size> header{};
    if (image->read_at(byte_offset, header) != header.size() ||
        !std::equal(stream_magic.begin(), stream_magic.end(), header.begin())) {
        throw std::runtime_error("Btrfs send 流签名无效");
    }
    const auto version = read_le32(header, 13);
    if (version == 0 || version > maximum_stream_version) {
        throw std::runtime_error("不支持的 Btrfs send 流版本: " +
                                 std::to_string(version));
    }

    auto state = std::make_unique<Impl>();
    state->image = std::move(image);
    state->source_offset = byte_offset;
    state->source_length = byte_length;
    state->version = version;
    state->nodes.emplace("/", VFSNode{".", "/", NodeType::directory, {}});
    std::uint64_t cursor = stream_header_size;
    std::uint32_t command_count = 0;
    bool subvolume_seen = false;

    while (cursor < byte_length) {
        if (++command_count > maximum_commands ||
            byte_length - cursor < command_header_size) {
            throw std::runtime_error("Btrfs send 流命令数量或头部无效");
        }
        std::array<std::byte, command_header_size> command_header{};
        if (state->image->read_at(byte_offset + cursor, command_header) !=
            command_header.size()) {
            throw std::runtime_error("读取 Btrfs send 命令头失败");
        }
        const auto payload_size = read_le32(command_header, 0);
        const auto command_id = read_le16(command_header, 4);
        const auto expected_crc = read_le32(command_header, 6);
        if (payload_size > maximum_command_size ||
            payload_size > byte_length - cursor - command_header_size) {
            throw std::runtime_error("Btrfs send 命令长度无效或超出镜像范围");
        }
        if ((version == 1 && command_id > 22) ||
            (version == 2 && command_id > 25) ||
            (version == 3 && command_id > 26)) {
            throw std::runtime_error("Btrfs send 命令超出流版本支持范围");
        }

        auto crc_header = command_header;
        std::fill(crc_header.begin() + 6, crc_header.end(), std::byte{0});
        auto crc = crc32c_update(0, crc_header);
        std::array<std::byte, 64U * 1024U> crc_buffer{};
        std::uint64_t crc_cursor = 0;
        while (crc_cursor < payload_size) {
            const auto count = static_cast<std::size_t>(
                std::min<std::uint64_t>(payload_size - crc_cursor,
                                        crc_buffer.size()));
            const auto read = state->image->read_at(
                byte_offset + cursor + command_header_size + crc_cursor,
                std::span<std::byte>(crc_buffer).first(count));
            if (read != count) {
                throw std::runtime_error("读取 Btrfs send 命令负载失败");
            }
            crc = crc32c_update(
                crc, std::span<const std::byte>(crc_buffer).first(count));
            crc_cursor += count;
        }
        if (crc != expected_crc) {
            throw std::runtime_error("Btrfs send 命令 CRC32C 校验失败");
        }

        Attributes attributes;
        std::uint64_t payload_cursor = 0;
        const auto payload_start = cursor + command_header_size;
        bool has_data_attribute = false;
        while (payload_cursor < payload_size) {
            if (payload_size - payload_cursor < sizeof(std::uint16_t)) {
                throw std::runtime_error("Btrfs send 属性头不完整");
            }
            std::array<std::byte, 4> attribute_header{};
            if (state->image->read_at(
                    byte_offset + payload_start + payload_cursor,
                    std::span<std::byte>(attribute_header).first(
                        sizeof(std::uint16_t))) != sizeof(std::uint16_t)) {
                throw std::runtime_error("读取 Btrfs send 属性头失败");
            }
            const auto attribute_id = read_le16(attribute_header, 0);
            payload_cursor += sizeof(std::uint16_t);
            std::uint64_t value_size = 0;
            if (attribute_id == static_cast<std::uint16_t>(Attribute::data) &&
                version >= 2) {
                value_size = payload_size - payload_cursor;
                has_data_attribute = true;
            } else {
                if (payload_size - payload_cursor < sizeof(std::uint16_t) ||
                    state->image->read_at(
                        byte_offset + payload_start + payload_cursor,
                        std::span<std::byte>(attribute_header).subspan(
                            sizeof(std::uint16_t),
                            sizeof(std::uint16_t))) != sizeof(std::uint16_t)) {
                    throw std::runtime_error(
                        "Btrfs send 属性长度字段缺失");
                }
                value_size = read_le16(attribute_header, 2);
                payload_cursor += sizeof(std::uint16_t);
            }
            if (value_size > payload_size - payload_cursor) {
                throw std::runtime_error("Btrfs send 属性长度超出命令负载");
            }
            if (attribute_id == static_cast<std::uint16_t>(Attribute::data)) {
                AttributeValue data;
                data.data_offset = byte_offset + payload_start + payload_cursor;
                data.data_size = value_size;
                data.is_stream_data = true;
                if (!attributes.emplace(Attribute::data, std::move(data)).second) {
                    throw std::runtime_error("Btrfs send 命令包含重复 DATA 属性");
                }
            } else {
                if (value_size > maximum_attribute_size) {
                    throw std::runtime_error("Btrfs send 元数据属性过大");
                }
                AttributeValue value;
                value.value.resize(static_cast<std::size_t>(value_size));
                if (value_size != 0 &&
                    state->image->read_at(
                        byte_offset + payload_start + payload_cursor,
                        value.value) != value.value.size()) {
                    throw std::runtime_error("读取 Btrfs send 属性数据失败");
                }
                if (!attributes.emplace(static_cast<Attribute>(attribute_id),
                                        std::move(value)).second) {
                    throw std::runtime_error("Btrfs send 命令包含重复属性");
                }
            }
            payload_cursor += value_size;
            if (has_data_attribute) {
                break;
            }
        }

        const auto command = static_cast<Command>(command_id);
        const auto find_node = [&state](const std::string& path) -> VFSNode& {
            const auto found = state->nodes.find(path);
            if (found == state->nodes.end()) {
                throw std::runtime_error("Btrfs send 流引用了不存在的路径: " +
                                         path);
            }
            return found->second;
        };
        const auto require_parent = [&find_node](const std::string& path) {
            const auto& parent = find_node(parent_path(path));
            if (parent.type != NodeType::directory) {
                throw std::runtime_error(
                    "Btrfs send 路径的父项不是目录: " + parent.path);
            }
        };
        const auto create_node = [&state, &require_parent, &attributes](
                                     const std::string& path, NodeType type) {
            if (state->nodes.contains(path)) {
                throw std::runtime_error("Btrfs send 流重复创建路径: " + path);
            }
            require_parent(path);
            VFSNode node;
            node.name = base_name(path);
            node.path = path;
            node.type = type;
            node.metadata.mode =
                read_u32_value(attributes, Attribute::mode, "MODE");
            const auto inode = attributes.find(Attribute::inode);
            if (inode == attributes.end() || inode->second.value.size() != 8) {
                throw std::runtime_error("Btrfs send 命令缺少有效的 INO 属性");
            }
            node.metadata.inode = read_le64(inode->second.value, 0);
            node.metadata.hard_link_group = node.metadata.inode;
            state->nodes.emplace(path, std::move(node));
        };
        const auto path_attr = [&attributes](Attribute key,
                                             std::string_view label) {
            return path_from_attribute(attributes, key, label);
        };
        const auto sync_inode_metadata = [&state](const std::string& path) {
            const auto source = state->nodes.find(path);
            if (source == state->nodes.end() ||
                source->second.metadata.inode == 0) {
                return;
            }
            const auto metadata = source->second.metadata;
            for (auto& [alias_path, alias] : state->nodes) {
                if (alias_path != path &&
                    alias.type == NodeType::regular_file &&
                    alias.metadata.inode == metadata.inode) {
                    alias.metadata = metadata;
                }
            }
        };

        switch (command) {
        case Command::subvolume:
            if (subvolume_seen || command_count != 1) {
                throw std::runtime_error(
                    "Btrfs send 流必须以单个 SUBVOL 命令开始");
            }
            (void)path_attr(Attribute::path, "PATH");
            subvolume_seen = true;
            break;
        case Command::snapshot:
            throw std::runtime_error(
                "Btrfs 增量 SNAPSHOT send 流需要基准子卷，当前不支持");
        case Command::make_file:
        case Command::make_directory:
        case Command::make_node:
        case Command::make_fifo:
        case Command::make_socket:
        case Command::symlink: {
            if (!subvolume_seen) {
                throw std::runtime_error("Btrfs send 流在 SUBVOL 前包含文件命令");
            }
            const auto path = path_attr(Attribute::path, "PATH");
            const auto type = command == Command::make_directory
                ? NodeType::directory
                : command == Command::make_file ? NodeType::regular_file
                : command == Command::symlink ? NodeType::symbolic_link
                                              : NodeType::other;
            create_node(path, type);
            auto& node = find_node(path);
            if (command == Command::symlink) {
                node.metadata.symlink_target =
                    read_string(attributes.at(Attribute::path_link), "PATH_LINK");
            }
            if (type == NodeType::regular_file) {
                node.metadata.size = 0;
            }
            break;
        }
        case Command::rename: {
            const auto from = path_attr(Attribute::path, "PATH");
            const auto to = path_attr(Attribute::path_to, "PATH_TO");
            const auto destination_prefix = to + "/";
            const auto destination_entry =
                state->nodes.lower_bound(destination_prefix);
            if (to == from ||
                (to.size() > from.size() &&
                 to.compare(0, from.size(), from) == 0 &&
                 to[from.size()] == '/') ||
                state->nodes.contains(to) ||
                (destination_entry != state->nodes.end() &&
                 destination_entry->first.compare(
                     0, destination_prefix.size(), destination_prefix) == 0)) {
                throw std::runtime_error("Btrfs RENAME 目标路径已存在: " + to);
            }
            require_parent(to);
            (void)find_node(from);
            std::vector<std::pair<std::string, VFSNode>> moved;
            for (auto item = state->nodes.lower_bound(from);
                 item != state->nodes.end() &&
                 (item->first == from ||
                  (item->first.size() > from.size() &&
                   item->first.compare(0, from.size(), from) == 0 &&
                   item->first[from.size()] == '/'));) {
                auto node = item->second;
                const auto new_path = to + item->first.substr(from.size());
                node.path = new_path;
                node.name = base_name(new_path);
                moved.emplace_back(new_path, std::move(node));
                const auto existing_extents = state->extents.find(item->first);
                if (existing_extents != state->extents.end()) {
                    state->extents.emplace(
                        new_path, std::move(existing_extents->second));
                    state->extents.erase(existing_extents);
                }
                item = state->nodes.erase(item);
            }
            for (auto& item : moved) {
                if (!state->nodes.emplace(item.first, std::move(item.second)).second) {
                    throw std::runtime_error("Btrfs RENAME 目标路径冲突");
                }
            }
            break;
        }
        case Command::link: {
            const auto path = path_attr(Attribute::path, "PATH");
            const auto target = path_attr(Attribute::path_link, "PATH_LINK");
            if (state->nodes.contains(path)) {
                throw std::runtime_error("Btrfs LINK 路径已存在: " + path);
            }
            require_parent(path);
            const auto& original = find_node(target);
            if (original.type != NodeType::regular_file) {
                throw std::runtime_error("Btrfs LINK 目标不是普通文件");
            }
            auto node = original;
            node.name = base_name(path);
            node.path = path;
            node.metadata.hard_link_group =
                original.metadata.inode != 0 ? original.metadata.inode
                                             : original.metadata.hard_link_group;
            for (auto& [existing_path, existing_node] : state->nodes) {
                (void)existing_path;
                if (existing_node.metadata.inode == node.metadata.inode) {
                    ++existing_node.metadata.link_count;
                    node.metadata.link_count =
                        existing_node.metadata.link_count;
                }
            }
            state->nodes.emplace(path, std::move(node));
            state->extents[path] = state->extents[target];
            break;
        }
        case Command::unlink:
        case Command::remove_directory: {
            const auto path = path_attr(Attribute::path, "PATH");
            auto& node = find_node(path);
            const auto inode = node.metadata.inode;
            const auto node_type = node.type;
            if ((command == Command::remove_directory) !=
                (node_type == NodeType::directory)) {
                throw std::runtime_error("Btrfs 删除命令与节点类型不匹配");
            }
            if (command == Command::unlink &&
                node_type == NodeType::directory) {
                throw std::runtime_error("Btrfs UNLINK 不能删除目录");
            }
            const auto child_prefix = path + "/";
            const auto first_child = state->nodes.lower_bound(child_prefix);
            if (first_child != state->nodes.end() &&
                first_child->first.compare(0, child_prefix.size(),
                                           child_prefix) == 0) {
                throw std::runtime_error(command == Command::remove_directory
                    ? "Btrfs RMDIR 命令不能删除非空目录"
                    : "Btrfs UNLINK 命令目标意外包含子路径");
            }
            for (auto item = state->nodes.lower_bound(path);
                 item != state->nodes.end() &&
                 (item->first == path ||
                  (item->first.size() > path.size() &&
                   item->first.compare(0, path.size(), path) == 0 &&
                   item->first[path.size()] == '/'));) {
                state->extents.erase(item->first);
                item = state->nodes.erase(item);
            }
            if (command == Command::unlink && inode != 0) {
                for (auto& [remaining_path, remaining_node] : state->nodes) {
                    (void)remaining_path;
                    if (remaining_node.metadata.inode == inode &&
                        remaining_node.metadata.link_count > 1) {
                        --remaining_node.metadata.link_count;
                    }
                }
            }
            break;
        }
        case Command::set_xattr:
        case Command::remove_xattr: {
            const auto path = path_attr(Attribute::path, "PATH");
            auto& node = find_node(path);
            const auto name = read_string(attributes.at(Attribute::xattr_name),
                                           "XATTR_NAME");
            if (command == Command::remove_xattr) {
                node.metadata.extended_attributes.erase(name);
            } else {
                const auto& value = attributes.at(Attribute::xattr_data).value;
                std::string value_string;
                if (!value.empty()) {
                    value_string.assign(
                        reinterpret_cast<const char*>(value.data()),
                        value.size());
                }
                node.metadata.extended_attributes.insert_or_assign(
                    name, std::move(value_string));
            }
            sync_inode_metadata(path);
            break;
        }
        case Command::write: {
            const auto path = path_attr(Attribute::path, "PATH");
            auto& node = find_node(path);
            if (node.type != NodeType::regular_file) {
                throw std::runtime_error("Btrfs WRITE 目标不是普通文件");
            }
            const auto file_offset = read_integer(
                attributes, Attribute::file_offset, 8, "FILE_OFFSET");
            const auto& data = attributes.at(Attribute::data);
            if (!data.is_stream_data ||
                data.data_size > std::numeric_limits<std::uint64_t>::max() -
                                     file_offset) {
                throw std::runtime_error("Btrfs WRITE 数据范围无效");
            }
            const auto end = file_offset + data.data_size;
            auto& file_extents = state->extents[path];
            const auto prior_size = node.metadata.size;
            std::vector<Impl::Extent> replacement;
            replacement.reserve(file_extents.size() + 1);
            for (const auto& extent : file_extents) {
                const auto extent_end = extent.file_offset + extent.length;
                if (extent_end <= file_offset || extent.file_offset >= end) {
                    replacement.push_back(extent);
                    continue;
                }
                if (extent.file_offset < file_offset) {
                    replacement.push_back(
                        {extent.file_offset, extent.data_offset,
                         file_offset - extent.file_offset});
                }
                if (extent_end > end) {
                    replacement.push_back(
                        {end, extent.data_offset + end - extent.file_offset,
                         extent_end - end});
                }
            }
            if (data.data_size != 0) {
                replacement.push_back(
                    {file_offset, data.data_offset, data.data_size});
            }
            std::sort(replacement.begin(), replacement.end(),
                      [](const auto& left, const auto& right) {
                          return left.file_offset < right.file_offset;
                      });
            file_extents = std::move(replacement);
            node.metadata.size = std::max(node.metadata.size, end);
            node.metadata.sparse = node.metadata.sparse ||
                                   file_offset > prior_size;
            for (const auto& [other_path, other_node] : state->nodes) {
                if (other_path != path &&
                    other_node.type == NodeType::regular_file &&
                    other_node.metadata.inode == node.metadata.inode) {
                    state->extents[other_path] = file_extents;
                }
            }
            sync_inode_metadata(path);
            break;
        }
        case Command::truncate: {
            const auto path = path_attr(Attribute::path, "PATH");
            auto& node = find_node(path);
            if (node.type != NodeType::regular_file) {
                throw std::runtime_error("Btrfs TRUNCATE 目标不是普通文件");
            }
            const auto size = read_integer(attributes, Attribute::size, 8, "SIZE");
            const auto prior_size = node.metadata.size;
            node.metadata.size = size;
            auto& file_extents = state->extents[path];
            std::vector<Impl::Extent> clipped;
            for (const auto& extent : file_extents) {
                if (extent.file_offset >= size) {
                    continue;
                }
                auto retained = extent;
                retained.length = std::min(retained.length,
                                           size - retained.file_offset);
                clipped.push_back(retained);
            }
            file_extents = std::move(clipped);
            node.metadata.sparse = node.metadata.sparse || size > prior_size;
            for (const auto& [other_path, other_node] : state->nodes) {
                if (other_path != path &&
                    other_node.type == NodeType::regular_file &&
                    other_node.metadata.inode == node.metadata.inode) {
                    state->extents[other_path] = file_extents;
                    state->nodes.at(other_path).metadata.size = size;
                    state->nodes.at(other_path).metadata.sparse =
                        node.metadata.sparse;
                }
            }
            sync_inode_metadata(path);
            break;
        }
        case Command::chmod:
        case Command::chown:
        case Command::utimes: {
            const auto path = path_attr(Attribute::path, "PATH");
            auto& node = find_node(path);
            if (command == Command::chmod) {
                node.metadata.mode =
                    read_u32_value(attributes, Attribute::mode, "MODE");
            } else if (command == Command::chown) {
                node.metadata.uid =
                    read_u32_value(attributes, Attribute::uid, "UID");
                node.metadata.gid =
                    read_u32_value(attributes, Attribute::gid, "GID");
                node.metadata.has_owner = true;
            } else {
                for (const auto [attribute, label, value, flag] :
                     {std::tuple{Attribute::atime, "ATIME",
                                 &node.metadata.accessed,
                                 &node.metadata.has_accessed_time},
                      std::tuple{Attribute::mtime, "MTIME",
                                 &node.metadata.modified,
                                 &node.metadata.has_modified_time},
                      std::tuple{Attribute::ctime, "CTIME",
                                 &node.metadata.created,
                                 &node.metadata.has_created_time}}) {
                    const auto time = attributes.find(attribute);
                    if (time != attributes.end()) {
                        *value = read_time(time->second, label);
                        *flag = true;
                    }
                }
            }
            sync_inode_metadata(path);
            break;
        }
        case Command::update_extent:
        case Command::set_file_attributes:
            throw std::runtime_error(
                "Btrfs send 流包含当前无法安全还原的 extent 或 inode 属性命令");
        case Command::end:
            if (!subvolume_seen || cursor + command_header_size + payload_size !=
                                       byte_length) {
                throw std::runtime_error(
                    "Btrfs END 命令必须存在且是流中的最后一条命令");
            }
            state->ended = true;
            break;
        case Command::clone:
        case Command::fallocate:
        case Command::encoded_write:
        case Command::enable_verity:
            throw std::runtime_error(
                "Btrfs send 流包含当前不支持的增量、预分配或编码数据命令");
        default:
            throw std::runtime_error("Btrfs send 流包含未知命令: " +
                                     std::to_string(command_id));
        }
        cursor += command_header_size + payload_size;
        if (state->ended) {
            break;
        }
    }
    if (!state->ended) {
        throw std::runtime_error("Btrfs send 流缺少 END 命令");
    }
    impl_ = std::move(state);
}

std::vector<VFSNode> BtrfsSendFsParser::list_dir(
    const std::string& path) const {
    if (!impl_) {
        throw std::runtime_error("Btrfs send 流尚未挂载");
    }
    const auto directory = impl_->nodes.find(path);
    if (directory == impl_->nodes.end() ||
        directory->second.type != NodeType::directory) {
        throw std::runtime_error("Btrfs send 路径不是目录: " + path);
    }
    std::vector<VFSNode> entries;
    const auto prefix = path == "/" ? "/" : path + "/";
    for (auto item = impl_->nodes.upper_bound(path);
         item != impl_->nodes.end() &&
         item->first.compare(0, prefix.size(), prefix) == 0; ++item) {
        const auto remainder = std::string_view(item->first).substr(prefix.size());
        if (!remainder.empty() && remainder.find('/') == std::string_view::npos) {
            entries.push_back(item->second);
        }
    }
    return entries;
}

VFSNode BtrfsSendFsParser::get_metadata(const std::string& path) const {
    if (!impl_) {
        throw std::runtime_error("Btrfs send 流尚未挂载");
    }
    const auto found = impl_->nodes.find(path);
    if (found == impl_->nodes.end()) {
        throw std::runtime_error("Btrfs send 路径不存在: " + path);
    }
    return found->second;
}

std::size_t BtrfsSendFsParser::read_file(
    const std::string& path, std::uint64_t offset,
    std::span<std::byte> destination) const {
    if (!impl_) {
        throw std::runtime_error("Btrfs send 流尚未挂载");
    }
    const auto found = impl_->nodes.find(path);
    if (found == impl_->nodes.end() ||
        found->second.type != NodeType::regular_file) {
        throw std::runtime_error("Btrfs send 路径不是普通文件: " + path);
    }
    const auto file_size = found->second.metadata.size;
    if (offset > file_size) {
        throw std::out_of_range("Btrfs send 文件读取偏移超出文件大小");
    }
    const auto count = static_cast<std::size_t>(
        std::min<std::uint64_t>(destination.size(), file_size - offset));
    if (count == 0) {
        return 0;
    }
    std::fill(destination.begin(),
              destination.begin() + static_cast<std::ptrdiff_t>(count),
              std::byte{0});
    const auto file_end = offset + count;
    const auto extents = impl_->extents.find(path);
    if (extents == impl_->extents.end()) {
        return count;
    }
    for (const auto& extent : extents->second) {
        const auto extent_end = extent.file_offset + extent.length;
        const auto intersection_start = std::max(offset, extent.file_offset);
        const auto intersection_end = std::min(file_end, extent_end);
        if (intersection_start >= intersection_end) {
            continue;
        }
        const auto destination_offset = static_cast<std::size_t>(
            intersection_start - offset);
        const auto data_offset =
            extent.data_offset + intersection_start - extent.file_offset;
        const auto length = static_cast<std::size_t>(
            intersection_end - intersection_start);
        if (impl_->image->read_at(
                data_offset,
                destination.subspan(destination_offset, length)) != length) {
            throw std::runtime_error("读取 Btrfs send 流文件数据失败: " + path);
        }
    }
    return count;
}

std::string_view BtrfsSendFsParser::filesystem_name() const noexcept {
    return "Btrfs send stream";
}

} // namespace uife
