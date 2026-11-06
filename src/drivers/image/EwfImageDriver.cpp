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
#if defined(_WIN32) && defined(LIBEWF_HAVE_WIDE_CHARACTER_TYPE)
    std::optional<scoped_directory_change> directory_guard;
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
#if defined(_WIN32) && defined(LIBEWF_HAVE_WIDE_CHARACTER_TYPE)
    directory_guard.reset();
#endif

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

    // libewf 用 glob 只打开实际存在的分卷，缺少的续卷要到读取那一段时才报
    // "missing segment file for offset"。对 E01 单独拿到手（其余续卷还在拷贝）
    // 的现场来说，这会表现为 probe 说“可读取”、list 却在挂载阶段神秘失败。
    // 打开时读一次介质末字节即可提前发现，并把可读前缀量化出来——不完整不应该
    // 让整份证据不可用，能读的部分照样可以导出。
    integrity_note_.clear();
    if (size_ > 0) {
        std::array<std::byte, 1> tail{};
        bool tail_readable = true;
        try {
            (void)read_at(size_ - 1, tail);
        } catch (const std::exception&) {
            tail_readable = false;
        }
        if (!tail_readable) {
            const auto readable = readable_prefix();
            integrity_note_ = "EWF 分卷不完整：介质声明 " +
                              std::to_string(size_) + " 字节，但偏移 " +
                              std::to_string(readable) +
                              " 之后的数据读不到（当前只找到 " +
                              std::to_string(state_->number_of_filenames) +
                              " 个分卷文件，缺少后续续卷）";
            integrity_note_ += readable == 0
                ? "；没有可读内容，请先补齐分卷"
                : "；前 " + std::to_string(readable) + " 字节仍可正常导出";
        }
    }
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
#if defined(UIFE_LIBEWF_READ_RANDOM)
    // libewf-legacy（如 Ubuntu 20140814）只提供旧名 read_random，签名相同
    const auto count = libewf_handle_read_random(
#else
    const auto count = libewf_handle_read_buffer_at_offset(
#endif
        state_->handle, destination.data(), requested,
        static_cast<off64_t>(offset), &error);
    if (count < 0) {
        const auto detail = ewf_error_message(error);
        if (detail.find("missing segment") != std::string::npos) {
            throw std::runtime_error(
                "读取 EWF 介质数据失败: 该位置的数据位于缺失的分卷里，"
                "并非镜像损坏；请补齐 .E02 之后的续卷后重试。原始错误: " +
                detail);
        }
        throw std::runtime_error("读取 EWF 介质数据失败: " + detail);
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

std::string EwfImageDriver::integrity_note() const {
    return integrity_note_;
}

std::uint64_t EwfImageDriver::readable_prefix() const noexcept {
    // 调用前已确认介质末字节读不到。
    // 二分只读 1 字节，代价是 log2(介质大小) 次块读取；缺后续分卷时
    // 可读区间是单调的，所以能定位到第一个读不到的位置。
    if (size_ == 0) {
        return 0;
    }
    std::array<std::byte, 1> first{};
    try {
        (void)read_at(0, first);
    } catch (...) {
        return 0;                       // 连首字节都读不到，可读前缀为零
    }
    if (size_ == 1) {
        return 1;
    }
    std::uint64_t low = 0;              // 已知可读
    std::uint64_t high = size_ - 1;     // 已知不可读
    while (high - low > 1) {
        const auto middle = low + (high - low) / 2;
        std::array<std::byte, 1> sample{};
        try {
            (void)read_at(middle, sample);
            low = middle;
        } catch (const std::exception&) {
            high = middle;
        } catch (...) {
            return 0;
        }
    }
    return low + 1;
}

void EwfImageDriver::close() noexcept {
    state_.reset();
    source_path_.clear();
    size_ = 0;
    sector_size_ = 512;
    integrity_note_.clear();
}

} // namespace uife
