#include "uife/drivers/fs/NtfsSecurity.h"

#include <cstdio>
#include <utility>

namespace uife::ntfs_security {
namespace {

constexpr std::uint32_t kControlDaclPresent = 0x0004U;
constexpr std::uint32_t kControlSaclPresent = 0x0010U;
constexpr std::uint32_t kControlDaclProtected = 0x1000U;
constexpr std::uint32_t kControlSaclProtected = 0x2000U;

std::uint16_t read_le16(const std::byte* data) noexcept {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(std::to_integer<unsigned char>(data[0])) |
        (static_cast<std::uint16_t>(std::to_integer<unsigned char>(data[1]))
         << 8U));
}

std::uint32_t read_le32(const std::byte* data) noexcept {
    return static_cast<std::uint32_t>(std::to_integer<unsigned char>(data[0])) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(data[1]))
            << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(data[2]))
            << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(data[3]))
            << 24U);
}

std::uint64_t read_le64(const std::byte* data) noexcept {
    return static_cast<std::uint64_t>(read_le32(data)) |
           (static_cast<std::uint64_t>(read_le32(data + 4)) << 32U);
}

std::size_t align_up_16(std::size_t value) noexcept {
    return (value + 15U) & ~static_cast<std::size_t>(15U);
}

std::string ace_type_name(std::uint8_t type) {
    switch (type) {
    case 0x00:
        return "allow";
    case 0x01:
        return "deny";
    case 0x02:
        return "audit";
    case 0x03:
        return "alarm";
    case 0x04:
        return "compound";
    case 0x05:
        return "object_allow";
    case 0x06:
        return "object_deny";
    case 0x07:
        return "object_audit";
    case 0x08:
        return "object_alarm";
    case 0x09:
        return "callback_allow";
    case 0x0A:
        return "callback_deny";
    case 0x0B:
        return "callback_audit";
    case 0x0C:
        return "callback_object_allow";
    case 0x0D:
        return "callback_object_deny";
    case 0x0E:
        return "callback_object_audit";
    case 0x11:
        return "resource_attribute";
    case 0x12:
        return "scoped_policy_id";
    default:
        {
            char buffer[12]{};
            std::snprintf(buffer, sizeof(buffer), "type_0x%02x", type);
            return std::string(buffer);
        }
    }
}

// 解析一条 ACL（DACL/SACL 同构）：revision(1) pad(1) size(2) count(2)，
// 然后是 count 条 ACE。单条 ACE 损坏只终止 ACL 内的遍历，不影响其余数据。
std::vector<VFSAce> parse_acl(std::span<const std::byte> acl) {
    std::vector<VFSAce> aces;
    if (acl.size() < 8) {
        return aces;
    }
    const auto count = read_le16(acl.data() + 4);
    // count 上限：每条 ACE 至少 8 字节，防御性拒绝明显荒谬的声明。
    if (count == 0 || count > (acl.size() - 8U) / 8U) {
        return aces;
    }
    std::size_t offset = 8;
    for (std::uint16_t index = 0; index < count && offset + 8 <= acl.size();
         ++index) {
        const auto type = std::to_integer<unsigned char>(acl[offset]);
        const auto flags = std::to_integer<unsigned char>(acl[offset + 1]);
        const auto size = read_le16(acl.data() + offset + 2);
        if (size < 8 || offset + size > acl.size()) {
            break;
        }
        VFSAce ace;
        ace.raw_type = type;
        ace.type = ace_type_name(type);
        ace.inherited = (flags & 0x10U) != 0;  // INHERITED_ACE
        ace.mask = read_le32(acl.data() + offset + 4);
        if (size > 8) {
            ace.sid = sid_to_string(acl.subspan(offset + 8, size - 8));
        }
        aces.push_back(std::move(ace));
        offset += size;
    }
    return aces;
}

} // namespace

namespace {

// 自相对描述符的快速校验：revision=1 且 control 带 SE_SELF_RELATIVE（0x8000）。
// 用于在两种 $SDS 布局之间判别，避免把头部字节误当描述符解析。
bool is_self_relative_descriptor(std::span<const std::byte> candidate) noexcept {
    if (candidate.size() < 20) {
        return false;
    }
    const auto revision = std::to_integer<unsigned char>(candidate[0]);
    const auto control = read_le16(candidate.data() + 2);
    return revision == 1 && (control & 0x8000U) != 0;
}

} // namespace

std::string sid_to_string(std::span<const std::byte> sid) {
    if (sid.size() < 8) {
        return {};
    }
    const auto revision = std::to_integer<unsigned char>(sid[0]);
    const auto sub_authority_count = std::to_integer<unsigned char>(sid[1]);
    if (revision != 1 || sub_authority_count > 15 ||
        sid.size() < 8 + static_cast<std::size_t>(sub_authority_count) * 4U) {
        return {};
    }
    // IdentifierAuthority 是 6 字节大端整数；只有低 2-6 字节常见于实践，
    // 高位非零时仍按完整 64 位值输出，避免静默截断。
    std::uint64_t authority = 0;
    for (std::size_t index = 2; index < 8; ++index) {
        authority = (authority << 8U) |
                    std::to_integer<unsigned char>(sid[index]);
    }
    std::string text = "S-1-" + std::to_string(authority);
    for (std::uint8_t index = 0; index < sub_authority_count; ++index) {
        text += "-" +
                std::to_string(read_le32(sid.data() + 8 +
                                         static_cast<std::size_t>(index) * 4U));
    }
    return text;
}

VFSSecurityDescriptor parse_security_descriptor(
    std::span<const std::byte> descriptor) {
    VFSSecurityDescriptor result;
    // 自相对头：revision(1) pad(1) control(2) owner(4) group(4) sacl(4) dacl(4)
    if (descriptor.size() < 20) {
        return result;
    }
    const auto revision = std::to_integer<unsigned char>(descriptor[0]);
    if (revision != 1) {
        // 只认 revision 1；未知版本的字段语义未定义，保守返回无效。
        return result;
    }
    const auto control = read_le16(descriptor.data() + 2);
    const auto owner_offset = read_le32(descriptor.data() + 4);
    const auto group_offset = read_le32(descriptor.data() + 8);
    const auto sacl_offset = read_le32(descriptor.data() + 12);
    const auto dacl_offset = read_le32(descriptor.data() + 16);

    result.valid = true;
    const auto within = [&descriptor](std::uint32_t offset) {
        return offset != 0 && offset < descriptor.size();
    };
    if ((control & kControlDaclPresent) != 0 && within(dacl_offset)) {
        result.dacl_present = true;
        result.dacl_protected = (control & kControlDaclProtected) != 0;
        result.dacl = parse_acl(descriptor.subspan(dacl_offset));
    }
    if ((control & kControlSaclPresent) != 0 && within(sacl_offset)) {
        result.sacl_present = true;
        result.sacl_protected = (control & kControlSaclProtected) != 0;
        result.sacl = parse_acl(descriptor.subspan(sacl_offset));
    }
    if (within(owner_offset)) {
        result.owner_sid =
            sid_to_string(descriptor.subspan(owner_offset));
    }
    if (within(group_offset)) {
        result.group_sid =
            sid_to_string(descriptor.subspan(group_offset));
    }
    return result;
}

std::map<std::uint32_t, VFSSecurityDescriptor> parse_sds(
    std::span<const std::byte> data) {
    std::map<std::uint32_t, VFSSecurityDescriptor> index;
    std::size_t entry = 0;
    while (entry + 20 <= data.size()) {
        const auto security_id = read_le32(data.data() + entry + 4);
        const auto offset_field = read_le64(data.data() + entry + 8);
        const auto length_field = read_le32(data.data() + entry + 16);
        // security_id 为 0 或 length 过小说明走到流尾/空隙，终止遍历。
        if (security_id == 0 || length_field < 40 ||
            length_field > data.size() - entry) {
            break;
        }

        // 实测 Windows 布局：offset 字段是条目自身的流内偏移（首条目为 0），
        // length 是"20 字节头 + 安全描述符"的总长，描述符紧跟头部。
        // 文档描述的另一种布局（offset 指向描述符、length 为描述符长度）
        // 作为回退，通过 revision=1 + self-relative 位验证哪种成立。
        std::span<const std::byte> descriptor;
        std::size_t stride = 0;
        const auto candidate_self_contained =
            data.subspan(entry + 20, static_cast<std::size_t>(length_field) - 20U);
        if (is_self_relative_descriptor(candidate_self_contained)) {
            descriptor = candidate_self_contained;
            stride = static_cast<std::size_t>(length_field);
        } else if (offset_field >= entry + 20 &&
                   offset_field <= data.size() &&
                   length_field <= data.size() - offset_field) {
            const auto candidate_external = data.subspan(
                static_cast<std::size_t>(offset_field),
                static_cast<std::size_t>(length_field));
            if (is_self_relative_descriptor(candidate_external)) {
                descriptor = candidate_external;
                stride = 20U + static_cast<std::size_t>(length_field);
            }
        }
        if (descriptor.empty()) {
            break;  // 两种布局都不成立：结构未知，无法继续推进
        }

        if (index.find(security_id) == index.end()) {
            auto parsed = parse_security_descriptor(descriptor);
            if (parsed.valid) {
                index.emplace(security_id, std::move(parsed));
            }
        }
        const auto aligned = align_up_16(stride);
        if (aligned == 0) {
            break;
        }
        entry += aligned;
    }
    return index;
}

} // namespace uife::ntfs_security
