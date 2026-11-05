#include "uife/drivers/fs/NtfsSecurity.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

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
    const std::size_t total = data.size();

    // 诊断轨迹：只在"整条流一条都没解出来"时才输出到 stderr（最多 8 行），
    // 正常数据与零星碎屑都不会产生噪声；解析失败时能直接看到卡在哪一步。
    std::vector<std::string> trace;
    const auto note = [&trace](const char* format, auto... args) {
        if (trace.size() >= 8) {
            return;
        }
        char buffer[192]{};
        std::snprintf(buffer, sizeof(buffer), format, args...);
        trace.emplace_back(buffer);
    };
    note("total=%zu", total);

    std::size_t entry = 0;
    while (total >= entry && total - entry >= 20) {
        const auto security_id = read_le32(data.data() + entry + 4);
        const auto offset_field = read_le64(data.data() + entry + 8);
        const auto length_field = read_le32(data.data() + entry + 16);
        note("entry=%zu id=0x%x offset=%llu length=%u", entry, security_id,
             static_cast<unsigned long long>(offset_field), length_field);
        // security_id 为 0 或 length 过小说明走到流尾/空隙，终止遍历。
        if (security_id == 0 || length_field < 40 ||
            length_field > total - entry) {
            note("  stop: id=%u length=%u remaining=%zu", security_id,
                 length_field, total - entry);
            break;
        }

        // 磁盘上的 $SDS 存在两种布局写法：
        //   A（Windows 实测）：偏移字段 = 条目自身流内偏移，length = 20 + 描述符长度，
        //                     描述符紧跟 20 字节条目头；
        //   B（部分文档）：   偏移字段指向描述符，length = 描述符长度。
        // 判别不再依赖"位掩码像不像描述符"，而是直接尝试解码：能解出 revision=1
        // 的描述符才算成立。判据即解析结果本身，避免启发式判断带来的分叉。
        std::span<const std::byte> descriptor;
        std::size_t stride = 0;
        VFSSecurityDescriptor parsed;

        const auto candidate_self_contained =
            data.subspan(entry + 20, static_cast<std::size_t>(length_field) - 20U);
        auto parsed_self_contained = parse_security_descriptor(candidate_self_contained);
        if (parsed_self_contained.valid) {
            descriptor = candidate_self_contained;
            stride = static_cast<std::size_t>(length_field);
            parsed = std::move(parsed_self_contained);
        } else if (offset_field >= entry + 20 && offset_field <= total &&
                   length_field <= total - offset_field) {
            const auto candidate_external = data.subspan(
                static_cast<std::size_t>(offset_field),
                static_cast<std::size_t>(length_field));
            auto parsed_external = parse_security_descriptor(candidate_external);
            if (parsed_external.valid) {
                descriptor = candidate_external;
                stride = 20U + static_cast<std::size_t>(length_field);
                parsed = std::move(parsed_external);
            }
        }
        if (descriptor.empty()) {
            note("  stop: no descriptor (A: ptr+%lld size=%zu head=%02x %02x %02x %02x)",
                 static_cast<long long>(candidate_self_contained.data() - data.data()),
                 candidate_self_contained.size(),
                 static_cast<int>(std::to_integer<unsigned char>(candidate_self_contained[0])),
                 static_cast<int>(std::to_integer<unsigned char>(candidate_self_contained[1])),
                 static_cast<int>(std::to_integer<unsigned char>(candidate_self_contained[2])),
                 static_cast<int>(std::to_integer<unsigned char>(candidate_self_contained[3])));
            break;  // 两种布局都不成立：结构未知，无法继续推进
        }

        if (index.find(security_id) == index.end()) {
            note("  descriptor=%zu dacl=%zu sacl=%zu", descriptor.size(),
                 parsed.dacl.size(), parsed.sacl.size());
            index.emplace(security_id, std::move(parsed));
        }
        const auto aligned = align_up_16(stride);
        if (aligned == 0) {
            break;
        }
        entry += aligned;
    }
    if (index.empty() && !trace.empty()) {
        std::fputs("[sds] no entry decoded\n", stderr);
        for (const auto& line : trace) {
            std::fprintf(stderr, "[sds] %s\n", line.c_str());
        }
    }
    return index;
}

} // namespace uife::ntfs_security
