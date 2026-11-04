#include "uife/drivers/image/VmdkSparseImageDriver.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace uife {
namespace {

constexpr std::uint32_t sparse_magic = 0x564d444bU;
constexpr std::uint64_t sector_size_bytes = 512;
constexpr std::uint32_t zeroed_grain_gte_flag = 0x4U;
constexpr std::uint32_t compressed_grain_flag = 0x10000U;
constexpr std::uint32_t grain_marker_flag = 0x20000U;
constexpr std::uint32_t supported_flags =
    0x1U | 0x2U | zeroed_grain_gte_flag | compressed_grain_flag;
constexpr std::uint64_t max_grain_directory_bytes = 16U * 1024U * 1024U;
constexpr std::uint64_t max_descriptor_bytes = 1024U * 1024U;

struct FlatExtent {
    std::filesystem::path path;
    std::uint64_t sectors{};
    std::uint64_t offset_sectors{};
};

std::uint32_t decode_u32(std::span<const std::byte> data, std::size_t offset) {
    return static_cast<std::uint32_t>(std::to_integer<unsigned char>(data[offset])) |
           (static_cast<std::uint32_t>(
                std::to_integer<unsigned char>(data[offset + 1])) << 8U) |
           (static_cast<std::uint32_t>(
                std::to_integer<unsigned char>(data[offset + 2])) << 16U) |
           (static_cast<std::uint32_t>(
                std::to_integer<unsigned char>(data[offset + 3])) << 24U);
}

std::uint64_t decode_u64(std::span<const std::byte> data, std::size_t offset) {
    return static_cast<std::uint64_t>(decode_u32(data, offset)) |
           (static_cast<std::uint64_t>(decode_u32(data, offset + 4)) << 32U);
}

std::uint64_t parse_decimal(std::string_view value, const char* field_name) {
    std::uint64_t parsed_value = 0;
    const auto parsed = std::from_chars(
        value.data(), value.data() + value.size(), parsed_value);
    if (value.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != value.data() + value.size()) {
        throw std::runtime_error(std::string("VMDK descriptor ") + field_name +
                                 " 无效");
    }
    return parsed_value;
}

std::filesystem::path resolve_flat_path(
    const std::filesystem::path& descriptor_path,
    const std::filesystem::path& relative_path) {
    if (relative_path.empty() || relative_path.is_absolute() ||
        relative_path.has_root_name() || relative_path.has_root_directory()) {
        throw std::runtime_error(
            "VMDK FLAT extent 仅允许使用 descriptor 目录内的相对路径");
    }
    for (const auto& component : relative_path) {
        if (component == "." || component == "..") {
            throw std::runtime_error(
                "VMDK FLAT extent 路径不能离开 descriptor 所在目录");
        }
    }

    std::error_code error;
    const auto parent = descriptor_path.parent_path().empty()
        ? std::filesystem::current_path(error)
        : descriptor_path.parent_path();
    if (error) {
        throw std::runtime_error("无法读取当前目录: " + error.message());
    }
    const auto descriptor_directory =
        std::filesystem::weakly_canonical(parent, error);
    if (error) {
        throw std::runtime_error("无法解析 VMDK descriptor 目录: " +
                                 error.message());
    }
    const auto extent_path = std::filesystem::canonical(
        descriptor_directory / relative_path, error);
    if (error) {
        throw std::runtime_error("无法解析 VMDK FLAT extent 路径: " +
                                 error.message());
    }
    const auto relative_extent =
        extent_path.lexically_relative(descriptor_directory);
    if (relative_extent.empty() || relative_extent.is_absolute() ||
        *relative_extent.begin() == "..") {
        throw std::runtime_error(
            "VMDK FLAT extent 不能通過符号链接离开 descriptor 所在目录");
    }
    return extent_path;
}

std::vector<FlatExtent> read_flat_extents(
    const std::filesystem::path& descriptor_path) {
    std::error_code error;
    const auto descriptor_size =
        std::filesystem::file_size(descriptor_path, error);
    if (error || descriptor_size == 0 || descriptor_size > max_descriptor_bytes) {
        throw std::runtime_error("VMDK descriptor 大小无效或超出安全解析上限");
    }
    std::ifstream descriptor(descriptor_path, std::ios::binary);
    if (!descriptor) {
        throw std::runtime_error("无法只读打开 VMDK descriptor");
    }

    bool found_header = false;
    bool found_version = false;
    std::string cid;
    std::string create_type;
    std::string parent_cid;
    bool found_cid = false;
    bool found_create_type = false;
    bool found_parent_cid = false;
    bool has_parent_file_hint = false;
    std::vector<FlatExtent> extents;
    std::string line;
    while (std::getline(descriptor, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!found_header) {
            if (line != "# Disk DescriptorFile") {
                throw std::runtime_error("VMDK descriptor 文件头无效");
            }
            found_header = true;
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }

        if (line.front() == '"') {
            std::istringstream setting(line);
            std::string key;
            std::string value;
            char equals = '\0';
            if (!(setting >> std::quoted(key) >> equals >> std::quoted(value)) ||
                equals != '=') {
                throw std::runtime_error(
                    "VMDK descriptor quoted setting 格式无效");
            }
            if (key == "createType") {
                if (found_create_type || value.empty()) {
                    throw std::runtime_error(
                        "VMDK descriptor createType 缺失或重复");
                }
                create_type = value;
                found_create_type = true;
            } else if (key == "parentFileNameHint") {
                has_parent_file_hint = true;
            }
            continue;
        }

        if (line.starts_with("CID=")) {
            cid = line.substr(std::string_view("CID=").size());
            std::transform(cid.begin(), cid.end(), cid.begin(),
                           [](unsigned char character) {
                               return static_cast<char>(std::tolower(character));
                           });
            if (found_cid || cid.size() != 8 ||
                !std::all_of(cid.begin(), cid.end(),
                             [](unsigned char character) {
                                 return std::isxdigit(character) != 0;
                             })) {
                throw std::runtime_error("VMDK descriptor CID 格式无效");
            }
            found_cid = true;
            continue;
        }
        if (line.starts_with("version=")) {
            if (found_version ||
                line.substr(std::string_view("version=").size()) != "1") {
                throw std::runtime_error(
                    "当前 VMDK descriptor 仅支持唯一且为 1 的 version 字段");
            }
            found_version = true;
            continue;
        }
        if (line.starts_with("parentCID=")) {
            parent_cid = line.substr(std::string_view("parentCID=").size());
            std::transform(parent_cid.begin(), parent_cid.end(),
                           parent_cid.begin(), [](unsigned char character) {
                               return static_cast<char>(std::tolower(character));
                           });
            if (found_parent_cid || parent_cid.size() != 8 ||
                !std::all_of(parent_cid.begin(), parent_cid.end(),
                             [](unsigned char character) {
                                 return std::isxdigit(character) != 0;
                             })) {
                throw std::runtime_error(
                    "VMDK descriptor parentCID 格式无效");
            }
            found_parent_cid = true;
            continue;
        }
        std::istringstream entry(line);
        std::string access;
        if (!(entry >> access)) {
            continue;
        }
        if (access != "RW" && access != "RDONLY") {
            if (access == "NOACCESS") {
                throw std::runtime_error(
                    "VMDK descriptor 含有不可读取的 NOACCESS extent");
            }
            if (access.starts_with("ddb.")) {
                continue;
            }
            throw std::runtime_error(
                "VMDK descriptor 含有不支持或无效的 extent 声明");
        }
        std::string sector_count_text;
        std::string extent_type;
        std::string file_name;
        std::string offset_text;
        if (!(entry >> sector_count_text >> extent_type >>
              std::quoted(file_name) >> offset_text)) {
            throw std::runtime_error("VMDK extent descriptor 格式无效");
        }
        std::string extra;
        if (entry >> extra) {
            throw std::runtime_error("VMDK extent descriptor 含有多余字段");
        }
        if (extent_type != "FLAT" || file_name.empty()) {
            throw std::runtime_error(
                "当前 VMDK descriptor 驱动仅支持 FLAT extent");
        }
        FlatExtent extent;
        extent.sectors = parse_decimal(sector_count_text, "extent sector count");
        extent.offset_sectors = parse_decimal(offset_text, "extent offset");
        std::u8string encoded_filename;
        encoded_filename.reserve(file_name.size());
        for (const unsigned char character : file_name) {
            encoded_filename.push_back(static_cast<char8_t>(character));
        }
        extent.path = std::filesystem::path(encoded_filename);
        extents.push_back(std::move(extent));
    }
    if (!descriptor.eof()) {
        throw std::runtime_error("读取 VMDK descriptor 失败");
    }
    if (!found_header || !found_version || !found_cid || !found_create_type ||
        !found_parent_cid || has_parent_file_hint ||
        (!parent_cid.empty() && parent_cid != "ffffffff") ||
        (create_type != "monolithicFlat" &&
         create_type != "twoGbMaxExtentFlat") ||
        extents.empty() ||
        (create_type == "monolithicFlat" && extents.size() != 1) ||
        std::any_of(extents.begin(), extents.end(),
                    [](const FlatExtent& extent) {
                        return extent.sectors == 0;
                    })) {
        throw std::runtime_error(
            "当前 VMDK descriptor 驱动仅支持无父盘的 monolithicFlat 单 extent 或 twoGbMaxExtentFlat FLAT extents");
    }
    if (create_type == "twoGbMaxExtentFlat" &&
        std::any_of(extents.begin(), extents.end(),
                    [](const FlatExtent& extent) {
                        constexpr std::uint64_t max_extent_sectors =
                            (std::uint64_t{2} * 1024U * 1024U * 1024U) /
                            sector_size_bytes;
                        return extent.sectors > max_extent_sectors;
                    })) {
        throw std::runtime_error(
            "twoGbMaxExtentFlat 中单个 FLAT extent 超过 2 GiB 限制");
    }

    for (auto& extent : extents) {
        extent.path = resolve_flat_path(descriptor_path, extent.path);
        if (extent.sectors >
                std::numeric_limits<std::uint64_t>::max() / sector_size_bytes ||
            extent.offset_sectors >
                std::numeric_limits<std::uint64_t>::max() / sector_size_bytes) {
            throw std::runtime_error("VMDK FLAT extent 几何参数溢出");
        }
    }
    return extents;
}

} // namespace

VmdkSparseImageDriver::~VmdkSparseImageDriver() {
    close();
}

void VmdkSparseImageDriver::open(const std::filesystem::path& image_path) {
    close();
    std::error_code error;
    const auto file_size = std::filesystem::file_size(image_path, error);
    if (error || file_size == 0 ||
        file_size > static_cast<std::uint64_t>(
            std::numeric_limits<std::streamoff>::max())) {
        throw std::runtime_error("VMDK 文件大小无效或无法读取");
    }

    std::ifstream candidate(image_path, std::ios::binary);
    if (!candidate) {
        throw std::runtime_error("无法以只读方式打开 VMDK 镜像");
    }
    std::array<std::byte, 4> magic{};
    candidate.read(reinterpret_cast<char*>(magic.data()),
                   static_cast<std::streamsize>(magic.size()));
    if (candidate.gcount() != static_cast<std::streamsize>(magic.size()) ||
        decode_u32(magic, 0) != sparse_magic) {
        candidate.close();
        const auto extents = read_flat_extents(image_path);
        std::vector<FlatExtentInput> inputs;
        inputs.reserve(extents.size());
        std::uint64_t virtual_size = 0;
        for (const auto& extent : extents) {
            std::error_code extent_error;
            const auto extent_file_size =
                std::filesystem::file_size(extent.path, extent_error);
            if (extent_error || extent_file_size >
                    static_cast<std::uint64_t>(
                        std::numeric_limits<std::streamoff>::max())) {
                throw std::runtime_error(
                    "VMDK FLAT extent 文件无法读取或大小无效");
            }
            const auto extent_size = extent.sectors * sector_size_bytes;
            const auto data_offset = extent.offset_sectors * sector_size_bytes;
            if (data_offset > extent_file_size ||
                extent_size > extent_file_size - data_offset ||
                extent_size >
                    std::numeric_limits<std::uint64_t>::max() - virtual_size) {
                throw std::runtime_error(
                    "VMDK FLAT extent 超出后备文件边界或虚拟容量溢出");
            }
            std::ifstream flat_file(extent.path, std::ios::binary);
            if (!flat_file) {
                throw std::runtime_error(
                    "无法只读打开 VMDK FLAT extent 文件");
            }
            inputs.push_back({std::move(flat_file), virtual_size, extent_size,
                              extent_file_size, data_offset});
            virtual_size += extent_size;
        }
        flat_extents_ = std::move(inputs);
        source_path_ = image_path;
        virtual_size_ = virtual_size;
        flat_extent_ = true;
        return;
    }
    candidate.clear();
    candidate.seekg(0, std::ios::beg);
    std::array<std::byte, sector_size_bytes> header{};
    candidate.read(reinterpret_cast<char*>(header.data()),
                   static_cast<std::streamsize>(header.size()));
    if (candidate.gcount() != static_cast<std::streamsize>(header.size()) ||
        decode_u32(header, 0) != sparse_magic) {
        throw std::runtime_error("VMDK sparse header 无效");
    }

    const auto version = decode_u32(header, 4);
    const auto flags = decode_u32(header, 8);
    const auto capacity_sectors = decode_u64(header, 12);
    const auto grain_size_sectors = decode_u64(header, 20);
    const auto entries_per_table = decode_u32(header, 44);
    const auto grain_directory_sector = decode_u64(header, 56);
    const auto compression_algorithm =
        std::to_integer<unsigned char>(header[77]) |
        (std::to_integer<unsigned char>(header[78]) << 8U);
    if (version != 1 || capacity_sectors == 0 || grain_size_sectors == 0 ||
        (grain_size_sectors & (grain_size_sectors - 1U)) != 0 ||
        grain_size_sectors >
            std::numeric_limits<std::uint64_t>::max() / sector_size_bytes ||
        entries_per_table == 0 || grain_directory_sector == 0) {
        throw std::runtime_error("VMDK sparse header 含有不支持或无效的几何参数");
    }
    if ((flags & compressed_grain_flag) != 0 || compression_algorithm != 0) {
        throw std::runtime_error("当前 VMDK 驱动不支持压缩 grain");
    }
    if ((flags & grain_marker_flag) != 0 ||
        (flags & ~supported_flags) != 0) {
        throw std::runtime_error("当前 VMDK 驱动不支持该 sparse header 标志");
    }
    if (capacity_sectors >
        std::numeric_limits<std::uint64_t>::max() / sector_size_bytes) {
        throw std::runtime_error("VMDK 虚拟容量超出可支持范围");
    }
    if (grain_directory_sector >
        std::numeric_limits<std::uint64_t>::max() / sector_size_bytes) {
        throw std::runtime_error("VMDK grain directory 偏移溢出");
    }

    const auto virtual_size = capacity_sectors * sector_size_bytes;
    const auto grain_count = capacity_sectors / grain_size_sectors +
        (capacity_sectors % grain_size_sectors != 0 ? 1U : 0U);
    const auto directory_entry_count = grain_count / entries_per_table +
        (grain_count % entries_per_table != 0 ? 1U : 0U);
    if (directory_entry_count >
        max_grain_directory_bytes / sizeof(std::uint32_t)) {
        throw std::runtime_error("VMDK grain directory 超出安全解析上限");
    }
    const auto directory_bytes =
        directory_entry_count * sizeof(std::uint32_t);
    const auto directory_offset = grain_directory_sector * sector_size_bytes;
    if (directory_offset > file_size ||
        directory_bytes > file_size - directory_offset ||
        directory_offset > static_cast<std::uint64_t>(
            std::numeric_limits<std::streamoff>::max())) {
        throw std::runtime_error("VMDK grain directory 超出文件边界");
    }

    std::vector<std::uint32_t> grain_directory(
        static_cast<std::size_t>(directory_entry_count));
    if (!grain_directory.empty()) {
        candidate.clear();
        candidate.seekg(static_cast<std::streamoff>(directory_offset), std::ios::beg);
        if (!candidate) {
            throw std::runtime_error("无法定位 VMDK grain directory");
        }
        std::vector<std::byte> encoded_directory(
            static_cast<std::size_t>(directory_bytes));
        candidate.read(reinterpret_cast<char*>(encoded_directory.data()),
                       static_cast<std::streamsize>(encoded_directory.size()));
        if (candidate.gcount() !=
            static_cast<std::streamsize>(encoded_directory.size())) {
            throw std::runtime_error("读取 VMDK grain directory 失败");
        }
        for (std::size_t index = 0; index < grain_directory.size(); ++index) {
            grain_directory[index] = decode_u32(encoded_directory, index * 4U);
        }
    }

    input_ = std::move(candidate);
    flat_extents_.clear();
    source_path_ = image_path;
    backing_size_ = file_size;
    virtual_size_ = virtual_size;
    grain_size_sectors_ = grain_size_sectors;
    entries_per_grain_table_ = entries_per_table;
    flags_ = flags;
    flat_extent_ = false;
    grain_directory_ = std::move(grain_directory);
}

std::size_t VmdkSparseImageDriver::read_sector(
    std::uint64_t sector_number, std::span<std::byte> destination) const {
    if (destination.size() != sector_size()) {
        throw std::invalid_argument("VMDK read_sector 要求恰好一个 512 字节扇区");
    }
    if (sector_number > std::numeric_limits<std::uint64_t>::max() / sector_size()) {
        throw std::out_of_range("VMDK 扇区号溢出");
    }
    return read_at(sector_number * sector_size(), destination);
}

std::size_t VmdkSparseImageDriver::read_at(
    std::uint64_t offset, std::span<std::byte> destination) const {
    std::scoped_lock lock(mutex_);
    if (!flat_extent_ && !input_.is_open()) {
        throw std::runtime_error("VMDK 镜像尚未打开");
    }
    if (offset > virtual_size_) {
        throw std::out_of_range("读取偏移超出 VMDK 虚拟容量");
    }
    const auto count = static_cast<std::size_t>(
        std::min<std::uint64_t>(virtual_size_ - offset, destination.size()));
    if (flat_extent_) {
        std::size_t completed = 0;
        while (completed < count) {
            const auto logical_offset = offset + completed;
            const auto extent = std::upper_bound(
                flat_extents_.begin(), flat_extents_.end(), logical_offset,
                [](std::uint64_t value, const FlatExtentInput& candidate) {
                    return value < candidate.logical_offset;
                });
            if (extent == flat_extents_.begin()) {
                throw std::runtime_error(
                    "VMDK FLAT 逻辑偏移未映射到任何 extent");
            }
            auto& current = *std::prev(extent);
            const auto extent_offset =
                logical_offset - current.logical_offset;
            if (extent_offset >= current.length) {
                throw std::runtime_error(
                    "VMDK FLAT 逻辑读取超出 extent 范围");
            }
            const auto chunk = static_cast<std::size_t>(
                std::min<std::uint64_t>(count - completed,
                                        current.length - extent_offset));
            const auto physical_offset = current.data_offset + extent_offset;
            if (physical_offset >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::streamoff>::max())) {
                throw std::out_of_range(
                    "VMDK FLAT 读取偏移超出当前平台文件流范围");
            }
            current.input.clear();
            current.input.seekg(static_cast<std::streamoff>(physical_offset),
                                std::ios::beg);
            if (!current.input) {
                throw std::runtime_error(
                    "定位 VMDK FLAT extent 文件失败");
            }
            current.input.read(
                reinterpret_cast<char*>(destination.data() + completed),
                static_cast<std::streamsize>(chunk));
            if (current.input.gcount() != static_cast<std::streamsize>(chunk)) {
                throw std::runtime_error(
                    "读取 VMDK FLAT extent 时遇到意外 EOF");
            }
            completed += chunk;
        }
        return count;
    }
    std::size_t completed = 0;
    const auto grain_size_bytes = grain_size_sectors_ * sector_size_bytes;
    while (completed < count) {
        const auto virtual_offset = offset + completed;
        const auto grain_index = virtual_offset / grain_size_bytes;
        const auto grain_offset = virtual_offset % grain_size_bytes;
        const auto chunk_size = static_cast<std::size_t>(
            std::min<std::uint64_t>(grain_size_bytes - grain_offset,
                                    count - completed));
        const auto directory_index = grain_index / entries_per_grain_table_;
        const auto table_index = grain_index % entries_per_grain_table_;
        if (directory_index >= grain_directory_.size()) {
            throw std::runtime_error("VMDK grain directory 索引超出范围");
        }
        const auto table_sector = grain_directory_[static_cast<std::size_t>(directory_index)];
        if (table_sector == 0) {
            std::fill_n(destination.begin() + static_cast<std::ptrdiff_t>(completed),
                        chunk_size, std::byte{0});
            completed += chunk_size;
            continue;
        }

        const auto table_offset =
            static_cast<std::uint64_t>(table_sector) * sector_size_bytes;
        const auto entry_offset = table_offset + table_index * sizeof(std::uint32_t);
        if (entry_offset > backing_size_ ||
            sizeof(std::uint32_t) > backing_size_ - entry_offset) {
            throw std::runtime_error("VMDK grain table entry 超出文件边界");
        }
        const auto grain_sector = read_u32_at(entry_offset);
        if (grain_sector == 0 ||
            (((flags_ & zeroed_grain_gte_flag) != 0) && grain_sector == 1)) {
            std::fill_n(destination.begin() + static_cast<std::ptrdiff_t>(completed),
                        chunk_size, std::byte{0});
            completed += chunk_size;
            continue;
        }
        if (grain_sector >
            std::numeric_limits<std::uint64_t>::max() / sector_size_bytes) {
            throw std::runtime_error("VMDK data grain 偏移溢出");
        }
        const auto data_offset =
            static_cast<std::uint64_t>(grain_sector) * sector_size_bytes;
        const auto grain_bytes = grain_size_sectors_ * sector_size_bytes;
        if (data_offset > backing_size_ || grain_bytes > backing_size_ - data_offset) {
            throw std::runtime_error("VMDK data grain 超出文件边界");
        }
        read_backing(data_offset + grain_offset,
                     destination.subspan(completed, chunk_size));
        completed += chunk_size;
    }
    return count;
}

std::uint64_t VmdkSparseImageDriver::get_size() const noexcept {
    return virtual_size_;
}

std::uint32_t VmdkSparseImageDriver::sector_size() const noexcept {
    return static_cast<std::uint32_t>(sector_size_bytes);
}

std::filesystem::path VmdkSparseImageDriver::source_path() const {
    return source_path_;
}

std::string_view VmdkSparseImageDriver::format_name() const noexcept {
    return "VMDK";
}

void VmdkSparseImageDriver::close() noexcept {
    std::scoped_lock lock(mutex_);
    if (input_.is_open()) {
        input_.close();
    }
    for (auto& extent : flat_extents_) {
        if (extent.input.is_open()) {
            extent.input.close();
        }
    }
    flat_extents_.clear();
    source_path_.clear();
    backing_size_ = 0;
    virtual_size_ = 0;
    grain_size_sectors_ = 0;
    entries_per_grain_table_ = 0;
    flags_ = 0;
    flat_extent_ = false;
    grain_directory_.clear();
}

std::uint32_t VmdkSparseImageDriver::read_u32_at(std::uint64_t offset) const {
    std::array<std::byte, sizeof(std::uint32_t)> encoded{};
    read_backing(offset, encoded);
    return decode_u32(encoded, 0);
}

void VmdkSparseImageDriver::read_backing(
    std::uint64_t offset, std::span<std::byte> destination) const {
    if (offset > backing_size_ || destination.size() > backing_size_ - offset ||
        offset > static_cast<std::uint64_t>(
            std::numeric_limits<std::streamoff>::max())) {
        throw std::out_of_range("VMDK 后备文件读取范围无效");
    }
    input_.clear();
    input_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input_) {
        throw std::runtime_error("定位 VMDK 后备文件失败");
    }
    input_.read(reinterpret_cast<char*>(destination.data()),
                static_cast<std::streamsize>(destination.size()));
    if (input_.gcount() != static_cast<std::streamsize>(destination.size())) {
        throw std::runtime_error("读取 VMDK 后备文件时遇到意外 EOF");
    }
}

} // namespace uife
