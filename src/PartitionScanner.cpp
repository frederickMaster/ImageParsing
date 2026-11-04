#include "uife/PartitionScanner.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace uife {
namespace {

std::uint32_t u32(std::span<const std::byte> data, std::size_t offset) {
    return static_cast<std::uint32_t>(std::to_integer<unsigned char>(data[offset])) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(data[offset + 1])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(data[offset + 2])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(data[offset + 3])) << 24U);
}

std::uint64_t u64(std::span<const std::byte> data, std::size_t offset) {
    return static_cast<std::uint64_t>(u32(data, offset)) |
           (static_cast<std::uint64_t>(u32(data, offset + 4)) << 32U);
}

std::uint32_t crc32(std::span<const std::byte> bytes) {
    std::uint32_t crc = 0xffffffffU;
    for (const auto byte : bytes) {
        crc ^= std::to_integer<unsigned char>(byte);
        for (unsigned int bit = 0; bit < 8; ++bit) {
            const auto mask = static_cast<std::uint32_t>(
                -static_cast<std::int32_t>(crc & 1U));
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

std::string guid_type(std::span<const std::byte> bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string value;
    value.reserve(36);
    for (std::size_t index = 0; index < 16; ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            value.push_back('-');
        }
        const auto byte = std::to_integer<unsigned char>(bytes[index]);
        value.push_back(digits[byte >> 4U]);
        value.push_back(digits[byte & 0x0fU]);
    }
    return value;
}

std::string mbr_type_name(unsigned char type) {
    constexpr char digits[] = "0123456789abcdef";
    std::string name = "0x";
    name.push_back(digits[(type >> 4U) & 0x0fU]);
    name.push_back(digits[type & 0x0fU]);
    return name;
}

bool is_extended_partition(unsigned char type) {
    return type == 0x05 || type == 0x0f || type == 0x85;
}

} // namespace

std::vector<Partition> scan_partitions(const IImageDriver& image) {
    constexpr std::size_t sector_bytes = 512;
    if (image.get_size() < sector_bytes) {
        return {};
    }
    std::array<std::byte, sector_bytes> sector{};
    const auto bytes_read = image.read_at(0, sector);
    if (bytes_read != sector.size() || sector[510] != std::byte{0x55} ||
        sector[511] != std::byte{0xaa}) {
        return {};
    }

    std::vector<Partition> partitions;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> extended_partitions;
    bool protective_mbr = false;
    for (std::size_t index = 0; index < 4; ++index) {
        const auto offset = 446 + index * 16;
        const auto type = std::to_integer<unsigned char>(sector[offset + 4]);
        const auto start = u32(sector, offset + 8);
        const auto count = u32(sector, offset + 12);
        if (type == 0xee) {
            protective_mbr = true;
        }
        if (type == 0 || count == 0 || type == 0xee) {
            continue;
        }
        const auto byte_offset = static_cast<std::uint64_t>(start) * sector_bytes;
        const auto byte_length = static_cast<std::uint64_t>(count) * sector_bytes;
        if (byte_offset <= image.get_size() &&
            byte_length <= image.get_size() - byte_offset) {
            if (is_extended_partition(type)) {
                extended_partitions.emplace_back(start, count);
            } else {
                partitions.push_back(
                    {byte_offset, byte_length, "MBR", mbr_type_name(type)});
            }
        }
    }

    constexpr std::uint64_t max_ebr_chain_length = 4096;
    const auto image_sector_count = image.get_size() / sector_bytes;
    for (const auto& [extended_base, extended_length] : extended_partitions) {
        const auto extended_end = extended_base + extended_length;
        auto ebr_lba = extended_base;
        std::unordered_set<std::uint64_t> visited_ebr_lbas;
        for (std::uint64_t depth = 0; depth < max_ebr_chain_length; ++depth) {
            if (ebr_lba < extended_base || ebr_lba >= extended_end ||
                ebr_lba >= image_sector_count) {
                throw std::runtime_error("MBR EBR 链指向扩展分区范围之外");
            }
            if (!visited_ebr_lbas.insert(ebr_lba).second) {
                throw std::runtime_error("MBR EBR 链存在循环引用");
            }
            std::array<std::byte, sector_bytes> ebr{};
            if (image.read_at(ebr_lba * sector_bytes, ebr) != ebr.size() ||
                ebr[510] != std::byte{0x55} || ebr[511] != std::byte{0xaa}) {
                throw std::runtime_error("MBR EBR 扇区签名无效");
            }

            const auto logical_type =
                std::to_integer<unsigned char>(ebr[446 + 4]);
            const auto logical_start = u32(ebr, 446 + 8);
            const auto logical_length = u32(ebr, 446 + 12);
            if (logical_type != 0 && logical_length != 0 &&
                !is_extended_partition(logical_type)) {
                const auto absolute_start = ebr_lba + logical_start;
                if (absolute_start < extended_base || absolute_start >= extended_end ||
                    logical_length > extended_end - absolute_start ||
                    absolute_start >= image_sector_count ||
                    logical_length > image_sector_count - absolute_start) {
                    throw std::runtime_error(
                        "MBR 逻辑分区超出扩展分区或镜像边界");
                }
                partitions.push_back({
                    absolute_start * sector_bytes,
                    static_cast<std::uint64_t>(logical_length) * sector_bytes,
                    "MBR",
                    mbr_type_name(logical_type)});
            }

            const auto link_type =
                std::to_integer<unsigned char>(ebr[462 + 4]);
            const auto link_start = u32(ebr, 462 + 8);
            const auto link_length = u32(ebr, 462 + 12);
            if (!is_extended_partition(link_type) || link_length == 0) {
                break;
            }
            ebr_lba = extended_base + link_start;
            if (depth + 1 == max_ebr_chain_length) {
                throw std::runtime_error("MBR EBR 链超过安全解析上限");
            }
        }
    }

    if (!protective_mbr || image.get_size() < 2 * sector_bytes) {
        return partitions;
    }
    std::array<std::byte, sector_bytes> gpt{};
    if (image.read_at(sector_bytes, gpt) != gpt.size() ||
        std::string_view(reinterpret_cast<const char*>(gpt.data()), 8) != "EFI PART") {
        return partitions;
    }
    const auto header_size = u32(gpt, 12);
    const auto entry_lba = u64(gpt, 72);
    const auto entry_count = u32(gpt, 80);
    const auto entry_size = u32(gpt, 84);
    const auto expected_header_crc = u32(gpt, 16);
    const auto expected_table_crc = u32(gpt, 88);
    if (header_size < 92 || header_size > sector_bytes ||
        entry_count > 4096 || entry_size < 128 || entry_size > 4096 ||
        entry_lba > std::numeric_limits<std::uint64_t>::max() / sector_bytes) {
        throw std::runtime_error("GPT 分区表头字段无效或超出安全解析上限");
    }
    std::vector<std::byte> checked_header(gpt.begin(),
        gpt.begin() + static_cast<std::ptrdiff_t>(header_size));
    std::fill(checked_header.begin() + 16, checked_header.begin() + 20, std::byte{0});
    if (crc32(checked_header) != expected_header_crc) {
        throw std::runtime_error("GPT 分区表头 CRC 校验失败");
    }

    const auto table_offset = entry_lba * sector_bytes;
    const auto table_size = static_cast<std::uint64_t>(entry_count) * entry_size;
    if (table_offset > image.get_size() || table_size > image.get_size() - table_offset ||
        table_size > 16U * 1024U * 1024U) {
        throw std::runtime_error("GPT 分区项数组超出镜像边界或安全解析上限");
    }
    std::vector<std::byte> table(static_cast<std::size_t>(table_size));
    if (image.read_at(table_offset, table) != table.size() ||
        crc32(table) != expected_table_crc) {
        throw std::runtime_error("GPT 分区项数组读取失败或 CRC 校验失败");
    }
    partitions.clear();
    for (std::uint32_t index = 0; index < entry_count; ++index) {
        const auto offset = static_cast<std::size_t>(index) * entry_size;
        const auto entry = std::span<const std::byte>(table).subspan(offset, entry_size);
        if (std::all_of(entry.begin(), entry.begin() + 16,
                        [](std::byte value) { return value == std::byte{0}; })) {
            continue;
        }
        const auto first_lba = u64(entry, 32);
        const auto last_lba = u64(entry, 40);
        if (last_lba < first_lba || first_lba >
            std::numeric_limits<std::uint64_t>::max() / sector_bytes) {
            continue;
        }
        const auto start = first_lba * sector_bytes;
        const auto span_lba = last_lba - first_lba;
        if (span_lba == std::numeric_limits<std::uint64_t>::max()) {
            continue;
        }
        const auto sector_count = span_lba + 1;
        if (sector_count > std::numeric_limits<std::uint64_t>::max() / sector_bytes) {
            continue;
        }
        const auto length = sector_count * sector_bytes;
        if (start <= image.get_size() && length <= image.get_size() - start) {
            partitions.push_back({start, length, "GPT",
                                  guid_type(entry.first(16))});
        }
    }
    return partitions;
}

} // namespace uife
