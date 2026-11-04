#include "uife/drivers/image/RawImageDriver.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace uife {

RawImageDriver::~RawImageDriver() {
    close();
}

void RawImageDriver::open(const std::filesystem::path& image_path) {
    close();
    std::error_code error;
    const auto file_size = std::filesystem::file_size(image_path, error);
    if (error) {
        throw std::runtime_error("无法读取镜像大小: " + image_path.string());
    }

    std::ifstream candidate(image_path, std::ios::binary);
    if (!candidate) {
        throw std::runtime_error("无法以只读方式打开镜像: " + image_path.string());
    }
    input_ = std::move(candidate);
    source_path_ = image_path;
    size_ = file_size;
}

std::size_t RawImageDriver::read_sector(std::uint64_t sector_number,
                                        std::span<std::byte> destination) const {
    if (destination.size() != sector_size()) {
        throw std::invalid_argument("RAW read_sector 要求恰好一个 512 字节扇区");
    }
    if (sector_number > std::numeric_limits<std::uint64_t>::max() / sector_size()) {
        throw std::out_of_range("扇区号溢出");
    }
    return read_at(sector_number * sector_size(), destination);
}

std::size_t RawImageDriver::read_at(std::uint64_t offset,
                                    std::span<std::byte> destination) const {
    std::scoped_lock lock(mutex_);
    if (!input_.is_open()) {
        throw std::runtime_error("RAW 镜像尚未打开");
    }
    if (offset > size_) {
        throw std::out_of_range("读取偏移超出镜像大小");
    }
    const auto available = size_ - offset;
    const auto count = static_cast<std::size_t>(
        std::min<std::uint64_t>(available, destination.size()));
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        throw std::out_of_range("读取偏移超出当前平台文件流范围");
    }
    input_.clear();
    input_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input_) {
        throw std::runtime_error("定位 RAW 镜像失败");
    }
    input_.read(reinterpret_cast<char*>(destination.data()),
                static_cast<std::streamsize>(count));
    if (input_.gcount() != static_cast<std::streamsize>(count)) {
        throw std::runtime_error("读取 RAW 镜像时遇到意外 EOF");
    }
    return count;
}

std::uint64_t RawImageDriver::get_size() const noexcept {
    return size_;
}

std::uint32_t RawImageDriver::sector_size() const noexcept {
    return 512;
}

std::filesystem::path RawImageDriver::source_path() const {
    return source_path_;
}

std::string_view RawImageDriver::format_name() const noexcept {
    return "RAW";
}

void RawImageDriver::close() noexcept {
    std::scoped_lock lock(mutex_);
    if (input_.is_open()) {
        input_.close();
    }
    source_path_.clear();
    size_ = 0;
}

} // namespace uife
