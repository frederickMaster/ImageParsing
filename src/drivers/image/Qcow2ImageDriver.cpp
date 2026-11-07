#include "uife/drivers/image/Qcow2ImageDriver.h"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

#ifdef UIFE_WITH_ZLIB
#include <zlib.h>
#endif

namespace uife {
namespace {

constexpr std::uint32_t qcow2_magic = 0x514649fbU;
constexpr std::uint64_t sector_size_bytes = 512;
constexpr std::uint64_t copied_flag = 1ULL << 63U;
constexpr std::uint64_t compressed_flag = 1ULL << 62U;
constexpr std::uint64_t zero_flag = 1ULL;
constexpr std::uint64_t offset_mask = 0x00fffffffffffe00ULL;
constexpr std::uint64_t maximum_table_bytes = 64U * 1024U * 1024U;
constexpr std::uint32_t minimum_cluster_bits = 9;
constexpr std::uint32_t maximum_cluster_bits = 21;

std::uint32_t decode_be32(std::span<const std::byte> data, std::size_t offset) {
    return (static_cast<std::uint32_t>(
                std::to_integer<unsigned char>(data[offset])) << 24U) |
           (static_cast<std::uint32_t>(
                std::to_integer<unsigned char>(data[offset + 1])) << 16U) |
           (static_cast<std::uint32_t>(
                std::to_integer<unsigned char>(data[offset + 2])) << 8U) |
           static_cast<std::uint32_t>(
               std::to_integer<unsigned char>(data[offset + 3]));
}

std::uint64_t decode_be64(std::span<const std::byte> data, std::size_t offset) {
    return (static_cast<std::uint64_t>(decode_be32(data, offset)) << 32U) |
           decode_be32(data, offset + 4);
}

std::uint64_t round_up_to_cluster(std::uint64_t size,
                                  std::uint64_t cluster_size) {
    const auto remainder = size % cluster_size;
    if (remainder == 0) {
        return size;
    }
    const auto padding = cluster_size - remainder;
    if (size > std::numeric_limits<std::uint64_t>::max() - padding) {
        throw std::runtime_error("QCOW2 表大小溢出");
    }
    return size + padding;
}

bool overlaps(std::uint64_t first_start, std::uint64_t first_size,
              std::uint64_t second_start, std::uint64_t second_size) {
    return first_start < second_start + second_size &&
           second_start < first_start + first_size;
}

} // namespace

Qcow2ImageDriver::~Qcow2ImageDriver() {
    close();
}

void Qcow2ImageDriver::open(const std::filesystem::path& image_path) {
    close();
    std::error_code error;
    const auto file_size = std::filesystem::file_size(image_path, error);
    if (error || file_size < 72 ||
        file_size > static_cast<std::uint64_t>(
            std::numeric_limits<std::streamoff>::max())) {
        throw std::runtime_error("QCOW2 文件大小无效或无法读取");
    }
    std::ifstream candidate(image_path, std::ios::binary);
    if (!candidate) {
        throw std::runtime_error("无法以只读方式打开 QCOW2 镜像");
    }
    std::array<std::byte, 104> header{};
    candidate.read(reinterpret_cast<char*>(header.data()),
                   static_cast<std::streamsize>(header.size()));
    const auto header_read = static_cast<std::size_t>(candidate.gcount());
    if (header_read < 72 || decode_be32(header, 0) != qcow2_magic) {
        throw std::runtime_error("QCOW2 header magic 无效");
    }

    const auto version = decode_be32(header, 4);
    const auto backing_file_offset = decode_be64(header, 8);
    const auto backing_file_size = decode_be32(header, 16);
    const auto cluster_bits = decode_be32(header, 20);
    const auto image_size = decode_be64(header, 24);
    const auto crypt_method = decode_be32(header, 32);
    const auto l1_size = decode_be32(header, 36);
    const auto l1_table_offset = decode_be64(header, 40);
    const auto refcount_table_offset = decode_be64(header, 48);
    const auto refcount_table_clusters = decode_be32(header, 56);
    const auto snapshot_count = decode_be32(header, 60);
    const auto snapshot_offset = decode_be64(header, 64);
    if ((version != 2 && version != 3) ||
        cluster_bits < minimum_cluster_bits ||
        cluster_bits > maximum_cluster_bits ||
        image_size == 0 || image_size % sector_size_bytes != 0 ||
        crypt_method != 0 || backing_file_offset != 0 ||
        backing_file_size != 0 || snapshot_count != 0 || snapshot_offset != 0) {
        throw std::runtime_error(
            "当前 QCOW2 驱动只支持无加密、无 backing file/snapshot 的 v2/v3 镜像");
    }
    if (version == 3) {
        if (header_read < header.size()) {
            throw std::runtime_error("QCOW2 v3 扩展 header 不完整");
        }
        const auto incompatible_features = decode_be64(header, 72);
        const auto compatible_features = decode_be64(header, 80);
        const auto autoclear_features = decode_be64(header, 88);
        const auto refcount_order = decode_be32(header, 96);
        const auto header_length = decode_be32(header, 100);
        if (incompatible_features != 0 || compatible_features != 0 ||
            autoclear_features != 0 || refcount_order != 4 ||
            header_length < 104 || header_length > file_size) {
            throw std::runtime_error(
                "当前 QCOW2 驱动不支持 v3 feature flags 或非标准 refcount");
        }
    }

    const auto cluster_size = 1ULL << cluster_bits;
    const auto entries_per_l2 = cluster_size / sizeof(std::uint64_t);
    const auto l2_coverage = cluster_size * entries_per_l2;
    const auto required_l1_entries = image_size / l2_coverage +
        (image_size % l2_coverage != 0 ? 1U : 0U);
    const auto l1_table_bytes =
        static_cast<std::uint64_t>(l1_size) * sizeof(std::uint64_t);
    const auto refcount_table_bytes =
        static_cast<std::uint64_t>(refcount_table_clusters) * cluster_size;
    if (l1_size == 0 || l1_size < required_l1_entries ||
        l1_table_bytes > maximum_table_bytes ||
        l1_table_offset % cluster_size != 0 ||
        l1_table_offset < cluster_size || refcount_table_clusters == 0 ||
        refcount_table_bytes > maximum_table_bytes ||
        refcount_table_offset % cluster_size != 0 ||
        refcount_table_offset < cluster_size) {
        throw std::runtime_error("QCOW2 L1/refcount table 几何参数无效");
    }
    const auto l1_table_storage_bytes =
        round_up_to_cluster(l1_table_bytes, cluster_size);
    if (l1_table_offset > file_size ||
        l1_table_storage_bytes > file_size - l1_table_offset) {
        throw std::runtime_error("QCOW2 L1 table 超出文件边界");
    }
    if (refcount_table_offset > file_size ||
        refcount_table_bytes > file_size - refcount_table_offset ||
        overlaps(refcount_table_offset, refcount_table_bytes, 0, cluster_size) ||
        overlaps(refcount_table_offset, refcount_table_bytes, l1_table_offset,
                 l1_table_storage_bytes)) {
        throw std::runtime_error("QCOW2 refcount table 超出文件边界或与其他表重叠");
    }

    std::vector<std::byte> encoded_l1(static_cast<std::size_t>(l1_table_bytes));
    candidate.clear();
    candidate.seekg(static_cast<std::streamoff>(l1_table_offset), std::ios::beg);
    if (!candidate) {
        throw std::runtime_error("无法定位 QCOW2 L1 table");
    }
    candidate.read(reinterpret_cast<char*>(encoded_l1.data()),
                   static_cast<std::streamsize>(encoded_l1.size()));
    if (candidate.gcount() != static_cast<std::streamsize>(encoded_l1.size())) {
        throw std::runtime_error("读取 QCOW2 L1 table 失败");
    }

    std::vector<std::uint64_t> l1_table(l1_size);
    std::vector<std::uint64_t> l2_table_offsets;
    const auto allowed_l1_bits = copied_flag | offset_mask;
    for (std::size_t index = 0; index < l1_table.size(); ++index) {
        const auto entry = decode_be64(encoded_l1, index * sizeof(std::uint64_t));
        if ((entry & ~allowed_l1_bits) != 0) {
            throw std::runtime_error("QCOW2 L1 entry 含有压缩或未知 flag");
        }
        const auto table_offset = entry & offset_mask;
        if (table_offset == 0) {
            continue;
        }
        if (table_offset % cluster_size != 0 ||
            table_offset > file_size ||
            cluster_size > file_size - table_offset ||
            overlaps(table_offset, cluster_size, 0, cluster_size) ||
            overlaps(table_offset, cluster_size, l1_table_offset,
                     l1_table_storage_bytes) ||
            overlaps(table_offset, cluster_size, refcount_table_offset,
                     refcount_table_bytes)) {
            throw std::runtime_error("QCOW2 L1 entry 指向越界或重叠的 L2 table");
        }
        l1_table[index] = table_offset;
        l2_table_offsets.push_back(table_offset);
    }
    std::sort(l2_table_offsets.begin(), l2_table_offsets.end());

    input_ = std::move(candidate);
    source_path_ = image_path;
    backing_size_ = file_size;
    virtual_size_ = image_size;
    l1_table_storage_bytes_ = l1_table_storage_bytes;
    l1_table_offset_ = l1_table_offset;
    refcount_table_offset_ = refcount_table_offset;
    refcount_table_bytes_ = refcount_table_bytes;
    entries_per_l2_ = entries_per_l2;
    cluster_size_ = cluster_size;
    version_ = version;
    cluster_bits_ = static_cast<std::uint32_t>(cluster_bits);
    l1_table_ = std::move(l1_table);
    l2_table_offsets_ = std::move(l2_table_offsets);
    cached_cluster_index_ = std::numeric_limits<std::uint64_t>::max();
    cached_cluster_data_.clear();
}

std::size_t Qcow2ImageDriver::read_sector(
    std::uint64_t sector_number, std::span<std::byte> destination) const {
    if (destination.size() != sector_size()) {
        throw std::invalid_argument(
            "QCOW2 read_sector 要求恰好一个 512 字节扇区");
    }
    if (sector_number > std::numeric_limits<std::uint64_t>::max() /
                            sector_size()) {
        throw std::out_of_range("QCOW2 扇区号溢出");
    }
    return read_at(sector_number * sector_size(), destination);
}

std::size_t Qcow2ImageDriver::read_at(
    std::uint64_t offset, std::span<std::byte> destination) const {
    std::scoped_lock lock(mutex_);
    if (!input_.is_open()) {
        throw std::runtime_error("QCOW2 镜像尚未打开");
    }
    if (offset > virtual_size_) {
        throw std::out_of_range("读取偏移超出 QCOW2 虚拟容量");
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
        const auto l1_index = cluster_index / entries_per_l2_;
        const auto l2_index = cluster_index % entries_per_l2_;
        if (l1_index >= l1_table_.size()) {
            throw std::runtime_error("QCOW2 L1 index 超出 table 范围");
        }
        const auto l2_table_offset = l1_table_[static_cast<std::size_t>(l1_index)];
        if (l2_table_offset == 0) {
            std::fill_n(destination.begin() + static_cast<std::ptrdiff_t>(completed),
                        chunk_size, std::byte{0});
            completed += chunk_size;
            continue;
        }

        const auto l2_entry_offset =
            l2_table_offset + l2_index * sizeof(std::uint64_t);
        const auto entry = read_u64_at(l2_entry_offset);
        const bool entry_compressed = (entry & compressed_flag) != 0;
        // 标准 cluster 描述符只允许 copied/offset/zero flag；压缩 cluster
        // 描述符合法位域是 bit 0–61（host_offset + additional_sectors），
        // 其中 additional_sectors 可延伸到 offset_mask 之外（bit 56–61），
        // bit 63（COPIED）对压缩 cluster 必须为 0。
        const auto allowed_l2_bits =
            entry_compressed
                ? (std::numeric_limits<std::uint64_t>::max() >> 1U)
                : copied_flag | offset_mask |
                      (version_ == 3 ? zero_flag : 0U);
        if ((entry & ~allowed_l2_bits) != 0) {
            throw std::runtime_error("QCOW2 L2 entry 含有未知 flag");
        }
        if (entry_compressed) {
#ifdef UIFE_WITH_ZLIB
            // 注意：压缩描述符的 bit 0 属于 host_offset（字节粒度），
            // 不能再用 v3 标准 cluster 的 zero_flag 语义去解读它。
            // 压缩 cluster 按整个 cluster 解压（结果缓存），再拷贝请求的子区间。
            std::vector<std::byte> cluster(cluster_size_);
            read_compressed_cluster(cluster_index, entry, cluster);
            std::copy_n(cluster.begin() + static_cast<std::ptrdiff_t>(cluster_offset),
                        chunk_size,
                        destination.begin() +
                            static_cast<std::ptrdiff_t>(completed));
#else
            throw std::runtime_error(
                "当前构建未启用 zlib，无法读取压缩 QCOW2 cluster");
#endif
        } else {
            const auto data_offset = entry & offset_mask;
            if ((version_ == 3 && (entry & zero_flag) != 0) ||
                data_offset == 0) {
                std::fill_n(
                    destination.begin() +
                        static_cast<std::ptrdiff_t>(completed),
                    chunk_size, std::byte{0});
            } else {
                if (data_offset % cluster_size_ != 0 ||
                    data_offset > backing_size_ ||
                    cluster_size_ > backing_size_ - data_offset ||
                    overlaps(data_offset, cluster_size_, 0, cluster_size_) ||
                    overlaps(data_offset, cluster_size_, l1_table_offset_,
                             l1_table_storage_bytes_) ||
                    overlaps(data_offset, cluster_size_, refcount_table_offset_,
                             refcount_table_bytes_) ||
                    overlaps(data_offset, cluster_size_, l2_table_offset,
                             cluster_size_) ||
                    std::binary_search(l2_table_offsets_.begin(),
                                       l2_table_offsets_.end(), data_offset)) {
                    throw std::runtime_error(
                        "QCOW2 L2 entry 指向越界或重叠的数据 cluster");
                }
                read_backing(data_offset + cluster_offset,
                             destination.subspan(completed, chunk_size));
            }
        }
        completed += chunk_size;
    }
    return count;
}

std::uint64_t Qcow2ImageDriver::get_size() const noexcept {
    return virtual_size_;
}

std::uint32_t Qcow2ImageDriver::sector_size() const noexcept {
    return static_cast<std::uint32_t>(sector_size_bytes);
}

std::filesystem::path Qcow2ImageDriver::source_path() const {
    return source_path_;
}

std::string_view Qcow2ImageDriver::format_name() const noexcept {
    return "QCOW2";
}

void Qcow2ImageDriver::close() noexcept {
    std::scoped_lock lock(mutex_);
    if (input_.is_open()) {
        input_.close();
    }
    source_path_.clear();
    backing_size_ = 0;
    virtual_size_ = 0;
    l1_table_storage_bytes_ = 0;
    l1_table_offset_ = 0;
    refcount_table_offset_ = 0;
    refcount_table_bytes_ = 0;
    entries_per_l2_ = 0;
    cluster_size_ = 0;
    version_ = 0;
    cluster_bits_ = 0;
    l1_table_.clear();
    l2_table_offsets_.clear();
    cached_cluster_index_ = std::numeric_limits<std::uint64_t>::max();
    cached_cluster_data_.clear();
}

// 压缩 cluster 描述符（QCOW2 spec "Compressed Clusters Descriptor"）：
//   x = 62 - (cluster_bits - 8)
//   host_offset       = entry & ((1 << x) - 1)          （非 cluster 对齐）
//   additional_sectors = (entry >> x) & ((1 << (62 - x)) - 1)
//   压缩数据长度 = (additional_sectors + 1) * 512 字节，raw deflate。
void Qcow2ImageDriver::read_compressed_cluster(
    std::uint64_t cluster_index, std::uint64_t l2_entry,
    std::span<std::byte> destination) const {
#ifdef UIFE_WITH_ZLIB
    if (cached_cluster_index_ == cluster_index &&
        cached_cluster_data_.size() == cluster_size_) {
        std::copy(cached_cluster_data_.begin(), cached_cluster_data_.end(),
                  destination.begin());
        return;
    }
    const auto x = 62U - static_cast<std::uint32_t>(cluster_bits_ - 8U);
    const auto host_offset =
        l2_entry & ((1ULL << x) - 1U);
    const auto additional_sectors =
        (l2_entry >> x) & ((1ULL << (62U - x)) - 1U);
    const auto compressed_length = (additional_sectors + 1U) * sector_size_bytes;
    if (host_offset < cluster_size_ || host_offset > backing_size_ ||
        compressed_length > backing_size_ - host_offset) {
        throw std::runtime_error("QCOW2 压缩 cluster 指向越界的数据");
    }
    std::vector<std::byte> encoded(static_cast<std::size_t>(compressed_length));
    read_backing(host_offset, encoded);

    std::vector<std::byte> decoded(static_cast<std::size_t>(cluster_size_));
    z_stream stream{};
    if (inflateInit2(&stream, -15) != Z_OK) {
        throw std::runtime_error("QCOW2 压缩 cluster 初始化解压失败");
    }
    stream.next_in = reinterpret_cast<Bytef*>(encoded.data());
    stream.avail_in = static_cast<uInt>(encoded.size());
    stream.next_out = reinterpret_cast<Bytef*>(decoded.data());
    stream.avail_out = static_cast<uInt>(decoded.size());
    const auto result = inflate(&stream, Z_FINISH);
    const auto produced = decoded.size() - stream.avail_out;
    inflateEnd(&stream);
    if (result != Z_STREAM_END || produced != decoded.size()) {
        throw std::runtime_error(
            "QCOW2 压缩 cluster 解压失败或解压后不足一个 cluster");
    }
    cached_cluster_index_ = cluster_index;
    cached_cluster_data_ = decoded;
    std::copy(decoded.begin(), decoded.end(), destination.begin());
#else
    (void)cluster_index;
    (void)l2_entry;
    throw std::runtime_error("当前构建未启用 zlib，无法读取压缩 QCOW2 cluster");
#endif
}

std::uint64_t Qcow2ImageDriver::read_u64_at(std::uint64_t offset) const {
    std::array<std::byte, sizeof(std::uint64_t)> encoded{};
    read_backing(offset, encoded);
    return decode_be64(encoded, 0);
}

void Qcow2ImageDriver::read_backing(
    std::uint64_t offset, std::span<std::byte> destination) const {
    if (offset > backing_size_ || destination.size() > backing_size_ - offset ||
        offset > static_cast<std::uint64_t>(
            std::numeric_limits<std::streamoff>::max())) {
        throw std::out_of_range("QCOW2 后备文件读取范围无效");
    }
    input_.clear();
    input_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input_) {
        throw std::runtime_error("定位 QCOW2 后备文件失败");
    }
    input_.read(reinterpret_cast<char*>(destination.data()),
                static_cast<std::streamsize>(destination.size()));
    if (input_.gcount() != static_cast<std::streamsize>(destination.size())) {
        throw std::runtime_error("读取 QCOW2 后备文件时遇到意外 EOF");
    }
}

} // namespace uife
