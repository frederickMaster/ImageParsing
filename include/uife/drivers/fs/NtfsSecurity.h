#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <string>

#include "uife/VFSNode.h"

namespace uife::ntfs_security {

// 把 NTFS $Secure:$SDS 数据流解码为 security_id → 安全描述符的索引。
//
// $SDS 条目布局（每条目 16 字节对齐）：
//   +0  hash        4B   （仅用于索引加速，本模块不校验）
//   +4  security_id 4B
//   +8  offset      8B   安全描述符在 $SDS 流内的偏移
//   +16 length      4B   安全描述符字节数
//   +20 <SECURITY_DESCRIPTOR_RELATIVE，紧跟头部>
//
// 结构损坏的条目会被跳过（或终止遍历），不抛异常：取证场景下"部分 ACL
// 可用"优于"整体失败"。同一 security_id 重复出现时保留第一条。
std::map<std::uint32_t, VFSSecurityDescriptor> parse_sds(
    std::span<const std::byte> data);

// 自相对 SECURITY_DESCRIPTOR 的解码入口（$SDS 条目头之后的部分）。
// revision 非 1 或字段越界时返回 valid=false 的对象。
VFSSecurityDescriptor parse_security_descriptor(
    std::span<const std::byte> descriptor);

// SID 的标准文本形式（S-1-<authority>-<sub0>-...）。
// revision 非 1、缓冲区截断或 authority 非法时返回空串。
std::string sid_to_string(std::span<const std::byte> sid);

} // namespace uife::ntfs_security
