#include "uife/drivers/image/QedImageDriver.h"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace uife {
namespace {

constexpr std::uint32_t qed_magic = 0x00444551U;
constexpr std::uint64_t sector_size_bytes = 512;
constexpr std::uint64_t qed_feature_backing_file = 0x1U;
constexpr std::uint64_t qed_feature_need_check = 0x2U;
constexpr std::uint64_t qed_feature_backing_format_no_probe = 0x4U;
constexpr std::uint64_t known_features = qed_feature_backing_file |
    qed_feature_need_check | qed_feature_backing_format_no_probe;
constexpr std::uint64_t max_table_bytes = 64U * 1024U * 1024U;

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

QedImageDriver::~QedImageDriver() {
    close();
}

void QedImageDriver::open(const std::filesystem::path& image_path) {
    close();
    std::error_code error;
    const auto file_size = std::filesystem::file_size(image_path, error);
    if (error || file_size < 64 ||
        file_size > static_cast<std::uint64_t>(
            std::numeric_limits<std::streamoff>::max())) {
        throw std::runtime_error("QED 文件大小无效或无法读取");
    }
    std::ifstream candidate(image_path, std::ios::binary);
    if (!candidate) {
        throw std::runtime_error("无法以只读方式打开 QED 镜像");
    }

    std::array<std::byte, 64> header{};
    candidate.read(reinterpret_cast<char*>(header.data()),
                   static_cast<std::streamsize>(header.size()));
    if (candidate.gcount() != static_cast<std::streamsize>(header.size()) ||
        decode_u32(header, 0) != qed_magic) {
        throw std::runtime_error("QED header magic 无效");
    }
    const auto cluster_size = decode_u32(header, 4);
    const auto table_size_clusters = decode_u32(header, 8);
    const auto header_size_clusters = decode_u32(header, 12);
    const auto features = decode_u64(header, 16);
    const auto compatible_features = decode_u64(header, 24);
    const auto autoclear_features = decode_u64(header, 32);
    const auto l1_table_offset = decode_u64(header, 40);
    const auto image_size = decode_u64(header, 48);
    const auto backing_filename_offset = decode_u32(header, 56);
    const auto backing_filename_size = decode_u32(header, 60);

    if (cluster_size < 4096 || cluster_size > 64U * 1024U * 1024U ||
        (cluster_size & (cluster_size - 1U)) != 0 ||
        table_size_clusters == 0 || table_size_clusters > 16 ||
        (table_size_clusters & (table_size_clusters - 1U)) != 0 ||
        header_size_clusters == 0 || image_size == 0 ||
        image_size % sector_size_bytes != 0) {
        throw std::runtime_error("QED header 几何参数无效或超出支持范围");
    }
    if ((features & ~known_features) != 0 ||
        (features & qed_feature_need_check) != 0 ||
        (features & qed_feature_backing_file) != 0 ||
        (features & qed_feature_backing_format_no_probe) != 0 ||
        compatible_features != 0 || autoclear_features != 0) {
        throw std::runtime_error(
            "当前 QED 驱动不支持 backing file、need-check 或未知 feature");
    }
    if (backing_filename_offset != 0 || backing_filename_size != 0) {
        throw std::runtime_error("当前 QED 驱动不支持 backing filename");
    }

    const auto table_bytes =
        static_cast<std::uint64_t>(cluster_size) * table_size_clusters;
    if (table_bytes > max_table_bytes || table_bytes % sizeof(std::uint64_t) != 0) {
        throw std::runtime_error("QED L1/L2 table 大小超出安全解析上限");
    }
    const auto entries_per_table = table_bytes / sizeof(std::uint64_t);
    const auto cluster_group_capacity =
        static_cast<std::uint64_t>(cluster_size) * entries_per_table;
    if (entries_per_table == 0 ||
        image_size / cluster_group_capacity +
                (image_size % cluster_group_capacity != 0 ? 1U : 0U) >
            entries_per_table) {
        throw std::runtime_error("QED L1 table 无法覆盖声明的虚拟容量");
    }
    if (header_size_clusters >
        std::numeric_limits<std::uint64_t>::max() / cluster_size) {
        throw std::runtime_error("QED header 区域大小溢出");
    }
    const auto header_region_size =
        static_cast<std::uint64_t>(header_size_clusters) * cluster_size;
    if (l1_table_offset < header_region_size ||
        l1_table_offset % cluster_size != 0 ||
        l1_table_offset > file_size || table_bytes > file_size - l1_table_offset ||
        l1_table_offset > static_cast<std::uint64_t>(
            std::numeric_limits<std::streamoff>::max())) {
        throw std::runtime_error("QED L1 table 超出文件边界或与 header 重叠");
    }

    std::vector<std::byte> encoded_l1(static_cast<std::size_t>(table_bytes));
    candidate.clear();
    candidate.seekg(static_cast<std::streamoff>(l1_table_offset), std::ios::beg);
    if (!candidate) {
        throw std::runtime_error("无法定位 QED L1 table");
    }
    candidate.read(reinterpret_cast<char*>(encoded_l1.data()),
                   static_cast<std::streamsize>(encoded_l1.size()));
    if (candidate.gcount() != static_cast<std::streamsize>(encoded_l1.size())) {
        throw std::runtime_error("读取 QED L1 table 失败");
    }

    std::vector<std::uint64_t> l1_table(static_cast<std::size_t>(entries_per_table));
    for (std::size_t index = 0; index < l1_table.size(); ++index) {
        const auto table_offset = decode_u64(encoded_l1, index * sizeof(std::uint64_t));
        if (table_offset == 0) {
            continue;
        }
        const bool overlaps_l1 =
            table_offset < l1_table_offset + table_bytes &&
            l1_table_offset < table_offset + table_bytes;
        if (table_offset < header_region_size ||
            table_offset % cluster_size != 0 || table_offset > file_size ||
            table_bytes > file_size - table_offset || overlaps_l1) {
            throw std::runtime_error("QED L1 entry 指向越界或未对齐的 L2 table");
        }
        l1_table[index] = table_offset;
    }

    input_ = std::move(candidate);
    source_path_ = image_path;
    backing_size_ = file_size;
    virtual_size_ = image_size;
    entries_per_table_ = entries_per_table;
    cluster_size_ = cluster_size;
    l1_table_ = std::move(l1_table);
}

std::size_t QedImageDriver::read_sector(
    std::uint64_t sector_number, std::span<std::byte> destination) const {
    if (destination.size() != sector_size()) {
        throw std::invalid_argument("QED read_sector 要求恰好一个 512 字节扇区");
    }
    if (sector_number > std::numeric_limits<std::uint64_t>::max() / sector_size()) {
        throw std::out_of_range("QED 扇区号溢出");
    }
    return read_at(sector_number * sector_size(), destination);
}

std::size_t QedImageDriver::read_at(
    std::uint64_t offset, std::span<std::byte> destination) const {
    std::scoped_lock lock(mutex_);
    if (!input_.is_open()) {
        throw std::runtime_error("QED 镜像尚未打开");
    }
    if (offset > virtual_size_) {
        throw std::out_of_range("读取偏移超出 QED 虚拟容量");
    }
    const auto count = static_cast<std::size_t>(
        std::min<std::uint64_t>(virtual_size_ - offset, destination.size()));
    std::size_t completed = 0;
    while (completed < count) {
        const auto virtual_offset = offset + completed;
        const auto cluster_index = virtual_offset / cluster_size_;
        const auto cluster_offset = virtual_offset % cluster_size_;
        const auto chunk_size = static_cast<std::size_t>(
            std::min<std::uint64_t>(cluster_size_ - cluster_offset,
                                    count - completed));
        const auto l1_index = cluster_index / entries_per_table_;
        const auto l2_index = cluster_index % entries_per_table_;
        if (l1_index >= l1_table_.size()) {
            throw std::runtime_error("QED L1 index 超出 table 范围");
        }
        const auto l2_table_offset = l1_table_[static_cast<std::size_t>(l1_index)];
        if (l2_table_offset == 0) {
            std::fill_n(destination.begin() + static_cast<std::ptrdiff_t>(completed),
                        chunk_size, std::byte{0});
            completed += chunk_size;
            continue;
        }
        const auto entry_offset =
            l2_table_offset + l2_index * sizeof(std::uint64_t);
        const auto data_offset = read_u64_at(entry_offset);
        if (data_offset == 0 || data_offset == 1) {
            std::fill_n(destination.begin() + static_cast<std::ptrdiff_t>(completed),
                        chunk_size, std::byte{0});
        } else {
            if (data_offset % cluster_size_ != 0 ||
                data_offset > backing_size_ ||
                cluster_size_ > backing_size_ - data_offset) {
                throw std::runtime_error("QED L2 entry 指向越界或未对齐的数据 cluster");
            }
            read_backing(data_offset + cluster_offset,
                         destination.subspan(completed, chunk_size));
        }
        completed += chunk_size;
    }
    return count;
}

std::uint64_t QedImageDriver::get_size() const noexcept {
    return virtual_size_;
}

std::uint32_t QedImageDriver::sector_size() const noexcept {
    return static_cast<std::uint32_t>(sector_size_bytes);
}

std::filesystem::path QedImageDriver::source_path() const {
    return source_path_;
}

std::string_view QedImageDriver::format_name() const noexcept {
    return "QED";
}

void QedImageDriver::close() noexcept {
    std::scoped_lock lock(mutex_);
    if (input_.is_open()) {
        input_.close();
    }
    source_path_.clear();
    backing_size_ = 0;
    virtual_size_ = 0;
    entries_per_table_ = 0;
    cluster_size_ = 0;
    l1_table_.clear();
}

std::uint64_t QedImageDriver::read_u64_at(std::uint64_t offset) const {
    std::array<std::byte, sizeof(std::uint64_t)> encoded{};
    read_backing(offset, encoded);
    return decode_u64(encoded, 0);
}

void QedImageDriver::read_backing(
    std::uint64_t offset, std::span<std::byte> destination) const {
    if (offset > backing_size_ || destination.size() > backing_size_ - offset ||
        offset > static_cast<std::uint64_t>(
            std::numeric_limits<std::streamoff>::max())) {
        throw std::out_of_range("QED 后备文件读取范围无效");
    }
    input_.clear();
    input_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input_) {
        throw std::runtime_error("定位 QED 后备文件失败");
    }
    input_.read(reinterpret_cast<char*>(destination.data()),
                static_cast<std::streamsize>(destination.size()));
    if (input_.gcount() != static_cast<std::streamsize>(destination.size())) {
        throw std::runtime_error("读取 QED 后备文件时遇到意外 EOF");
    }
}

} // namespace uife
