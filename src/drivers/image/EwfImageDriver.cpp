#include "uife/drivers/image/EwfImageDriver.h"

#ifdef UIFE_WITH_LIBEWF
#ifdef _WIN32
#include <windows.h>
#endif
#include <libewf.h>
#endif

#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace uife {

#ifdef UIFE_WITH_LIBEWF
namespace {

std::string ewf_error_message(libewf_error_t* error) {
    if (error == nullptr) {
        return "libewf 未提供错误详情";
    }
    std::array<char, 1024> message{};
    const auto length = libewf_error_backtrace_sprint(
        error, message.data(), message.size());
    libewf_error_free(&error);
    if (length <= 0) {
        return "libewf 未提供错误详情";
    }
    return std::string(message.data(), static_cast<std::size_t>(
        std::min<int>(length, static_cast<int>(message.size() - 1))));
}

void ensure_utf8_codepage() {
    static std::once_flag codepage_once;
    std::call_once(codepage_once, [] {
        libewf_error_t* error = nullptr;
        if (libewf_set_codepage(0, &error) != 1) {
            throw std::runtime_error(
                "设置 libewf UTF-8 路径编码失败: " +
                ewf_error_message(error));
        }
        if (error != nullptr) {
            libewf_error_free(&error);
        }
    });
}

#if !defined(_WIN32) || !defined(LIBEWF_HAVE_WIDE_CHARACTER_TYPE)
std::string utf8_path_string(const std::filesystem::path& path) {
    const auto encoded = path.u8string();
    return std::string(reinterpret_cast<const char*>(encoded.data()),
                       encoded.size());
}
#endif

#if defined(_WIN32) && defined(LIBEWF_HAVE_WIDE_CHARACTER_TYPE)
// libewf 内嵌的 libcpath 在展开 Windows 绝对路径时存在缺陷：会把当前目录
// 误拼接到绝对路径前（相对路径则完全正常）。因此在打开期间把进程当前
// 目录临时切换到镜像所在目录，以纯文件名调用 libewf，打开完成后立即恢复。
class scoped_directory_change {
public:
    explicit scoped_directory_change(const std::filesystem::path& target) {
        if (target.empty()) {
            return;
        }
        const auto required = GetCurrentDirectoryW(0, nullptr);
        if (required == 0) {
            return;
        }
        previous_.resize(static_cast<std::size_t>(required));
        if (GetCurrentDirectoryW(required, previous_.data()) == 0) {
            previous_.clear();
            return;
        }
        if (SetCurrentDirectoryW(target.c_str()) == 0) {
            previous_.clear();
            return;
        }
        active_ = true;
    }

    scoped_directory_change(const scoped_directory_change&) = delete;
    scoped_directory_change& operator=(const scoped_directory_change&) = delete;

    ~scoped_directory_change() {
        if (active_) {
            (void)SetCurrentDirectoryW(previous_.c_str());
        }
    }

    bool active() const noexcept {
        return active_;
    }

private:
    std::wstring previous_;
    bool active_ = false;
};
#endif

} // namespace
#endif

struct EwfImageDriver::State {
#ifdef UIFE_WITH_LIBEWF
    libewf_handle_t* handle{};
#if defined(_WIN32) && defined(LIBEWF_HAVE_WIDE_CHARACTER_TYPE)
    wchar_t** filenames{};
#else
    char** filenames{};
#endif
    int number_of_filenames{};
    mutable std::mutex mutex;

    ~State() {
        if (handle != nullptr) {
            libewf_error_t* error = nullptr;
            (void)libewf_handle_close(handle, &error);
            if (error != nullptr) {
                libewf_error_free(&error);
            }
            error = nullptr;
            (void)libewf_handle_free(&handle, &error);
            if (error != nullptr) {
                libewf_error_free(&error);
            }
        }
        if (filenames != nullptr) {
            libewf_error_t* error = nullptr;
#if defined(_WIN32) && defined(LIBEWF_HAVE_WIDE_CHARACTER_TYPE)
            (void)libewf_glob_wide_free(
                filenames, number_of_filenames, &error);
#else
            (void)libewf_glob_free(
                filenames, number_of_filenames, &error);
#endif
            if (error != nullptr) {
                libewf_error_free(&error);
            }
        }
    }
#endif
};

EwfImageDriver::EwfImageDriver() = default;
EwfImageDriver::~EwfImageDriver() = default;

void EwfImageDriver::open(const std::filesystem::path& image_path) {
    close();
#ifdef UIFE_WITH_LIBEWF
    auto candidate = std::make_unique<State>();
    std::optional<scoped_directory_change> directory_guard;
#if defined(_WIN32) && defined(LIBEWF_HAVE_WIDE_CHARACTER_TYPE)
    auto effective_path = image_path;
    if (effective_path.is_absolute() && effective_path.has_filename()) {
        // 仅当目录切换成功时才改用文件名，否则保留原路径让 libewf 报错
        directory_guard.emplace(effective_path.parent_path());
        if (directory_guard->active()) {
            effective_path = effective_path.filename();
        }
    }
    const auto native_path = effective_path.native();
    if (native_path.empty()) {
        throw std::invalid_argument("EWF 镜像路径不能为空");
    }
#else
    const auto utf8_path = utf8_path_string(image_path);
    if (utf8_path.empty()) {
        throw std::invalid_argument("EWF 镜像路径不能为空");
    }
#endif

    ensure_utf8_codepage();
    libewf_error_t* error = nullptr;

#if defined(_WIN32) && defined(LIBEWF_HAVE_WIDE_CHARACTER_TYPE)
    if (libewf_glob_wide(native_path.c_str(), native_path.size(),
                         LIBEWF_FORMAT_UNKNOWN, &candidate->filenames,
                         &candidate->number_of_filenames, &error) != 1) {
#else
    if (libewf_glob(utf8_path.c_str(), utf8_path.size(),
                    LIBEWF_FORMAT_UNKNOWN, &candidate->filenames,
                    &candidate->number_of_filenames, &error) != 1) {
#endif
        throw std::runtime_error(
            "无法枚举 EWF 关联分卷: " + ewf_error_message(error));
    }
    if (candidate->filenames == nullptr ||
        candidate->number_of_filenames <= 0) {
        throw std::runtime_error("EWF 没有可读取的镜像分卷");
    }
    if (libewf_handle_initialize(&candidate->handle, &error) != 1) {
        throw std::runtime_error(
            "初始化 libewf 读取器失败: " + ewf_error_message(error));
    }
    if (error != nullptr) {
        libewf_error_free(&error);
    }
#if defined(_WIN32) && defined(LIBEWF_HAVE_WIDE_CHARACTER_TYPE)
    if (libewf_handle_open_wide(
            candidate->handle, candidate->filenames,
            candidate->number_of_filenames,
            LIBEWF_ACCESS_FLAG_READ, &error) != 1) {
#else
    if (libewf_handle_open(candidate->handle, candidate->filenames,
                           candidate->number_of_filenames,
                           LIBEWF_ACCESS_FLAG_READ, &error) != 1) {
#endif
        throw std::runtime_error(
            "只读打开 EWF 分卷失败: " + ewf_error_message(error));
    }
    if (error != nullptr) {
        libewf_error_free(&error);
    }
    // 分卷句柄已在打开阶段全部建立，此处恢复当前目录不影响后续读取
    directory_guard.reset();

    size64_t media_size = 0;
    if (libewf_handle_get_media_size(
            candidate->handle, &media_size, &error) != 1) {
        throw std::runtime_error(
            "读取 EWF 介质容量失败: " + ewf_error_message(error));
    }
    if (error != nullptr) {
        libewf_error_free(&error);
    }

    std::uint32_t bytes_per_sector = 0;
    if (libewf_handle_get_bytes_per_sector(
            candidate->handle, &bytes_per_sector, &error) != 1) {
        throw std::runtime_error(
            "读取 EWF 扇区大小失败: " + ewf_error_message(error));
    }
    if (error != nullptr) {
        libewf_error_free(&error);
    }
    if (bytes_per_sector == 0 || bytes_per_sector > 65536U) {
        throw std::runtime_error("EWF 报告了无效的扇区大小");
    }

    source_path_ = image_path;
    size_ = media_size;
    sector_size_ = bytes_per_sector;
    state_ = std::move(candidate);
#else
    (void)image_path;
    throw std::runtime_error("当前构建未启用 libewf");
#endif
}

std::size_t EwfImageDriver::read_sector(
    std::uint64_t sector_number,
    std::span<std::byte> destination) const {
    if (destination.size() != sector_size_) {
        throw std::invalid_argument("EWF read_sector 缓冲区大小必须等于介质扇区大小");
    }
    if (sector_number > std::numeric_limits<std::uint64_t>::max() /
                            sector_size_) {
        throw std::out_of_range("EWF 扇区偏移溢出");
    }
    return read_at(sector_number * sector_size_, destination);
}

std::size_t EwfImageDriver::read_at(
    std::uint64_t offset,
    std::span<std::byte> destination) const {
#ifdef UIFE_WITH_LIBEWF
    if (!state_ || state_->handle == nullptr) {
        throw std::runtime_error("EWF 镜像尚未打开");
    }
    if (offset > size_) {
        throw std::out_of_range("EWF 读取偏移超出介质范围");
    }
    const auto available = size_ - offset;
    const auto requested = static_cast<std::size_t>(
        std::min<std::uint64_t>(available, destination.size()));
    if (offset > static_cast<std::uint64_t>(
                     std::numeric_limits<off64_t>::max())) {
        throw std::out_of_range("EWF 读取偏移超出 libewf 可寻址范围");
    }
    if (requested == 0) {
        return 0;
    }

    std::scoped_lock lock(state_->mutex);
    libewf_error_t* error = nullptr;
    const auto count = libewf_handle_read_buffer_at_offset(
        state_->handle, destination.data(), requested,
        static_cast<off64_t>(offset), &error);
    if (count < 0) {
        throw std::runtime_error(
            "读取 EWF 介质数据失败: " + ewf_error_message(error));
    }
    if (error != nullptr) {
        libewf_error_free(&error);
    }
    if (static_cast<std::uint64_t>(count) != requested) {
        throw std::runtime_error("EWF 介质读取返回了意外的短读");
    }
    return requested;
#else
    (void)offset;
    (void)destination;
    throw std::runtime_error("当前构建未启用 libewf");
#endif
}

std::uint64_t EwfImageDriver::get_size() const noexcept {
    return size_;
}

std::uint32_t EwfImageDriver::sector_size() const noexcept {
    return sector_size_;
}

std::filesystem::path EwfImageDriver::source_path() const {
    return source_path_;
}

std::string_view EwfImageDriver::format_name() const noexcept {
    return "EWF/E01";
}

void EwfImageDriver::close() noexcept {
    state_.reset();
    source_path_.clear();
    size_ = 0;
    sector_size_ = 512;
}

} // namespace uife
