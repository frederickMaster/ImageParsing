#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace uife {

enum class NodeType {
    regular_file,
    directory,
    symbolic_link,
    other
};

// 命名备用数据流（NTFS ADS / 资源分支）中可由解析器读取的一项。
// name 为流名称原文（不含 ":$DATA" 后缀），仅用于展示与读取；导出时会做文件名编码。
struct VFSStream {
    std::string name;
    std::uint64_t size{};
    bool size_valid{true};
    // false 表示解析器只能报告流的存在与大小，当前后端无法导出其内容
    // （例如 wimlib 未公开单流提取接口）。提取器会跳过此类流。
    bool readable{true};
};

// NTFS $STANDARD_INFORMATION 中的 FileAttributes 位（仅保留取证相关项）。
namespace ntfs_attribute {constexpr std::uint32_t read_only = 0x00000001U;
constexpr std::uint32_t hidden = 0x00000002U;
constexpr std::uint32_t system = 0x00000004U;
constexpr std::uint32_t archive = 0x00000020U;
constexpr std::uint32_t sparse = 0x00000200U;
constexpr std::uint32_t reparse_point = 0x00000400U;
constexpr std::uint32_t compressed = 0x00000800U;
constexpr std::uint32_t offline = 0x00001000U;
constexpr std::uint32_t not_content_indexed = 0x00002000U;
constexpr std::uint32_t encrypted = 0x00004000U;
} // namespace ntfs_attribute

// 文件数据在磁盘上的分配区间（字节单位的逻辑偏移）。
// allocated=false 表示该区间是稀疏洞，读取时按零填充。
struct VFSExtent {
    std::uint64_t offset{};
    std::uint64_t length{};
    bool allocated{true};
};

// NTFS ACL 中的一条 ACE。sid 是标准 SDDL 形式（S-1-<authority>-<sub>...），
// 不做 SID→账户名解析（那需要读 SAM/注册表，属于下游工具职责）。
struct VFSAce {
    std::string type;  // allow / deny / audit / alarm / object_allow / ... / type_0xNN
    std::uint8_t raw_type{};
    bool inherited{};  // 来自父目录继承（ACE flags 的 INHERITED_ACE 位）
    std::uint32_t mask{};  // 访问掩码原值（FILE_* / GENERIC_* 位）
    std::string sid;
};

// 一份自相对（self-relative）SECURITY_DESCRIPTOR 的解码结果。
// valid=false 表示该 security_id 在 $SDS 中缺失或记录损坏。
struct VFSSecurityDescriptor {
    bool valid{};
    std::string owner_sid;
    std::string group_sid;
    bool dacl_present{};
    bool dacl_protected{};
    std::vector<VFSAce> dacl;
    bool sacl_present{};
    bool sacl_protected{};
    std::vector<VFSAce> sacl;
};

struct VFSMetadata {
    std::uint64_t size{};
    bool size_valid{true};
    std::uint64_t inode{};
    std::uint64_t hard_link_group{};
    std::uint32_t mode{};
    std::uint32_t uid{};
    std::uint32_t gid{};
    bool has_owner{};
    std::uint64_t link_count{1};
    std::chrono::system_clock::time_point created{};
    std::chrono::system_clock::time_point accessed{};
    std::chrono::system_clock::time_point modified{};
    bool has_created_time{};
    bool has_accessed_time{};
    bool has_modified_time{};
    bool sparse{};
    bool compressed{};
    bool encrypted{};
    bool reparse_point{};
    std::uint32_t file_attributes{};
    bool has_file_attributes{};
    // NTFS 安全描述符在 $Secure:$SDS 中的索引；完整 ACL 需由下游按索引解析。
    std::uint32_t security_id{};
    bool has_security_id{};
    // 由 security_id 在 $Secure:$SDS 索引中解码出的安全描述符；
    // 仅在 has_security 为真时有效（id 缺失或 $SDS 不可读时保持 false）。
    VFSSecurityDescriptor security{};
    bool has_security{};
    // 重解析点标签（IO_REPARSE_TAG_*），仅在 reparse_point 为真时有意义。
    std::uint32_t reparse_tag{};
    bool has_reparse_tag{};
    std::string symlink_target;
    std::vector<VFSStream> alternate_streams;
    // 仅在后端能可靠给出物理分配映射时填充（当前为未压缩、未加密的 NTFS 默认数据流）。
    std::vector<VFSExtent> extents;
    bool extents_truncated{};
    std::map<std::string, std::string> extended_attributes;
};

struct VFSNode {
    std::string name;
    std::string path;
    NodeType type{NodeType::other};
    VFSMetadata metadata;
};

} // namespace uife
