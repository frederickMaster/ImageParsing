#include "uife/drivers/image/VdiImageDriver.h"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace uife {
namespace {

constexpr std::uint32_t vdi_signature = 0xbeda107fU;
constexpr std::uint32_t vdi_version_1_1 = 0x00010001U;
constexpr std::uint32_t vdi_type_dynamic = 1;
constexpr std::uint32_t vdi_type_fixed = 2;
constexpr std::uint32_t vdi_block_discarded = 0xfffffffeU;
constexpr std::uint32_t vdi_block_unallocated = 0xffffffffU;
constexpr std::uint64_t sector_size_bytes = 512;
constexpr std::uint64_t maximum_block_map_bytes = 64U * 1024U * 1024U;
constexpr std::uint32_t maximum_block_size = 64U * 1024U * 1024U;

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

} // namespace

VdiImageDriver::~VdiImageDriver() {
    close();
}

void VdiImageDriver::open(const std::filesystem::path& image_path) {
    close();
    std::error_code error;
    const auto file_size = std::filesystem::file_size(image_path, error);
    if (error || file_size < 512 ||
        file_size > static_cast<std::uint64_t>(
            std::numeric_limits<std::streamoff>::max())) {
        throw std::runtime_error("VDI 文件大小无效或无法读取");
    }
    std::ifstream candidate(image_path, std::ios::binary);
    if (!candidate) {
        throw std::runtime_error("无法以只读方式打开 VDI 镜像");
    }
    std::array<std::byte, 512> header{};
    candidate.read(reinterpret_cast<char*>(header.data()),
                   static_cast<std::streamsize>(header.size()));
    if (candidate.gcount() != static_cast<std::streamsize>(header.size()) ||
        decode_u32(header, 64) != vdi_signature ||
        decode_u32(header, 68) != vdi_version_1_1) {
        throw std::runtime_error("VDI 预头签名或版本无效/不支持");
    }

    const auto header_size = decode_u32(header, 72);
    const auto image_type = decode_u32(header, 76);
    const auto block_map_offset = decode_u32(header, 340);
    const auto data_offset = decode_u32(header, 344);
    const auto disk_size = decode_u64(header, 368);
    const auto block_size = decode_u32(header, 376);
    const auto block_extra = decode_u32(header, 380);
    const auto block_count = decode_u32(header, 384);
    const auto allocated_block_count = decode_u32(header, 388);
    if (header_size < 400 || header_size > file_size - 72 ||
        (image_type != vdi_type_dynamic && image_type != vdi_type_fixed)) {
        throw std::runtime_error(
            "当前 VDI 驱动只支持有效的 VDI 1.1 动态或固定镜像");
    }
    if (disk_size == 0 || disk_size > std::numeric_limits<std::uint64_t>::max() -
            (sector_size_bytes - 1)) {
        throw std::runtime_error("VDI 虚拟磁盘容量无效");
    }
    if (block_size < sector_size_bytes || block_size > maximum_block_size ||
        (block_size & (block_size - 1U)) != 0 ||
        block_size % sector_size_bytes != 0 || block_extra != 0) {
        throw std::runtime_error("VDI block 几何参数无效或包含不支持的扩展数据");
    }
    const auto expected_block_count = disk_size / block_size +
        (disk_size % block_size != 0 ? 1U : 0U);
    if (block_count == 0 || block_count != expected_block_count ||
        allocated_block_count > block_count) {
        throw std::runtime_error("VDI block 数量与虚拟容量不匹配");
    }
    if (block_map_offset < 512 || block_map_offset > file_size ||
        data_offset < block_map_offset || data_offset > file_size) {
        throw std::runtime_error("VDI block map 或数据区偏移无效");
    }
    const auto block_map_bytes =
        static_cast<std::uint64_t>(block_count) * sizeof(std::uint32_t);
    if (block_map_bytes > maximum_block_map_bytes ||
        block_map_bytes > file_size - block_map_offset) {
        throw std::runtime_error("VDI block map 超出文件边界或安全解析上限");
    }

    std::vector<std::byte> encoded_map(static_cast<std::size_t>(block_map_bytes));
    candidate.clear();
    candidate.seekg(static_cast<std::streamoff>(block_map_offset), std::ios::beg);
    if (!candidate) {
        throw std::runtime_error("无法定位 VDI block map");
    }
    candidate.read(reinterpret_cast<char*>(encoded_map.data()),
                   static_cast<std::streamsize>(encoded_map.size()));
    if (candidate.gcount() != static_cast<std::streamsize>(encoded_map.size())) {
        throw std::runtime_error("读取 VDI block map 失败");
    }

    std::vector<std::uint32_t> block_map(block_count);
    std::uint32_t allocated_seen = 0;
    for (std::size_t index = 0; index < block_map.size(); ++index) {
        const auto physical_block = decode_u32(encoded_map, index * 4U);
        block_map[index] = physical_block;
        if (physical_block < vdi_block_discarded) {
            if (physical_block >= allocated_block_count) {
                throw std::runtime_error("VDI block map 指向无效的物理 block");
            }
            const auto physical_offset = static_cast<std::uint64_t>(data_offset) +
                static_cast<std::uint64_t>(physical_block) * block_size;
            const auto bytes_in_block = std::min<std::uint64_t>(
                block_size, disk_size - static_cast<std::uint64_t>(index) * block_size);
            if (physical_offset > file_size ||
                bytes_in_block > file_size - physical_offset) {
                throw std::runtime_error("VDI 已分配 block 超出后备文件边界");
            }
            ++allocated_seen;
        }
    }
    if (allocated_seen != allocated_block_count) {
        throw std::runtime_error("VDI 已分配 block 数量与 block map 不匹配");
    }

    input_ = std::move(candidate);
    source_path_ = image_path;
    backing_size_ = file_size;
    virtual_size_ = disk_size;
    data_offset_ = data_offset;
    block_size_ = block_size;
    block_map_ = std::move(block_map);
}

std::size_t VdiImageDriver::read_sector(
    std::uint64_t sector_number, std::span<std::byte> destination) const {
    if (destination.size() != sector_size()) {
        throw std::invalid_argument("VDI read_sector 要求恰好一个 512 字节扇区");
    }
    if (sector_number > std::numeric_limits<std::uint64_t>::max() / sector_size()) {
        throw std::out_of_range("VDI 扇区号溢出");
    }
    return read_at(sector_number * sector_size(), destination);
}

std::size_t VdiImageDriver::read_at(
    std::uint64_t offset, std::span<std::byte> destination) const {
    std::scoped_lock lock(mutex_);
    if (!input_.is_open()) {
        throw std::runtime_error("VDI 镜像尚未打开");
    }
    if (offset > virtual_size_) {
        throw std::out_of_range("读取偏移超出 VDI 虚拟容量");
    }
    const auto count = static_cast<std::size_t>(
        std::min<std::uint64_t>(virtual_size_ - offset, destination.size()));
    std::size_t completed = 0;
    while (completed < count) {
        const auto virtual_offset = offset + completed;
        const auto block_index = virtual_offset / block_size_;
        const auto block_offset = virtual_offset % block_size_;
        const auto chunk_size = static_cast<std::size_t>(
            std::min<std::uint64_t>(block_size_ - block_offset, count - completed));
        const auto mapping = block_map_[static_cast<std::size_t>(block_index)];
        if (mapping >= vdi_block_discarded) {
            std::fill_n(destination.begin() + static_cast<std::ptrdiff_t>(completed),
                        chunk_size, std::byte{0});
        } else {
            const auto physical_offset = data_offset_ +
                static_cast<std::uint64_t>(mapping) * block_size_ + block_offset;
            read_backing(physical_offset,
                         destination.subspan(completed, chunk_size));
        }
        completed += chunk_size;
    }
    return count;
}

std::uint64_t VdiImageDriver::get_size() const noexcept {
    return virtual_size_;
}

std::uint32_t VdiImageDriver::sector_size() const noexcept {
    return static_cast<std::uint32_t>(sector_size_bytes);
}

std::filesystem::path VdiImageDriver::source_path() const {
    return source_path_;
}

std::string_view VdiImageDriver::format_name() const noexcept {
    return "VDI";
}

void VdiImageDriver::close() noexcept {
    std::scoped_lock lock(mutex_);
    if (input_.is_open()) {
        input_.close();
    }
    source_path_.clear();
    backing_size_ = 0;
    virtual_size_ = 0;
    data_offset_ = 0;
    block_size_ = 0;
    block_map_.clear();
}

void VdiImageDriver::read_backing(
    std::uint64_t offset, std::span<std::byte> destination) const {
    if (offset > backing_size_ || destination.size() > backing_size_ - offset ||
        offset > static_cast<std::uint64_t>(
            std::numeric_limits<std::streamoff>::max())) {
        throw std::out_of_range("VDI 后备文件读取范围无效");
    }
    input_.clear();
    input_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input_) {
        throw std::runtime_error("定位 VDI 后备文件失败");
    }
    input_.read(reinterpret_cast<char*>(destination.data()),
                static_cast<std::streamsize>(destination.size()));
    if (input_.gcount() != static_cast<std::streamsize>(destination.size())) {
        throw std::runtime_error("读取 VDI 后备文件时遇到意外 EOF");
    }
}

} // namespace uife
