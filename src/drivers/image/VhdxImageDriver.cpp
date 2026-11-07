#include "uife/drivers/image/VhdxImageDriver.h"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <stdexcept>
#include <string_view>

namespace uife {
namespace {

constexpr std::uint64_t header_block_size = 64U * 1024U;
constexpr std::uint64_t header_section_size = 1024U * 1024U;
constexpr std::uint64_t maximum_vhdx_size = 64ULL * 1024U * 1024U * 1024U * 1024U;
constexpr std::uint64_t maximum_table_size = 64U * 1024U * 1024U;
constexpr std::uint32_t minimum_block_size = 1U * 1024U * 1024U;
constexpr std::uint32_t maximum_block_size = 256U * 1024U * 1024U;
constexpr std::uint64_t maximum_sectors_per_block = 1ULL << 23U;
constexpr std::uint64_t bat_offset_mask = 0xfffffffffff00000ULL;

constexpr std::array<std::byte, 16> bat_region_guid{
    std::byte{0x66}, std::byte{0x77}, std::byte{0xc2}, std::byte{0x2d},
    std::byte{0x23}, std::byte{0xf6}, std::byte{0x00}, std::byte{0x42},
    std::byte{0x9d}, std::byte{0x64}, std::byte{0x11}, std::byte{0x5e},
    std::byte{0x9b}, std::byte{0xfd}, std::byte{0x4a}, std::byte{0x08}};
constexpr std::array<std::byte, 16> metadata_region_guid{
    std::byte{0x06}, std::byte{0xa2}, std::byte{0x7c}, std::byte{0x8b},
    std::byte{0x90}, std::byte{0x47}, std::byte{0x9a}, std::byte{0x4b},
    std::byte{0xb8}, std::byte{0xfe}, std::byte{0x57}, std::byte{0x5f},
    std::byte{0x05}, std::byte{0x0f}, std::byte{0x88}, std::byte{0x6e}};
constexpr std::array<std::byte, 16> file_parameters_guid{
    std::byte{0x37}, std::byte{0x67}, std::byte{0xa1}, std::byte{0xca},
    std::byte{0x36}, std::byte{0xfa}, std::byte{0x43}, std::byte{0x4d},
    std::byte{0xb3}, std::byte{0xb6}, std::byte{0x33}, std::byte{0xf0},
    std::byte{0xaa}, std::byte{0x44}, std::byte{0xe7}, std::byte{0x6b}};
constexpr std::array<std::byte, 16> virtual_disk_size_guid{
    std::byte{0x24}, std::byte{0x42}, std::byte{0xa5}, std::byte{0x2f},
    std::byte{0x1b}, std::byte{0xcd}, std::byte{0x76}, std::byte{0x48},
    std::byte{0xb2}, std::byte{0x11}, std::byte{0x5d}, std::byte{0xbe},
    std::byte{0xd8}, std::byte{0x3b}, std::byte{0xf4}, std::byte{0xb8}};
constexpr std::array<std::byte, 16> logical_sector_size_guid{
    std::byte{0x1d}, std::byte{0xbf}, std::byte{0x41}, std::byte{0x81},
    std::byte{0x6f}, std::byte{0xa9}, std::byte{0x09}, std::byte{0x47},
    std::byte{0xba}, std::byte{0x47}, std::byte{0xf2}, std::byte{0x33},
    std::byte{0xa8}, std::byte{0xfa}, std::byte{0xab}, std::byte{0x5f}};
constexpr std::array<std::byte, 16> physical_sector_size_guid{
    std::byte{0xc7}, std::byte{0x48}, std::byte{0xa3}, std::byte{0xcd},
    std::byte{0x5d}, std::byte{0x44}, std::byte{0x71}, std::byte{0x44},
    std::byte{0x9c}, std::byte{0xc9}, std::byte{0xe9}, std::byte{0x88},
    std::byte{0x52}, std::byte{0x51}, std::byte{0xc5}, std::byte{0x56}};
constexpr std::array<std::byte, 16> virtual_disk_id_guid{
    std::byte{0xab}, std::byte{0x12}, std::byte{0xca}, std::byte{0xbe},
    std::byte{0xe6}, std::byte{0xb2}, std::byte{0x23}, std::byte{0x45},
    std::byte{0x93}, std::byte{0xef}, std::byte{0xc3}, std::byte{0x09},
    std::byte{0xe0}, std::byte{0x00}, std::byte{0xc7}, std::byte{0x46}};
constexpr std::array<std::byte, 16> parent_locator_guid{
    std::byte{0x2d}, std::byte{0x5f}, std::byte{0xd3}, std::byte{0xa8},
    std::byte{0x0b}, std::byte{0xb3}, std::byte{0x4d}, std::byte{0x45},
    std::byte{0xab}, std::byte{0xf7}, std::byte{0xd3}, std::byte{0xd8},
    std::byte{0x48}, std::byte{0x34}, std::byte{0xab}, std::byte{0x0c}};
// 规范原文：MS-VHDX 的 "Virtual Disk Type" metadata item
// （2dc1517d-dbd0-4482-a799-64d642c78d9f；2=Fixed、3=Dynamic、4=Differencing）。
constexpr std::array<std::byte, 16> virtual_disk_type_guid{
    std::byte{0x7d}, std::byte{0x51}, std::byte{0xc1}, std::byte{0x2d},
    std::byte{0xd0}, std::byte{0xdb}, std::byte{0x82}, std::byte{0x44},
    std::byte{0xa7}, std::byte{0x99}, std::byte{0x64}, std::byte{0xd6},
    std::byte{0x42}, std::byte{0xc7}, std::byte{0x8d}, std::byte{0x9f}};

std::uint32_t read_le32(std::span<const std::byte> bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset])) |
           (static_cast<std::uint32_t>(
                std::to_integer<unsigned char>(bytes[offset + 1])) << 8U) |
           (static_cast<std::uint32_t>(
                std::to_integer<unsigned char>(bytes[offset + 2])) << 16U) |
           (static_cast<std::uint32_t>(
                std::to_integer<unsigned char>(bytes[offset + 3])) << 24U);
}

std::uint16_t read_le16(std::span<const std::byte> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(
        std::to_integer<unsigned char>(bytes[offset])) |
        static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(
                std::to_integer<unsigned char>(bytes[offset + 1])) << 8U);
}

std::uint64_t read_le64(std::span<const std::byte> bytes, std::size_t offset) {
    return static_cast<std::uint64_t>(read_le32(bytes, offset)) |
           (static_cast<std::uint64_t>(read_le32(bytes, offset + 4)) << 32U);
}

std::uint32_t crc32c(std::span<const std::byte> bytes,
                     std::size_t checksum_offset) {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto value = index >= checksum_offset &&
                                   index < checksum_offset + sizeof(std::uint32_t)
                               ? 0U
                               : std::to_integer<unsigned char>(bytes[index]);
        crc ^= value;
        for (unsigned int bit = 0; bit < 8; ++bit) {
            const auto mask = static_cast<std::uint32_t>(
                -static_cast<std::int32_t>(crc & 1U));
            crc = (crc >> 1U) ^ (0x82f63b78U & mask);
        }
    }
    return ~crc;
}

bool has_signature(std::span<const std::byte> bytes, std::size_t offset,
                   std::string_view signature) {
    return offset <= bytes.size() &&
           signature.size() <= bytes.size() - offset &&
           std::equal(signature.begin(), signature.end(),
                      bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                      [](char left, std::byte right) {
                          return static_cast<unsigned char>(left) ==
                                 std::to_integer<unsigned char>(right);
                      });
}

bool overlaps(std::uint64_t first_offset, std::uint64_t first_size,
              std::uint64_t second_offset, std::uint64_t second_size) {
    return first_size != 0 && second_size != 0 &&
           first_offset < second_offset + second_size &&
           second_offset < first_offset + first_size;
}

struct Region {
    std::uint64_t offset{};
    std::uint64_t length{};
    bool required{};
};

struct Header {
    std::uint64_t sequence{};
    std::uint64_t log_offset{};
    std::uint32_t log_length{};
    // LogGuid 非零表示"日志里有尚未提交到镜像的条目"。这不是 header 损坏，
    // 而是需要日志重放——两者都让镜像目前不可读，但用户要做的事完全不同。
    bool log_pending{};
};

bool valid_header(std::span<const std::byte> header, Header& parsed) {
    if (header.size() != 4096 || !has_signature(header, 0, "head") ||
        read_le32(header, 4) != crc32c(header, 4) ||
        read_le16(header, 64) != 0 || read_le16(header, 66) != 1) {
        return false;
    }
    const auto log_guid = header.subspan(48, 16);
    parsed.log_pending = !std::all_of(log_guid.begin(), log_guid.end(),
                                      [](std::byte value) {
                                          return value == std::byte{0};
                                      });
    parsed.sequence = read_le64(header, 8);
    parsed.log_length = read_le32(header, 68);
    parsed.log_offset = read_le64(header, 72);
    if ((parsed.log_offset % header_block_size) != 0 ||
        (parsed.log_length % (1024U * 1024U)) != 0 ||
        (parsed.log_length != 0 &&
         (parsed.log_offset > std::numeric_limits<std::uint64_t>::max() -
                                  parsed.log_length))) {
        return false;
    }
    return true;
}

bool read_region_table(std::ifstream& input, std::uint64_t file_size,
                      std::uint64_t offset,
                      std::vector<std::byte>& table) {
    if (offset > file_size || table.size() > file_size - offset ||
        offset > static_cast<std::uint64_t>(
                     std::numeric_limits<std::streamoff>::max())) {
        return false;
    }
    input.clear();
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input) {
        return false;
    }
    input.read(reinterpret_cast<char*>(table.data()),
               static_cast<std::streamsize>(table.size()));
    return input.gcount() == static_cast<std::streamsize>(table.size()) &&
           has_signature(table, 0, "regi") &&
           read_le32(table, 4) == crc32c(table, 4);
}

std::map<std::array<std::byte, 16>, Region> parse_regions(
    std::span<const std::byte> table) {
    const auto entry_count = read_le32(table, 8);
    if (entry_count > 2047 || 16ULL + static_cast<std::uint64_t>(entry_count) * 32 >
                                  table.size() ||
        read_le32(table, 12) != 0) {
        throw std::runtime_error("VHDX region table 条目数量或保留字段无效");
    }
    std::map<std::array<std::byte, 16>, Region> regions;
    for (std::uint32_t index = 0; index < entry_count; ++index) {
        const auto entry_offset = 16U + static_cast<std::size_t>(index) * 32U;
        std::array<std::byte, 16> guid{};
        std::copy_n(table.begin() + static_cast<std::ptrdiff_t>(entry_offset),
                    guid.size(), guid.begin());
        const auto region_offset = read_le64(table, entry_offset + 16);
        const auto length = read_le32(table, entry_offset + 24);
        const auto flags = read_le32(table, entry_offset + 28);
        if (length == 0 || (region_offset % (1024U * 1024U)) != 0 ||
            (flags & ~1U) != 0 ||
            !regions.emplace(guid, Region{region_offset, length,
                                          (flags & 1U) != 0}).second) {
            throw std::runtime_error("VHDX region entry 无效或重复");
        }
    }
    return regions;
}

} // namespace

VhdxImageDriver::~VhdxImageDriver() {
    close();
}

void VhdxImageDriver::open(const std::filesystem::path& image_path) {
    close();
    std::error_code error;
    const auto file_size = std::filesystem::file_size(image_path, error);
    if (error || file_size < header_section_size ||
        file_size > static_cast<std::uint64_t>(
                        std::numeric_limits<std::streamoff>::max())) {
        throw std::runtime_error("VHDX 文件大小无效或无法读取");
    }
    std::ifstream candidate(image_path, std::ios::binary);
    if (!candidate) {
        throw std::runtime_error("无法以只读方式打开 VHDX 镜像");
    }
    std::array<std::byte, 64> file_identifier{};
    candidate.read(reinterpret_cast<char*>(file_identifier.data()),
                   static_cast<std::streamsize>(file_identifier.size()));
    if (candidate.gcount() != static_cast<std::streamsize>(file_identifier.size()) ||
        !has_signature(file_identifier, 0, "vhdxfile")) {
        throw std::runtime_error("VHDX 文件标识无效");
    }

    std::array<std::byte, 4096> header1_bytes{};
    std::array<std::byte, 4096> header2_bytes{};
    candidate.seekg(static_cast<std::streamoff>(header_block_size), std::ios::beg);
    candidate.read(reinterpret_cast<char*>(header1_bytes.data()),
                   static_cast<std::streamsize>(header1_bytes.size()));
    const bool header1_read =
        candidate.gcount() == static_cast<std::streamsize>(header1_bytes.size());
    candidate.clear();
    candidate.seekg(static_cast<std::streamoff>(2 * header_block_size), std::ios::beg);
    candidate.read(reinterpret_cast<char*>(header2_bytes.data()),
                   static_cast<std::streamsize>(header2_bytes.size()));
    const bool header2_read =
        candidate.gcount() == static_cast<std::streamsize>(header2_bytes.size());
    Header header1{};
    Header header2{};
    const bool header1_valid = header1_read && valid_header(header1_bytes, header1);
    const bool header2_valid = header2_read && valid_header(header2_bytes, header2);
    if (!header1_valid && !header2_valid) {
        throw std::runtime_error(
            "VHDX 两份 header 都无效（签名、校验和或版本不符）");
    }
    const auto header = !header2_valid ||
                               (header1_valid && header1.sequence >= header2.sequence)
                           ? header1
                           : header2;
    if (header.log_pending) {
        throw std::runtime_error(
            "VHDX 需要日志重放：header 的 LogGuid 非零，说明还有未提交的日志条目；"
            "当前驱动只读已落盘数据，不重放日志");
    }
    if (header.log_length != 0 &&
        (header.log_offset > file_size ||
         header.log_length > file_size - header.log_offset)) {
        throw std::runtime_error("VHDX 日志区域超出文件边界");
    }

    std::vector<std::byte> region_table(64U * 1024U);
    auto regions = read_region_table(candidate, file_size, 3 * header_block_size,
                                     region_table)
                       ? parse_regions(region_table)
                       : std::map<std::array<std::byte, 16>, Region>{};
    if (regions.empty()) {
        if (!read_region_table(candidate, file_size, 4 * header_block_size,
                               region_table)) {
            throw std::runtime_error("VHDX 两份 region table 均无效");
        }
        regions = parse_regions(region_table);
    }
    const auto bat_it = regions.find(bat_region_guid);
    const auto metadata_it = regions.find(metadata_region_guid);
    if (bat_it == regions.end() || metadata_it == regions.end() ||
        !bat_it->second.required || !metadata_it->second.required) {
        throw std::runtime_error("VHDX 缺少必需的 BAT 或 metadata region");
    }
    const auto bat_region = bat_it->second;
    const auto metadata_region = metadata_it->second;
    for (const auto& [guid, region] : regions) {
        if (guid != bat_region_guid && guid != metadata_region_guid &&
            region.required) {
            throw std::runtime_error("VHDX 含有当前不支持的必需 region");
        }
        if (region.offset > file_size || region.length > file_size - region.offset ||
            overlaps(region.offset, region.length, 0, header_section_size) ||
            (header.log_length != 0 &&
             overlaps(region.offset, region.length, header.log_offset,
                      header.log_length))) {
            throw std::runtime_error("VHDX region 超出边界或与保留区域重叠");
        }
    }
    for (auto first = regions.begin(); first != regions.end(); ++first) {
        for (auto second = std::next(first); second != regions.end(); ++second) {
            if (overlaps(first->second.offset, first->second.length,
                         second->second.offset, second->second.length)) {
                throw std::runtime_error("VHDX region 互相重叠");
            }
        }
    }
    if (bat_region.length > maximum_table_size) {
        throw std::runtime_error("VHDX BAT 超出安全解析上限");
    }

    std::array<std::byte, 32> metadata_header{};
    candidate.clear();
    candidate.seekg(static_cast<std::streamoff>(metadata_region.offset),
                   std::ios::beg);
    candidate.read(reinterpret_cast<char*>(metadata_header.data()),
                   static_cast<std::streamsize>(metadata_header.size()));
    if (candidate.gcount() != static_cast<std::streamsize>(metadata_header.size()) ||
        !has_signature(metadata_header, 0, "metadata") ||
        read_le16(metadata_header, 8) != 0 ||
        !std::all_of(metadata_header.begin() + 12, metadata_header.end(),
                     [](std::byte value) { return value == std::byte{0}; })) {
        throw std::runtime_error("VHDX metadata table header 无效");
    }
    const auto metadata_entry_count = read_le16(metadata_header, 10);
    if (metadata_entry_count == 0 || metadata_entry_count > 2047 ||
        32ULL + static_cast<std::uint64_t>(metadata_entry_count) * 32 >
            metadata_region.length) {
        throw std::runtime_error("VHDX metadata table 条目数量无效");
    }
    const auto metadata_table_size =
        32U + static_cast<std::size_t>(metadata_entry_count) * 32U;
    std::vector<std::byte> metadata_table(metadata_table_size);
    candidate.clear();
    candidate.seekg(static_cast<std::streamoff>(metadata_region.offset),
                   std::ios::beg);
    candidate.read(reinterpret_cast<char*>(metadata_table.data()),
                   static_cast<std::streamsize>(metadata_table.size()));
    if (candidate.gcount() != static_cast<std::streamsize>(metadata_table.size())) {
        throw std::runtime_error("读取 VHDX metadata table 失败");
    }

    struct MetadataItem {
        std::uint32_t offset{};
        std::uint32_t length{};
        std::uint32_t flags{};
    };
    std::map<std::array<std::byte, 16>, MetadataItem> metadata_items;
    for (std::uint16_t index = 0; index < metadata_entry_count; ++index) {
        const auto entry_offset = 32U + static_cast<std::size_t>(index) * 32U;
        std::array<std::byte, 16> guid{};
        std::copy_n(metadata_table.begin() +
                        static_cast<std::ptrdiff_t>(entry_offset),
                    guid.size(), guid.begin());
        const auto item_offset = read_le32(metadata_table, entry_offset + 16);
        const auto item_length = read_le32(metadata_table, entry_offset + 20);
        const auto flags = read_le32(metadata_table, entry_offset + 24);
        if (read_le32(metadata_table, entry_offset + 28) != 0 ||
            (flags & ~7U) != 0 ||
            (item_length == 0
                 ? item_offset != 0
                 : item_offset < 64U * 1024U ||
                       item_offset > metadata_region.length ||
                       item_length > metadata_region.length - item_offset) ||
            !metadata_items.emplace(
                guid, MetadataItem{item_offset, item_length, flags}).second) {
            throw std::runtime_error("VHDX metadata entry 无效或重复");
        }
        const bool known_item =
            guid == file_parameters_guid || guid == virtual_disk_size_guid ||
            guid == logical_sector_size_guid ||
            guid == physical_sector_size_guid || guid == virtual_disk_id_guid ||
            guid == metadata_region_guid || guid == parent_locator_guid ||
            guid == virtual_disk_type_guid;
        if (!known_item && (flags & 4U) != 0) {
            throw std::runtime_error("VHDX 含有当前不支持的必需 metadata item");
        }
    }
    const auto read_metadata_item = [&](const auto& guid,
                                        std::size_t expected_size) {
        const auto found = metadata_items.find(guid);
        if (found == metadata_items.end() ||
            found->second.length != expected_size) {
            throw std::runtime_error("VHDX 缺少必需或长度无效的 metadata item");
        }
        std::vector<std::byte> item(expected_size);
        candidate.clear();
        candidate.seekg(static_cast<std::streamoff>(
                           metadata_region.offset + found->second.offset),
                       std::ios::beg);
        candidate.read(reinterpret_cast<char*>(item.data()),
                       static_cast<std::streamsize>(item.size()));
        if (candidate.gcount() != static_cast<std::streamsize>(item.size())) {
            throw std::runtime_error("读取 VHDX metadata item 失败");
        }
        return item;
    };

    const auto file_parameters =
        read_metadata_item(file_parameters_guid, sizeof(std::uint64_t));
    const auto virtual_disk_size_bytes =
        read_metadata_item(virtual_disk_size_guid, sizeof(std::uint64_t));
    const auto logical_sector_size_bytes =
        read_metadata_item(logical_sector_size_guid, sizeof(std::uint32_t));
    const auto physical_sector_item = metadata_items.find(physical_sector_size_guid);
    const auto block_size = read_le32(file_parameters, 0);
    const auto file_parameter_flags = read_le32(file_parameters, 4);
    const auto virtual_size = read_le64(virtual_disk_size_bytes, 0);
    const auto logical_sector_size = read_le32(logical_sector_size_bytes, 0);
    std::uint32_t physical_sector_size = logical_sector_size;
    if (physical_sector_item != metadata_items.end() &&
        physical_sector_item->second.length != 0) {
        const auto physical_sector_size_bytes =
            read_metadata_item(physical_sector_size_guid, sizeof(std::uint32_t));
        physical_sector_size = read_le32(physical_sector_size_bytes, 0);
    }
    // 差分镜像（differencing）的所有识别途径都要拒绝，且各给各的报错：
    // ① File Parameters 的 Has Parent 标志（Hyper-V 实际写的就是这个）；
    // ② Parent Locator metadata item（规范要求差分盘携带）；
    // ③ Virtual Disk Type metadata item = 4（防止标志与盘类型互相矛盾的镜像
    //    被静默当成动态盘读出错误数据——对取证工具这是不可接受的）。
    if ((file_parameter_flags & 2U) != 0) {
        throw std::runtime_error(
            "VHDX 是差分镜像（File Parameters 的 Has Parent 标志置位）：数据分散在"
            "父盘链中，当前驱动只读自包含镜像，请先在 Hyper-V 中合并差分盘");
    }
    const auto parent_locator_item = metadata_items.find(parent_locator_guid);
    if (parent_locator_item != metadata_items.end() &&
        parent_locator_item->second.length != 0) {
        throw std::runtime_error(
            "VHDX 含有 Parent Locator metadata item，说明它是差分镜像：需要按父盘链"
            "合并读取，当前驱动只读自包含镜像");
    }
    const auto disk_type_item = metadata_items.find(virtual_disk_type_guid);
    if (disk_type_item != metadata_items.end() &&
        disk_type_item->second.length != 0) {
        if (disk_type_item->second.length != sizeof(std::uint32_t)) {
            throw std::runtime_error("VHDX Virtual Disk Type metadata item 长度无效");
        }
        const auto disk_type_bytes =
            read_metadata_item(virtual_disk_type_guid, sizeof(std::uint32_t));
        const auto disk_type = read_le32(disk_type_bytes, 0);
        if (disk_type == 4) {
            throw std::runtime_error(
                "VHDX 是差分镜像（Virtual Disk Type = 4）：需要按父盘链合并读取，"
                "当前驱动只读自包含镜像");
        }
        if (disk_type != 2 && disk_type != 3) {
            throw std::runtime_error(
                "VHDX Virtual Disk Type 值无效（既不是 Fixed/Dynamic 也不是 "
                "Differencing）");
        }
        if ((disk_type == 2) != ((file_parameter_flags & 1U) != 0)) {
            throw std::runtime_error(
                "VHDX File Parameters 的固定盘标志与 Virtual Disk Type 互相矛盾，"
                "拒绝解析");
        }
    }
    if (block_size < minimum_block_size || block_size > maximum_block_size ||
        (block_size & (block_size - 1U)) != 0 ||
        (file_parameter_flags & ~3U) != 0 ||
        (logical_sector_size != 512 && logical_sector_size != 4096) ||
        (physical_sector_size != 512 && physical_sector_size != 4096) ||
        physical_sector_size < logical_sector_size ||
        virtual_size == 0 || virtual_size > maximum_vhdx_size ||
        virtual_size % logical_sector_size != 0) {
        throw std::runtime_error("VHDX 几何参数无效");
    }
    const auto sectors_per_block = block_size / logical_sector_size;
    const auto chunk_ratio =
        (maximum_sectors_per_block * logical_sector_size) / block_size;
    if (sectors_per_block == 0 || chunk_ratio == 0 ||
        (chunk_ratio & (chunk_ratio - 1U)) != 0) {
        throw std::runtime_error("VHDX block/chunk 几何参数无效");
    }
    const auto payload_block_count =
        virtual_size / block_size + (virtual_size % block_size != 0 ? 1U : 0U);
    const auto bitmap_block_count =
        payload_block_count / chunk_ratio +
        (payload_block_count % chunk_ratio != 0 ? 1U : 0U);
    if (payload_block_count >
        (std::numeric_limits<std::uint64_t>::max() - bitmap_block_count) /
            sizeof(std::uint64_t)) {
        throw std::runtime_error("VHDX BAT 条目数量溢出");
    }
    const auto bat_entry_count = payload_block_count + bitmap_block_count;
    if (bat_entry_count > bat_region.length / sizeof(std::uint64_t)) {
        throw std::runtime_error("VHDX BAT region 不足以覆盖虚拟容量");
    }
    const auto bat_byte_count =
        static_cast<std::size_t>(bat_entry_count * sizeof(std::uint64_t));
    std::vector<std::byte> encoded_bat(bat_byte_count);
    candidate.clear();
    candidate.seekg(static_cast<std::streamoff>(bat_region.offset), std::ios::beg);
    candidate.read(reinterpret_cast<char*>(encoded_bat.data()),
                   static_cast<std::streamsize>(encoded_bat.size()));
    if (candidate.gcount() != static_cast<std::streamsize>(encoded_bat.size())) {
        throw std::runtime_error("读取 VHDX BAT 失败");
    }
    std::vector<PayloadBlock> payload_blocks;
    payload_blocks.reserve(static_cast<std::size_t>(payload_block_count));
    for (std::uint64_t index = 0; index < payload_block_count; ++index) {
        const auto bat_index = index + index / chunk_ratio;
        const auto entry = read_le64(encoded_bat,
                                     static_cast<std::size_t>(bat_index * 8));
        const auto state = static_cast<std::uint8_t>(entry & 7U);
        const auto physical_offset = entry & bat_offset_mask;
        if ((entry & ~(bat_offset_mask | 7ULL)) != 0 ||
            state == 4 || state == 5 || state == 7) {
            throw std::runtime_error(
                "VHDX BAT entry 使用了当前不支持的保留/partial state");
        }
        if ((file_parameter_flags & 1U) != 0 && state != 6) {
            throw std::runtime_error(
                "VHDX fixed image 包含未完全分配的 payload block");
        }
        if (state == 6) {
            if (physical_offset < header_section_size ||
                physical_offset % (1024U * 1024U) != 0 ||
                physical_offset > file_size ||
                block_size > file_size - physical_offset ||
                overlaps(physical_offset, block_size, bat_region.offset,
                         bat_region.length) ||
                overlaps(physical_offset, block_size, metadata_region.offset,
                         metadata_region.length) ||
                (header.log_length != 0 &&
                 overlaps(physical_offset, block_size, header.log_offset,
                          header.log_length))) {
                throw std::runtime_error(
                    "VHDX payload block 超出文件边界或与元数据重叠");
            }
        } else if (physical_offset != 0) {
            throw std::runtime_error(
                "非 allocated VHDX payload block 携带了无效文件偏移");
        }
        payload_blocks.push_back({physical_offset, state});
    }

    input_ = std::move(candidate);
    source_path_ = image_path;
    backing_size_ = file_size;
    virtual_size_ = virtual_size;
    block_size_ = block_size;
    chunk_ratio_ = chunk_ratio;
    logical_sector_size_ = logical_sector_size;
    payload_blocks_ = std::move(payload_blocks);
}

std::size_t VhdxImageDriver::read_sector(
    std::uint64_t sector_number, std::span<std::byte> destination) const {
    if (logical_sector_size_ == 0) {
        throw std::runtime_error("VHDX 镜像尚未打开");
    }
    if (destination.size() != sector_size()) {
        throw std::invalid_argument(
            "VHDX read_sector 要求 destination 长度等于逻辑扇区大小");
    }
    if (sector_number > std::numeric_limits<std::uint64_t>::max() /
                            logical_sector_size_) {
        throw std::out_of_range("VHDX 扇区号溢出");
    }
    return read_at(sector_number * logical_sector_size_, destination);
}

std::size_t VhdxImageDriver::read_at(
    std::uint64_t offset, std::span<std::byte> destination) const {
    std::scoped_lock lock(mutex_);
    if (!input_.is_open()) {
        throw std::runtime_error("VHDX 镜像尚未打开");
    }
    if (offset > virtual_size_) {
        throw std::out_of_range("读取偏移超出 VHDX 虚拟容量");
    }
    const auto count = static_cast<std::size_t>(
        std::min<std::uint64_t>(virtual_size_ - offset, destination.size()));
    std::size_t completed = 0;
    while (completed < count) {
        const auto virtual_offset = offset + completed;
        const auto block_index = virtual_offset / block_size_;
        const auto block_offset = virtual_offset % block_size_;
        const auto chunk_size = static_cast<std::size_t>(
            std::min<std::uint64_t>(block_size_ - block_offset,
                                    count - completed));
        const auto& block = payload_blocks_[static_cast<std::size_t>(block_index)];
        if (block.state == 6) {
            read_backing(block.file_offset + block_offset,
                         destination.subspan(completed, chunk_size));
        } else {
            std::fill_n(destination.begin() + static_cast<std::ptrdiff_t>(completed),
                        chunk_size, std::byte{0});
        }
        completed += chunk_size;
    }
    return count;
}

std::uint64_t VhdxImageDriver::get_size() const noexcept {
    return virtual_size_;
}

std::uint32_t VhdxImageDriver::sector_size() const noexcept {
    return logical_sector_size_;
}

std::filesystem::path VhdxImageDriver::source_path() const {
    return source_path_;
}

std::string_view VhdxImageDriver::format_name() const noexcept {
    return "VHDX";
}

void VhdxImageDriver::close() noexcept {
    std::scoped_lock lock(mutex_);
    if (input_.is_open()) {
        input_.close();
    }
    source_path_.clear();
    backing_size_ = 0;
    virtual_size_ = 0;
    block_size_ = 0;
    chunk_ratio_ = 0;
    logical_sector_size_ = 0;
    payload_blocks_.clear();
}

void VhdxImageDriver::read_backing(
    std::uint64_t offset, std::span<std::byte> destination) const {
    if (offset > backing_size_ || destination.size() > backing_size_ - offset ||
        offset > static_cast<std::uint64_t>(
                     std::numeric_limits<std::streamoff>::max())) {
        throw std::out_of_range("VHDX 后备文件读取范围无效");
    }
    input_.clear();
    input_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input_) {
        throw std::runtime_error("定位 VHDX 后备文件失败");
    }
    input_.read(reinterpret_cast<char*>(destination.data()),
                static_cast<std::streamsize>(destination.size()));
    if (input_.gcount() != static_cast<std::streamsize>(destination.size())) {
        throw std::runtime_error("读取 VHDX 后备文件时遇到意外 EOF");
    }
}

} // namespace uife
