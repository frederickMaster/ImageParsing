#include "uife/drivers/fs/Iso9660.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>

namespace uife::iso9660 {
namespace {

constexpr std::uint32_t sector_size = 2048;
constexpr std::uint64_t maximum_directory_size = 64U * 1024U * 1024U;
constexpr std::uint32_t maximum_susp_area_size = 1024U * 1024U;
constexpr std::uint32_t maximum_susp_total_size = 4U * 1024U * 1024U;
constexpr std::size_t maximum_susp_continuations = 16;
constexpr std::size_t maximum_path_depth = 128;

std::uint32_t read_le32(const char* bytes) {
    const auto* value = reinterpret_cast<const unsigned char*>(bytes);
    return static_cast<std::uint32_t>(value[0]) |
           (static_cast<std::uint32_t>(value[1]) << 8U) |
           (static_cast<std::uint32_t>(value[2]) << 16U) |
           (static_cast<std::uint32_t>(value[3]) << 24U);
}

std::uint32_t read_be32(const char* bytes) {
    const auto* value = reinterpret_cast<const unsigned char*>(bytes);
    return (static_cast<std::uint32_t>(value[0]) << 24U) |
           (static_cast<std::uint32_t>(value[1]) << 16U) |
           (static_cast<std::uint32_t>(value[2]) << 8U) |
           static_cast<std::uint32_t>(value[3]);
}

std::uint32_t read_dual32(const char* bytes) {
    const auto little = read_le32(bytes);
    if (little != read_be32(bytes + 4)) {
        throw std::runtime_error("ISO 双端序 32 位字段不一致");
    }
    return little;
}

std::uint16_t read_dual16(const char* bytes) {
    const auto* value = reinterpret_cast<const unsigned char*>(bytes);
    const auto little = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(value[0]) |
        (static_cast<std::uint16_t>(value[1]) << 8U));
    const auto big = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(value[2]) << 8U) |
        static_cast<std::uint16_t>(value[3]));
    if (little != big) {
        throw std::runtime_error("ISO 双端序 16 位字段不一致");
    }
    return little;
}

struct RockRidgeAttributes {
    std::string name;
    std::uint32_t mode{};
    std::uint32_t uid{};
    std::uint32_t gid{};
    std::uint64_t link_count{1};
    std::uint64_t hard_link_group{};
    std::string symlink_target;
    bool has_name{};
    bool has_posix_metadata{};
    bool has_symlink_target{};
};

std::size_t system_use_offset(const char* record) {
    const auto identifier_length =
        static_cast<unsigned char>(record[32]);
    return 33U + identifier_length +
           ((identifier_length & 1U) == 0 ? 1U : 0U);
}

std::optional<std::uint8_t> rock_ridge_susp_skip(
    const char* record, std::size_t record_length) {
    if (record_length < 34 ||
        static_cast<unsigned char>(record[32]) != 1 ||
        static_cast<unsigned char>(record[33]) != 0) {
        return std::nullopt;
    }
    auto offset = system_use_offset(record);
    while (offset + 4 <= record_length) {
        const auto length = static_cast<unsigned char>(record[offset + 2]);
        if (length < 4 || length > record_length - offset) {
            throw std::runtime_error("ISO SUSP 扩展记录长度无效");
        }
        if (record[offset] == 'S' && record[offset + 1] == 'P') {
            if (length != 7 ||
                static_cast<unsigned char>(record[offset + 3]) != 1 ||
                static_cast<unsigned char>(record[offset + 4]) != 0xbe ||
                static_cast<unsigned char>(record[offset + 5]) != 0xef) {
                throw std::runtime_error("Rock Ridge SUSP SP 标记无效");
            }
            return static_cast<std::uint8_t>(record[offset + 6]);
        }
        offset += length;
    }
    return std::nullopt;
}

struct RockRidgeParseState {
    RockRidgeAttributes attributes;
    bool name_continues{};
    bool symlink_continues{};
    bool component_continues{};
    std::set<std::pair<std::uint64_t, std::uint32_t>> continuations;
    std::uint64_t continuation_bytes{};
};

using SuspContinuationReader =
    std::function<std::vector<char>(std::uint64_t, std::uint32_t)>;

void parse_rock_ridge_area(
    const char* record, std::size_t record_length, std::size_t offset,
    RockRidgeParseState& state,
    const SuspContinuationReader& read_continuation,
    std::uint64_t volume_bytes, std::size_t depth) {
    if (depth > maximum_susp_continuations) {
        throw std::runtime_error("Rock Ridge CE continuation 嵌套过深");
    }
    while (offset + 4 <= record_length) {
        const auto length = static_cast<unsigned char>(record[offset + 2]);
        const auto version = static_cast<unsigned char>(record[offset + 3]);
        if (length < 4 || length > record_length - offset || version != 1) {
            throw std::runtime_error("ISO SUSP 扩展记录格式无效");
        }
        if (record[offset] == 'S' && record[offset + 1] == 'T') {
            if (length != 4) {
                throw std::runtime_error("Rock Ridge SUSP ST 记录无效");
            }
            return;
        }
        if (record[offset] == 'C' && record[offset + 1] == 'E') {
            if (length != 28) {
                throw std::runtime_error("Rock Ridge CE continuation 记录无效");
            }
            const auto extent_lba = read_dual32(record + offset + 4);
            const auto area_offset = read_dual32(record + offset + 12);
            const auto area_length = read_dual32(record + offset + 20);
            if (extent_lba >= volume_bytes / sector_size ||
                area_offset >= sector_size || area_length == 0 ||
                area_length > maximum_susp_area_size) {
                throw std::runtime_error(
                    "Rock Ridge CE continuation 超出支持范围");
            }
            const auto start =
                static_cast<std::uint64_t>(extent_lba) * sector_size +
                area_offset;
            if (start > volume_bytes || area_length > volume_bytes - start ||
                state.continuation_bytes >
                    maximum_susp_total_size - area_length) {
                throw std::runtime_error(
                    "Rock Ridge CE continuation 超出卷边界或总量限制");
            }
            const auto location = std::make_pair(start, area_length);
            if (!state.continuations.insert(location).second) {
                throw std::runtime_error(
                    "Rock Ridge CE continuation 存在循环引用");
            }
            state.continuation_bytes += area_length;
            auto continuation =
                read_continuation(start, area_length);
            parse_rock_ridge_area(
                continuation.data(), continuation.size(), 0, state,
                read_continuation, volume_bytes, depth + 1U);
            offset += length;
            continue;
        }
        auto& attributes = state.attributes;
        if (state.symlink_continues &&
            !(record[offset] == 'S' && record[offset + 1] == 'L')) {
            throw std::runtime_error("Rock Ridge SL 符号链接扩展不连续");
        }
        if (record[offset] == 'N' && record[offset + 1] == 'M') {
            if (length < 5) {
                throw std::runtime_error("Rock Ridge NM 文件名扩展无效");
            }
            const auto flags =
                static_cast<unsigned char>(record[offset + 4]);
            constexpr unsigned char supported_flags = 0x01U;
            if ((flags & ~supported_flags) != 0) {
                throw std::runtime_error(
                    "当前版本不支持 Rock Ridge NM 特殊文件名标志");
            }
            const auto fragment_length = length - 5U;
            for (std::size_t index = 0; index < fragment_length; ++index) {
                const auto character = record[offset + 5U + index];
                if (character == '/' || character == '\\' ||
                    character == ':' || character == '\0') {
                    throw std::runtime_error(
                        "Rock Ridge 文件名包含当前 VFS 不支持的路径字符");
                }
                attributes.name.push_back(character);
            }
            attributes.has_name = true;
            state.name_continues = (flags & 0x01U) != 0;
        } else if (state.name_continues) {
            throw std::runtime_error("Rock Ridge NM 文件名片段不连续");
        } else if (record[offset] == 'P' && record[offset + 1] == 'X') {
            if ((length != 36 && length != 44) ||
                attributes.has_posix_metadata) {
                throw std::runtime_error("Rock Ridge PX 元数据扩展无效");
            }
            attributes.mode = read_dual32(record + offset + 4);
            attributes.link_count = read_dual32(record + offset + 12);
            attributes.uid = read_dual32(record + offset + 20);
            attributes.gid = read_dual32(record + offset + 28);
            if (length == 44) {
                attributes.hard_link_group =
                    read_dual32(record + offset + 36);
            }
            if (attributes.link_count == 0) {
                throw std::runtime_error("Rock Ridge PX 链接数无效");
            }
            const auto file_type = attributes.mode & 0170000U;
            if (file_type != 0100000U && file_type != 0040000U &&
                file_type != 0120000U) {
                throw std::runtime_error(
                    "当前版本不支持 Rock Ridge 特殊节点");
            }
            attributes.has_posix_metadata = true;
        } else if (record[offset] == 'S' && record[offset + 1] == 'L') {
            if (length < 5) {
                throw std::runtime_error("Rock Ridge SL 符号链接扩展无效");
            }
            const auto entry_flags =
                static_cast<unsigned char>(record[offset + 4]);
            if ((entry_flags & ~0x01U) != 0) {
                throw std::runtime_error(
                    "Rock Ridge SL 包含不支持的扩展标志");
            }
            attributes.has_symlink_target = true;
            state.symlink_continues = (entry_flags & 0x01U) != 0;
            auto component_offset = offset + 5U;
            while (component_offset < offset + length) {
                if (offset + length - component_offset < 2) {
                    throw std::runtime_error(
                        "Rock Ridge SL 路径组件头部被截断");
                }
                const auto component_flags =
                    static_cast<unsigned char>(record[component_offset]);
                const auto component_length =
                    static_cast<unsigned char>(record[component_offset + 1]);
                component_offset += 2;
                if (component_length > offset + length - component_offset) {
                    throw std::runtime_error(
                        "Rock Ridge SL 路径组件越界");
                }
                constexpr unsigned char component_continue = 0x01U;
                constexpr unsigned char component_special = 0x3eU;
                if ((component_flags &
                     ~(component_continue | component_special)) != 0) {
                    throw std::runtime_error(
                        "Rock Ridge SL 包含不支持的路径组件标志");
                }

                std::string component;
                const auto special_flags =
                    component_flags & component_special;
                if (special_flags != 0) {
                    if (component_length != 0 ||
                        state.component_continues ||
                        (special_flags & (special_flags - 1U)) != 0) {
                        throw std::runtime_error(
                            "Rock Ridge SL 特殊路径组件无效");
                    }
                    if (special_flags == 0x02U) {
                        component = ".";
                    } else if (special_flags == 0x04U) {
                        component = "..";
                    } else if (special_flags == 0x08U) {
                        if (!attributes.symlink_target.empty()) {
                            throw std::runtime_error(
                                "Rock Ridge SL 根组件位置无效");
                        }
                        attributes.symlink_target = "/";
                    } else {
                        throw std::runtime_error(
                            "当前版本不支持 Rock Ridge SL 卷根或主机组件");
                    }
                } else {
                    component.assign(record + component_offset,
                                     component_length);
                    if (component.find('\0') != std::string::npos ||
                        component.find('/') != std::string::npos) {
                        throw std::runtime_error(
                            "Rock Ridge SL 路径组件包含无效字符");
                    }
                }
                if (special_flags != 0 || !state.component_continues) {
                    if (!attributes.symlink_target.empty() &&
                        attributes.symlink_target.back() != '/') {
                        attributes.symlink_target.push_back('/');
                    }
                }
                attributes.symlink_target += component;
                state.component_continues =
                    (component_flags & component_continue) != 0;
                component_offset += component_length;
            }
        }
        offset += length;
    }
}

RockRidgeAttributes read_rock_ridge_attributes(
    const char* record, std::size_t record_length, std::uint8_t susp_skip,
    const SuspContinuationReader& read_continuation,
    std::uint64_t volume_bytes) {
    RockRidgeParseState state;
    const auto offset = system_use_offset(record) + susp_skip;
    if (offset > record_length) {
        throw std::runtime_error("Rock Ridge SUSP skip 字节超出目录记录");
    }
    parse_rock_ridge_area(record, record_length, offset, state,
                          read_continuation, volume_bytes, 0);
    auto& attributes = state.attributes;
    if (state.name_continues) {
        throw std::runtime_error("Rock Ridge NM 文件名片段不完整");
    }
    if (attributes.has_name && attributes.name.empty()) {
        throw std::runtime_error("Rock Ridge NM 文件名为空");
    }
    if (attributes.name == "." || attributes.name == "..") {
        throw std::runtime_error("Rock Ridge NM 文件名不能是路径特殊分量");
    }
    if (state.symlink_continues || state.component_continues ||
        (attributes.has_symlink_target &&
         attributes.symlink_target.empty())) {
        throw std::runtime_error("Rock Ridge SL 符号链接目标不完整");
    }
    if (attributes.has_posix_metadata &&
        ((attributes.mode & 0170000U) == 0120000U) !=
            attributes.has_symlink_target) {
        throw std::runtime_error(
            "Rock Ridge PX 符号链接类型与 SL 目标不匹配");
    }
    return attributes;
}

void append_utf8(std::string& output, std::uint32_t codepoint) {
    if (codepoint <= 0x7fU) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ffU) {
        output.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else if (codepoint <= 0xffffU) {
        output.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else {
        output.push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    }
}

std::string visible_name(const char* bytes, std::size_t length, bool joliet) {
    std::string name;
    if (!joliet) {
        name.assign(bytes, length);
    } else {
        if ((length & 1U) != 0) {
            throw std::runtime_error("Joliet 文件名长度不是偶数");
        }
        for (std::size_t index = 0; index < length; index += 2) {
            const auto high = static_cast<unsigned char>(bytes[index]);
            const auto low = static_cast<unsigned char>(bytes[index + 1]);
            std::uint32_t codepoint = (static_cast<std::uint32_t>(high) << 8U) | low;
            if (codepoint >= 0xd800U && codepoint <= 0xdbffU) {
                if (index + 3 >= length) {
                    throw std::runtime_error("Joliet 文件名包含无效 UTF-16 代理项");
                }
                const auto next_high = static_cast<unsigned char>(bytes[index + 2]);
                const auto next_low = static_cast<unsigned char>(bytes[index + 3]);
                const auto low_surrogate =
                    (static_cast<std::uint32_t>(next_high) << 8U) | next_low;
                if (low_surrogate < 0xdc00U || low_surrogate > 0xdfffU) {
                    throw std::runtime_error("Joliet 文件名包含无效 UTF-16 代理项");
                }
                codepoint = 0x10000U + ((codepoint - 0xd800U) << 10U) +
                            (low_surrogate - 0xdc00U);
                index += 2;
            } else if (codepoint >= 0xdc00U && codepoint <= 0xdfffU) {
                throw std::runtime_error("Joliet 文件名包含无效 UTF-16 代理项");
            }
            append_utf8(name, codepoint);
        }
    }
    if (const auto version = name.rfind(';'); version != std::string::npos) {
        name.erase(version);
    }
    if (name.size() > 1 && name.back() == '.') {
        name.pop_back();
    }
    return name;
}

std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> parts;
    std::size_t position = 0;
    while (position < path.size()) {
        while (position < path.size() && (path[position] == '/' || path[position] == '\\')) {
            ++position;
        }
        const auto end = path.find_first_of("/\\", position);
        const auto part = path.substr(position, end == std::string::npos
            ? std::string::npos : end - position);
        if (!part.empty() && part != ".") {
            if (part == "..") {
                throw std::invalid_argument("ISO 路径不允许包含 '..'");
            }
            parts.push_back(part);
        }
        if (end == std::string::npos) {
            break;
        }
        position = end + 1;
    }
    if (parts.size() > maximum_path_depth) {
        throw std::invalid_argument("ISO 路径层级过深");
    }
    return parts;
}

bool same_name(std::string left, std::string right) {
    std::transform(left.begin(), left.end(), left.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    std::transform(right.begin(), right.end(), right.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return left == right;
}

std::optional<std::chrono::system_clock::time_point> recording_time(
    const char* record) {
    const auto year = 1900 + static_cast<int>(
        static_cast<unsigned char>(record[18]));
    const std::chrono::year_month_day date{
        std::chrono::year{year},
        std::chrono::month{static_cast<unsigned int>(
            static_cast<unsigned char>(record[19]))},
        std::chrono::day{static_cast<unsigned int>(
            static_cast<unsigned char>(record[20]))}};
    const auto hour = static_cast<unsigned char>(record[21]);
    const auto minute = static_cast<unsigned char>(record[22]);
    const auto second = static_cast<unsigned char>(record[23]);
    const auto encoded_offset = static_cast<unsigned char>(record[24]);
    const auto offset_quarters = encoded_offset < 128U
        ? static_cast<int>(encoded_offset)
        : static_cast<int>(encoded_offset) - 256;
    if (!date.ok() || hour > 23U || minute > 59U || second > 59U ||
        offset_quarters < -48 || offset_quarters > 52) {
        return std::nullopt;
    }

    const auto local_time = std::chrono::sys_days{date} +
        std::chrono::hours{hour} + std::chrono::minutes{minute} +
        std::chrono::seconds{second};
    return std::chrono::system_clock::time_point{
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            local_time.time_since_epoch() -
            std::chrono::minutes{offset_quarters * 15})};
}

} // namespace

Iso9660::Iso9660(const std::filesystem::path& image_path,
                 std::uint64_t byte_offset,
                 std::uint64_t byte_length)
    : image_path_(image_path), byte_offset_(byte_offset) {
    std::error_code error;
    const auto physical_size = std::filesystem::file_size(image_path_, error);
    if (error) {
        throw std::runtime_error("无法读取 ISO 文件大小: " + image_path_.string());
    }
    if (byte_offset_ > physical_size) {
        throw std::invalid_argument("ISO 起始偏移超出镜像范围");
    }
    const auto available_size = physical_size - byte_offset_;
    image_size_ = byte_length == 0 ? available_size : byte_length;
    if (image_size_ > available_size) {
        throw std::invalid_argument("ISO 长度超出镜像范围");
    }
    initialize();
}

Iso9660::Iso9660(std::uint64_t source_size,
                 Reader reader,
                 std::uint64_t byte_offset,
                 std::uint64_t byte_length)
    : reader_(std::move(reader)), byte_offset_(byte_offset) {
    if (!reader_) {
        throw std::invalid_argument("ISO 随机读取接口不能为空");
    }
    if (byte_offset_ > source_size) {
        throw std::invalid_argument("ISO 起始偏移超出镜像范围");
    }
    const auto available_size = source_size - byte_offset_;
    image_size_ = byte_length == 0 ? available_size : byte_length;
    if (image_size_ > available_size) {
        throw std::invalid_argument("ISO 长度超出镜像范围");
    }
    initialize();
}

void Iso9660::initialize() {
    if (image_size_ < 17ULL * sector_size) {
        throw std::runtime_error("文件过小，不是有效的 ISO 9660 镜像");
    }

    std::array<char, sector_size> descriptor{};
    bool found_primary = false;
    IsoEntry primary_root;
    std::uint32_t primary_volume_sectors = 0;
    IsoEntry joliet_root;
    std::uint32_t joliet_volume_sectors = 0;
    unsigned int joliet_level = 0;
    for (std::uint32_t lba = 16; lba < 16 + 256; ++lba) {
        const auto offset = static_cast<std::uint64_t>(lba) * sector_size;
        if (offset + sector_size > image_size_) {
            break;
        }
        read_at(offset, descriptor.data(), descriptor.size());
        if (descriptor[1] != 'C' || descriptor[2] != 'D' || descriptor[3] != '0' ||
            descriptor[4] != '0' || descriptor[5] != '1' || descriptor[6] != 1) {
            throw std::runtime_error("ISO 卷描述符签名无效");
        }
        const auto descriptor_type = static_cast<unsigned char>(descriptor[0]);
        if (descriptor_type == 1 || descriptor_type == 2) {
            if (descriptor_type == 2) {
                unsigned int level = 0;
                if (descriptor[88] == '%' && descriptor[89] == '/' &&
                    descriptor[90] == '@') {
                    level = 1;
                } else if (descriptor[88] == '%' && descriptor[89] == '/' &&
                           descriptor[90] == 'C') {
                    level = 2;
                } else if (descriptor[88] == '%' && descriptor[89] == '/' &&
                           descriptor[90] == 'E') {
                    level = 3;
                }
                if (level == 0 || level <= joliet_level) {
                    continue;
                }
            }
            const auto volume_sectors = read_dual32(descriptor.data() + 80);
            if (read_dual16(descriptor.data() + 128) != sector_size) {
                throw std::runtime_error("当前仅支持 ISO 2048 字节逻辑扇区");
            }
            if (volume_sectors == 0 ||
                static_cast<std::uint64_t>(volume_sectors) * sector_size > image_size_) {
                throw std::runtime_error("ISO 卷容量超出镜像边界");
            }

            const auto* root = descriptor.data() + 156;
            const auto record_length = static_cast<unsigned char>(root[0]);
            if (record_length < 34 || static_cast<unsigned char>(root[32]) != 1 ||
                static_cast<unsigned char>(root[33]) != 0) {
                throw std::runtime_error("ISO 根目录记录无效");
            }
            IsoEntry candidate{};
            candidate.extent_lba = read_dual32(root + 2);
            candidate.size = read_dual32(root + 10);
            candidate.is_directory = true;
            if (const auto modified = recording_time(root)) {
                candidate.modified = *modified;
                candidate.has_modified_time = true;
            }
            const auto root_sectors =
                (candidate.size + sector_size - 1U) / sector_size;
            if (candidate.extent_lba > volume_sectors ||
                root_sectors > volume_sectors - candidate.extent_lba ||
                static_cast<std::uint64_t>(candidate.extent_lba) * sector_size +
                    candidate.size > image_size_) {
                throw std::runtime_error("ISO 根目录超出镜像边界");
            }
            if (descriptor_type == 1) {
                primary_root = candidate;
                primary_volume_sectors = volume_sectors;
                found_primary = true;
            } else {
                const auto level = descriptor[90] == 'E' ? 3U
                    : descriptor[90] == 'C' ? 2U : 1U;
                if (level > joliet_level) {
                    joliet_root = candidate;
                    joliet_volume_sectors = volume_sectors;
                    joliet_level = level;
                }
            }
        }
        if (static_cast<unsigned char>(descriptor[0]) == 255) {
            break;
        }
    }
    if (!found_primary) {
        throw std::runtime_error("未找到 ISO 9660 主卷描述符");
    }
    if (joliet_level != 0) {
        root_ = joliet_root;
        volume_sectors_ = joliet_volume_sectors;
        joliet_ = true;
    } else {
        root_ = primary_root;
        volume_sectors_ = primary_volume_sectors;
        if (root_.size != 0) {
            std::array<char, sector_size> first_directory_sector{};
            const auto bytes_to_read = static_cast<std::size_t>(
                std::min<std::uint64_t>(root_.size, first_directory_sector.size()));
            read_at(entry_offset(root_), first_directory_sector.data(),
                    bytes_to_read);
            if (bytes_to_read >= 34) {
                const auto record_length = static_cast<unsigned char>(
                    first_directory_sector[0]);
                if (record_length >= 34 && record_length <= bytes_to_read) {
                    const auto skip = rock_ridge_susp_skip(
                        first_directory_sector.data(), record_length);
                    if (skip) {
                        rock_ridge_ = true;
                        rock_ridge_skip_ = *skip;
                    }
                }
            }
        }
    }
}

std::vector<IsoEntry> Iso9660::read_directory(const IsoEntry& directory) const {
    if (!directory.is_directory) {
        throw std::invalid_argument("指定路径不是目录");
    }
    if (directory.size > maximum_directory_size) {
        throw std::runtime_error("目录超过当前版本的安全读取上限（64 MiB）");
    }

    std::vector<char> bytes(static_cast<std::size_t>(directory.size));
    if (!bytes.empty()) {
        read_at(entry_offset(directory), bytes.data(), bytes.size());
    }
    std::vector<IsoEntry> entries;
    const auto volume_bytes =
        static_cast<std::uint64_t>(volume_sectors_) * sector_size;
    const SuspContinuationReader read_continuation =
        [this, volume_bytes](std::uint64_t offset, std::uint32_t length) {
            if (offset > volume_bytes || length > volume_bytes - offset) {
                throw std::runtime_error(
                    "Rock Ridge CE continuation 超出卷边界");
            }
            std::vector<char> continuation(length);
            read_at(offset, continuation.data(), continuation.size());
            return continuation;
        };
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto record_length = static_cast<unsigned char>(bytes[offset]);
        if (record_length == 0) {
            offset = ((offset / sector_size) + 1) * sector_size;
            continue;
        }
        if (record_length < 34 || offset + record_length > bytes.size()) {
            throw std::runtime_error("ISO 目录记录长度无效");
        }
        const auto* record = bytes.data() + offset;
        const auto identifier_length = static_cast<unsigned char>(record[32]);
        if (33U + identifier_length > record_length) {
            throw std::runtime_error("ISO 文件名记录越界");
        }
        if (!(identifier_length == 1 &&
              (static_cast<unsigned char>(record[33]) == 0 ||
               static_cast<unsigned char>(record[33]) == 1))) {
            auto name = visible_name(record + 33, identifier_length, joliet_);
            const auto flags = static_cast<unsigned char>(record[25]);
            const bool is_directory = (flags & 0x02U) != 0;
            RockRidgeAttributes rock_ridge_attributes;
            if (rock_ridge_) {
                rock_ridge_attributes =
                    read_rock_ridge_attributes(
                        record, record_length, rock_ridge_skip_,
                        read_continuation, volume_bytes);
                if (rock_ridge_attributes.has_name) {
                    name = rock_ridge_attributes.name;
                }
                if (rock_ridge_attributes.has_posix_metadata &&
                    (((rock_ridge_attributes.mode & 0170000U) == 0040000U) !=
                     is_directory)) {
                    throw std::runtime_error(
                        "Rock Ridge PX 类型与 ISO 目录记录不一致");
                }
            }
            const bool interleaved = record[26] != 0 || record[27] != 0;
            if (interleaved) {
                throw std::runtime_error("当前版本不支持 ISO 交错文件");
            }
            const auto validate_extent = [this](std::uint32_t lba, std::uint32_t size) {
                const auto extent_offset = static_cast<std::uint64_t>(lba) * sector_size;
                const auto sectors =
                    (static_cast<std::uint64_t>(size) + sector_size - 1U) /
                    sector_size;
                if (lba > volume_sectors_ ||
                    sectors > volume_sectors_ - lba ||
                    extent_offset > image_size_ || size > image_size_ - extent_offset) {
                    throw std::runtime_error("ISO 文件区段超出镜像边界");
                }
            };
            const auto first_lba = read_dual32(record + 2);
            const auto first_size = read_dual32(record + 10);
            validate_extent(first_lba, first_size);
            bool has_more_extents = (flags & 0x80U) != 0;
            IsoEntry entry{};
            entry.name = name;
            entry.extent_lba = first_lba;
            entry.size = first_size;
            entry.is_directory = is_directory;
            entry.extents.push_back({first_lba, first_size});
            if (rock_ridge_attributes.has_posix_metadata) {
                entry.mode = rock_ridge_attributes.mode & 07777U;
                entry.uid = rock_ridge_attributes.uid;
                entry.gid = rock_ridge_attributes.gid;
                entry.link_count = rock_ridge_attributes.link_count;
                entry.hard_link_group =
                    rock_ridge_attributes.hard_link_group;
                entry.has_posix_metadata = true;
            }
            if (rock_ridge_attributes.has_symlink_target) {
                if (has_more_extents) {
                    throw std::runtime_error(
                        "Rock Ridge 多区段符号链接不受支持");
                }
                entry.is_symlink = true;
                entry.symlink_target =
                    std::move(rock_ridge_attributes.symlink_target);
            }
            if (const auto modified = recording_time(record)) {
                entry.modified = *modified;
                entry.has_modified_time = true;
            }
            if (has_more_extents && is_directory) {
                throw std::runtime_error("ISO 多区段目录不受支持");
            }
            offset += record_length;
            while (has_more_extents) {
                while (offset < bytes.size() && bytes[offset] == 0) {
                    offset = ((offset / sector_size) + 1) * sector_size;
                }
                if (offset >= bytes.size()) {
                    throw std::runtime_error("ISO 多区段文件缺少后续区段记录");
                }
                const auto next_length = static_cast<unsigned char>(bytes[offset]);
                if (next_length < 34 || offset + next_length > bytes.size()) {
                    throw std::runtime_error("ISO 多区段记录长度无效");
                }
                const auto* next = bytes.data() + offset;
                const auto next_name_length = static_cast<unsigned char>(next[32]);
                if (33U + next_name_length > next_length ||
                    (next_name_length == 1 &&
                     (static_cast<unsigned char>(next[33]) == 0 ||
                      static_cast<unsigned char>(next[33]) == 1))) {
                    throw std::runtime_error("ISO 多区段记录文件名无效");
                }
                const auto next_name = visible_name(
                    next + 33, next_name_length, joliet_);
                const auto next_flags = static_cast<unsigned char>(next[25]);
                if (!same_name(name, next_name) || (next_flags & 0x02U) != 0 ||
                    next[26] != 0 || next[27] != 0) {
                    throw std::runtime_error("ISO 多区段文件记录不连续或不匹配");
                }
                const auto next_lba = read_dual32(next + 2);
                const auto next_size = read_dual32(next + 10);
                validate_extent(next_lba, next_size);
                if (entry.size > std::numeric_limits<std::uint64_t>::max() - next_size) {
                    throw std::runtime_error("ISO 多区段文件总长度溢出");
                }
                entry.size += next_size;
                entry.extents.push_back({next_lba, next_size});
                has_more_extents = (next_flags & 0x80U) != 0;
                offset += next_length;
            }
            entries.push_back(std::move(entry));
            continue;
        }
        offset += record_length;
    }
    return entries;
}

std::vector<IsoEntry> Iso9660::list(const std::string& iso_path) const {
    const auto target = resolve(iso_path);
    return read_directory(target);
}

IsoEntry Iso9660::resolve(const std::string& iso_path) const {
    auto current = root_;
    for (const auto& part : split_path(iso_path)) {
        const auto entries = read_directory(current);
        const auto found = std::find_if(entries.begin(), entries.end(),
            [&part](const IsoEntry& entry) { return same_name(entry.name, part); });
        if (found == entries.end()) {
            throw std::runtime_error("ISO 路径不存在: " + iso_path);
        }
        current = *found;
    }
    return current;
}

IsoEntry Iso9660::stat(const std::string& iso_path) const {
    return resolve(iso_path);
}

std::size_t Iso9660::read_file(const std::string& iso_path,
                               std::uint64_t offset,
                               char* destination,
                               std::size_t length) const {
    const auto entry = resolve(iso_path);
    if (entry.is_directory || entry.is_symlink) {
        throw std::invalid_argument(
            "指定 ISO 路径是目录或符号链接，不能读取常规文件数据");
    }
    return read_entry(entry, offset, destination, length);
}

std::size_t Iso9660::read_entry(const IsoEntry& entry,
                                std::uint64_t offset,
                                char* destination,
                                std::size_t length) const {
    if (offset >= entry.size || length == 0) {
        return 0;
    }
    const auto requested = static_cast<std::size_t>(
        std::min<std::uint64_t>(length, entry.size - offset));
    std::size_t written = 0;
    std::uint64_t extent_start = 0;
    for (const auto& extent : entry.extents) {
        const auto extent_size = static_cast<std::uint64_t>(extent.size);
        if (offset >= extent_start + extent_size) {
            extent_start += extent_size;
            continue;
        }
        const auto offset_in_extent = offset > extent_start
            ? offset - extent_start : 0;
        const auto available = extent_size - offset_in_extent;
        const auto count = std::min<std::size_t>(
            requested - written, static_cast<std::size_t>(available));
        const auto physical_offset =
            static_cast<std::uint64_t>(extent.lba) * sector_size + offset_in_extent;
        read_at(physical_offset, destination + written, count);
        written += count;
        offset += count;
        extent_start += extent_size;
        if (written == requested) {
            return written;
        }
    }
    if (entry.extents.empty()) {
        const auto count = static_cast<std::size_t>(
            std::min<std::uint64_t>(length, entry.size - offset));
        read_at(entry_offset(entry) + offset, destination, count);
        return count;
    }
    if (written != requested) {
        throw std::runtime_error("ISO 多区段文件 extent 长度与目录记录不匹配");
    }
    return written;
}

void Iso9660::extract_file(const std::string& iso_path,
                          const std::filesystem::path& destination) const {
    const auto entry = resolve(iso_path);
    if (entry.is_directory) {
        throw std::invalid_argument("不能将目录作为单个文件提取");
    }
    if (destination.empty()) {
        throw std::invalid_argument("必须指定导出目标文件");
    }

    std::error_code error;
    const auto source = std::filesystem::weakly_canonical(image_path_, error);
    if (error) {
        throw std::runtime_error("无法规范化镜像路径");
    }
    const auto output = std::filesystem::weakly_canonical(destination, error);
    if (error) {
        throw std::runtime_error("无法规范化导出路径");
    }
    bool same_file = source == output;
    const bool destination_exists = std::filesystem::exists(destination, error);
    if (error) {
        throw std::runtime_error("无法检查导出目标");
    }
    if (destination_exists) {
        same_file = same_file || std::filesystem::equivalent(image_path_, destination, error);
        if (error) {
            throw std::runtime_error("无法确认导出目标是否与镜像为同一文件");
        }
    }
    if (same_file) {
        throw std::invalid_argument("导出目标不能覆盖原始镜像");
    }

    std::ofstream output_file(destination, std::ios::binary | std::ios::trunc);
    if (!output_file) {
        throw std::runtime_error("无法创建导出文件: " + destination.string());
    }

    std::array<char, 64U * 1024U> buffer{};
    std::uint64_t remaining = entry.size;
    std::uint64_t offset = 0;
    while (remaining > 0) {
        const auto requested = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, buffer.size()));
        const auto count = read_entry(entry, offset, buffer.data(), requested);
        if (count != requested) {
            throw std::runtime_error("读取 ISO 文件内容时遇到意外 EOF");
        }
        output_file.write(buffer.data(), static_cast<std::streamsize>(count));
        if (!output_file) {
            throw std::runtime_error("写入导出文件失败");
        }
        remaining -= count;
        offset += count;
    }
}

std::uint64_t Iso9660::image_size() const noexcept {
    return image_size_;
}

std::uint32_t Iso9660::volume_sectors() const noexcept {
    return volume_sectors_;
}

std::uint64_t Iso9660::entry_offset(const IsoEntry& entry) const {
    const auto lba = entry.extents.empty() ? entry.extent_lba : entry.extents.front().lba;
    return static_cast<std::uint64_t>(lba) * sector_size;
}

void Iso9660::read_at(std::uint64_t offset, char* destination, std::size_t length) const {
    if (offset > image_size_ || length > image_size_ - offset ||
        byte_offset_ > std::numeric_limits<std::uint64_t>::max() - offset) {
        throw std::runtime_error("读取请求超出镜像范围");
    }
    if (reader_) {
        const auto destination_bytes = std::span<std::byte>(
            reinterpret_cast<std::byte*>(destination), length);
        const auto count = reader_(byte_offset_ + offset, destination_bytes);
        if (count != length) {
            throw std::runtime_error("读取 ISO 镜像时遇到意外 EOF");
        }
        return;
    }
    const auto absolute_offset = byte_offset_ + offset;
    if (absolute_offset >
        static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        throw std::runtime_error("读取请求超出镜像范围");
    }
    std::ifstream input(image_path_, std::ios::binary);
    if (!input) {
        throw std::runtime_error("无法以只读方式打开 ISO 镜像");
    }
    input.seekg(static_cast<std::streamoff>(absolute_offset));
    input.read(destination, static_cast<std::streamsize>(length));
    if (input.gcount() != static_cast<std::streamsize>(length)) {
        throw std::runtime_error("读取 ISO 镜像时遇到意外 EOF");
    }
}

} // namespace uife::iso9660
