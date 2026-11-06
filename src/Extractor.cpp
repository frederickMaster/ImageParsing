#include "uife/Extractor.h"

#include "uife/utils/Sha256.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <map>
#include <stdexcept>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <sddl.h>
#include <winioctl.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__) || defined(__APPLE__)
#include <sys/xattr.h>
#endif
#endif

namespace uife {
namespace {

struct PendingNode {
    VFSNode node;
    std::filesystem::path relative_path;
};

std::filesystem::path utf8_path(const std::string& value) {
    std::u8string encoded;
    encoded.reserve(value.size());
    for (const unsigned char character : value) {
        encoded.push_back(static_cast<char8_t>(character));
    }
    return std::filesystem::path(encoded);
}

// NTFS 根目录下的系统元数据文件：属于文件系统结构而非用户数据，
// 整卷导出时跳过（仍可通过显式路径单独提取）。
bool is_ntfs_system_file(std::string_view name) {
    static constexpr std::string_view names[] = {
        "$MFT", "$MFTMirr", "$LogFile", "$Volume", "$AttrDef", "$Bitmap",
        "$Boot", "$BadClus", "$Secure", "$UpCase", "$Extend", "$OrphanFiles",
    };
    for (const auto candidate : names) {
        if (name == candidate) {
            return true;
        }
    }
    return false;
}

// 导出用的本地名字：优先取 VFS 路径末段，路径缺失时退回显示名。
// 这里刻意不做路径校验——非法名字由调用方拒绝，避免在遍历阶段抛异常。
std::string node_output_name(const VFSNode& node) {
    const std::string_view path(node.path);
    const auto separator = path.find_last_of('/');
    auto basename = separator == std::string_view::npos
        ? path : path.substr(separator + 1);
    if (basename.empty()) {
        return node.name;
    }
    return std::string(basename);
}

void collect_tree(const VFS& filesystem,
                  const VFSNode& node,
                  const std::filesystem::path& relative,
                  std::vector<PendingNode>& pending,
                  std::size_t depth) {    if (depth > 256) {
        throw std::runtime_error("目录树超过安全递归深度限制");
    }
    pending.push_back({node, relative});
    if (node.type != NodeType::directory) {
        return;
    }
    const auto virtual_path = VFS::normalize_path(node.path);
    for (auto child : filesystem.list_dir(virtual_path)) {
        // 解析器可能给出与显示名不同的规范路径（如 WIM 的 "/images/1 - 名称"），
        // 导出时统一用路径末段作为本地名字，避免父子路径不一致。
        const auto output_name = node_output_name(child);
        if (output_name.empty() || output_name == "." || output_name == ".." ||
            output_name.find('/') != std::string::npos ||
            output_name.find('\\') != std::string::npos ||
            output_name.find('\0') != std::string::npos ||
            output_name.find(':') != std::string::npos) {
            throw std::runtime_error("文件系统提供了不安全的文件名");
        }
        if (child.path.empty()) {
            child.path = virtual_path == "/" ? "/" + output_name
                                             : virtual_path + "/" + output_name;
        }
        if (virtual_path == "/" && is_ntfs_system_file(output_name)) {
            continue;
        }
        collect_tree(filesystem, child, relative / utf8_path(output_name), pending, depth + 1);
    }
}

void ensure_safe_destination(const std::filesystem::path& root,
                             const std::filesystem::path& relative,
                             bool create_parents) {
    // Windows 上 "/" 只有 root_directory 而没有 root_name，is_absolute() 为 false，
    // 因此还要显式拒绝带根路径的输入，避免 root / "/" 把导出目标变成盘符根。
    if (relative.empty() || relative.is_absolute() || relative.has_root_path()) {
        throw std::invalid_argument("导出路径必须是非空相对路径");
    }
    for (const auto& part : relative) {
        if (part.empty() || part == "." || part == ".." ||
            part.native().find(static_cast<std::filesystem::path::value_type>(':')) !=
                std::filesystem::path::string_type::npos) {
            throw std::invalid_argument("导出路径包含不安全分量");
        }
    }
    std::filesystem::path current = root;
    auto end = relative.end();
    if (end != relative.begin()) {
        --end;
    }
    for (auto part = relative.begin(); part != end; ++part) {
        if (*part == "." || *part == ".." || part->empty()) {
            throw std::invalid_argument("导出路径包含不安全分量");
        }
        current /= *part;
        std::error_code error;
        const auto status = std::filesystem::symlink_status(current, error);
        if (!error && std::filesystem::is_symlink(status)) {
            throw std::runtime_error("导出路径经过现有符号链接，已拒绝以防止目录逃逸");
        }
        if (error && error != std::errc::no_such_file_or_directory) {
            throw std::filesystem::filesystem_error("检查导出路径失败", current, error);
        }
        if (create_parents && !std::filesystem::exists(current)) {
            std::filesystem::create_directory(current);
        }
    }
}

std::string hardlink_key(const VFSMetadata& metadata) {
    const auto identity = metadata.hard_link_group != 0
        ? metadata.hard_link_group : metadata.inode;
    return metadata.link_count > 1 && identity != 0 ? std::to_string(identity) : std::string{};
}

// NTFS 重解析点标签。目录联接（junction）与符号链接在磁盘上是两种不同的
// 重解析点，导出时必须分别重建：把 junction 导出成文件符号链接会改变目标语义
// （Windows 上文件符号链接指向目录是不可用的），也会丢掉"无需特权的目录重定向"
// 这一取证特征。
constexpr std::uint32_t kReparseTagMountPoint = 0xA0000003U;

std::string to_utf8(const std::filesystem::path& path);

#ifdef _WIN32
// 与 winnt.h 的 REPARSE_DATA_BUFFER 前 16 字节布局一致；MinGW 的 winnt.h 在
// 部分 _WIN32_WINNT 取值下不提供该联合体，这里只声明用得到的 mount point 部分。
struct MountPointReparseData {
    std::uint32_t tag;
    std::uint16_t data_length;
    std::uint16_t reserved;
    std::uint16_t substitute_name_offset;
    std::uint16_t substitute_name_length;
    std::uint16_t print_name_offset;
    std::uint16_t print_name_length;
    // 之后紧跟 PathBuffer：替换名 \0 打印名 \0
};

std::wstring widen_utf8(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    const auto length = MultiByteToWideChar(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) {
        throw std::runtime_error("无法把 UTF-8 路径转换为宽字符");
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), length);
    return result;
}

// 用 FSCTL_SET_REPARSE_POINT 重建目录联接。junction 与符号链接不同：它不需要
// SeCreateSymbolicLinkPrivilege，取证机以普通用户运行也能忠实还原。
void create_directory_junction(const std::filesystem::path& link,
                               const std::string& target) {
    auto wide_target = widen_utf8(target);
    for (auto& character : wide_target) {
        if (character == L'/') {
            character = L'\\';
        }
    }
    // 目标可能来自 $REPARSE_POINT（已规范化为 '/'）或 libtsk 的 meta->link
    // （保留 \??\ 前缀的原样），两种形式都要收敛成 "C:\..." 再拼 NT 路径。
    for (const std::wstring prefix : {std::wstring(L"\\??\\"), std::wstring(L"\\\\?\\")}) {
        if (wide_target.rfind(prefix, 0) == 0) {
            wide_target.erase(0, prefix.size());
            break;
        }
    }
    const bool local_absolute = wide_target.size() >= 3 &&
        std::iswalpha(static_cast<wint_t>(wide_target[0])) != 0 &&
        wide_target[1] == L':' && wide_target[2] == L'\\';
    if (!local_absolute) {
        // 卷 GUID 路径、UNC 目标与相对目标都不是合法的 junction 目标。
        throw std::runtime_error(
            "目录联接的目标不是本地绝对路径，无法重建: " + target);
    }
    const std::wstring substitute = L"\\??\\" + wide_target;

    constexpr USHORT header_size = 8;        // Tag + DataLength + Reserved
    constexpr USHORT mount_header_size = 8;  // 四个 offset/length 字段
    const auto substitute_bytes = static_cast<USHORT>(substitute.size() * sizeof(wchar_t));
    const auto print_bytes = static_cast<USHORT>(wide_target.size() * sizeof(wchar_t));
    const auto path_bytes = static_cast<USHORT>(substitute_bytes + sizeof(wchar_t) +
                                                print_bytes + sizeof(wchar_t));
    std::vector<std::byte> buffer(
        header_size + mount_header_size + path_bytes, std::byte{0});
    auto* reparse = reinterpret_cast<MountPointReparseData*>(buffer.data());
    reparse->tag = IO_REPARSE_TAG_MOUNT_POINT;
    reparse->data_length = static_cast<std::uint16_t>(mount_header_size + path_bytes);
    reparse->reserved = 0;
    reparse->substitute_name_offset = 0;
    reparse->substitute_name_length = substitute_bytes;
    reparse->print_name_offset = static_cast<std::uint16_t>(substitute_bytes + sizeof(wchar_t));
    reparse->print_name_length = print_bytes;
    auto* payload = buffer.data() + header_size + mount_header_size;
    std::memcpy(payload, substitute.data(), substitute_bytes);
    std::memcpy(payload + substitute_bytes + sizeof(wchar_t),
                wide_target.data(), print_bytes);

    std::error_code error;
    std::filesystem::create_directory(link, error);
    if (error && error != std::errc::file_exists) {
        throw std::filesystem::filesystem_error("创建联接点目录失败", link, error);
    }
    // readonly-audit: link 是刚创建的导出目录联接点，不是源镜像
    HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("无法打开目录以写入重解析点: " + to_utf8(link));
    }
    DWORD returned = 0;
    // readonly-audit: 在导出目录的联接点上写重解析数据
    const auto ok = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.data(),
        static_cast<DWORD>(buffer.size()), nullptr, 0, &returned, nullptr);
    const auto last_error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    if (!ok) {
        std::filesystem::remove(link, error);
        throw std::runtime_error("重建目录联接失败（Windows 错误 " +
            std::to_string(last_error) + "）: " + to_utf8(link) + " -> " + target);
    }
}

// MinGW 的 libstdc++ 在 Windows 上把 create_symlink 实现成 ENOSYS（"Function
// not implemented"），所以直接调用 CreateSymbolicLinkW；顺带能按文件属性区分
// 目录/文件链接，并在缺少特权时给出可操作的提示。
void create_windows_symlink(const std::filesystem::path& link,
                            const std::string& target, bool directory) {
    auto wide_target = widen_utf8(target);
    for (auto& character : wide_target) {
        if (character == L'/') {
            character = L'\\';
        }
    }
    const auto flags = directory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0UL;
    if (CreateSymbolicLinkW(link.c_str(), wide_target.c_str(), flags) != 0) {
        return;
    }
    auto last_error = GetLastError();
    // Windows 10 1703+ 允许在开启"开发者模式"后无特权创建符号链接。
    if (last_error == ERROR_PRIVILEGE_NOT_HELD) {
        constexpr DWORD allow_unprivileged = 0x2;
        if (CreateSymbolicLinkW(link.c_str(), wide_target.c_str(),
                                flags | allow_unprivileged) != 0) {
            return;
        }
        last_error = GetLastError();
    }
    throw std::runtime_error(
        "创建符号链接失败（Windows 错误 " + std::to_string(last_error) +
        "）：普通账户需要开启开发者模式或以管理员身份运行；目标: " + target);
}
#endif


std::string hash_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("无法重新读取导出文件进行哈希校验: " + path.string());
    }
    Sha256 hash;
    std::array<std::byte, 64U * 1024U> buffer{};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            hash.update(std::span<const std::byte>(buffer).first(
                static_cast<std::size_t>(count)));
        }
    }
    if (!input.eof()) {
        throw std::runtime_error("读取导出文件以进行哈希校验时失败");
    }
    return Sha256::to_hex(hash.finish());
}

std::string to_utf8(const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size());
}

// 把数据流名称编码为可安全落盘的文件名片段：仅保留 [A-Za-z0-9._-]，
// 其余字节按 %XX 转义（与 APFS 卷名编码风格一致）。"." 与 ".." 整体转义，
// 避免产生目录穿越语义的文件名。过长的名称截断后附加原文哈希以保证唯一。
std::string encode_stream_component(const std::string& name) {
    constexpr char hex[] = "0123456789ABCDEF";
    const bool dot_component = name == "." || name == "..";
    std::string encoded;
    encoded.reserve(std::min<std::size_t>(name.size() * 3, 128));
    for (const unsigned char value : name) {
        const bool plain = !dot_component &&
            ((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
             (value >= '0' && value <= '9') || value == '.' || value == '_' ||
             value == '-');
        if (plain) {
            encoded.push_back(static_cast<char>(value));
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[value >> 4U]);
            encoded.push_back(hex[value & 0x0fU]);
        }
    }
    constexpr std::size_t maximum_component = 96;
    if (encoded.size() > maximum_component) {
        std::uint64_t hash = 1469598103934665603ULL;
        for (const unsigned char value : name) {
            hash = (hash ^ value) * 1099511628211ULL;
        }
        char suffix[17]{};
        std::snprintf(suffix, sizeof(suffix), "%016llx",
                      static_cast<unsigned long long>(hash));
        encoded.resize(maximum_component);
        encoded += "-";
        encoded += suffix;
    }
    return encoded;
}

// 写入一个数据流（默认流或命名流），返回源数据流的 SHA-256。
// 稀疏文件写入器：跳过空洞，不把零字节真的写到盘上。
// NTFS 需要先 FSCTL_SET_SPARSE（在非 NTFS 目标上会失败，此时退回写零），
// POSIX 的文件天然支持空洞，lseek 即可。
class SparseWriter {
public:
    SparseWriter(const std::filesystem::path& path, bool want_sparse) {
#ifdef _WIN32
        // readonly-audit: path 是导出目标，用 CREATE_NEW 保证不会覆盖已有文件
        handle_ = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("无法创建导出文件: " + to_utf8(path));
        }
        if (want_sparse) {
            FILE_SET_SPARSE_BUFFER setting{TRUE};
            DWORD returned = 0;
            // readonly-audit: 把刚创建的导出文件标记为稀疏
            sparse_ = DeviceIoControl(handle_, FSCTL_SET_SPARSE, &setting,
                                      sizeof(setting), nullptr, 0, &returned,
                                      nullptr) != 0;
        }
#else
        (void)want_sparse;
        // readonly-audit: 同上，O_EXCL 保证只创建新文件
        fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd_ < 0) {
            throw std::runtime_error("无法创建导出文件: " + to_utf8(path) +
                                     ": " + std::strerror(errno));
        }
        sparse_ = want_sparse;   // POSIX 上空洞是隐式的
#endif
    }

    ~SparseWriter() {
        release();
    }

    SparseWriter(const SparseWriter&) = delete;
    SparseWriter& operator=(const SparseWriter&) = delete;

    [[nodiscard]] bool sparse() const noexcept { return sparse_; }

    void write(const std::byte* data, std::size_t length) {
        if (length == 0) {
            return;
        }
#ifdef _WIN32
        auto remaining = length;
        const auto* cursor = data;
        while (remaining > 0) {
            const auto chunk = static_cast<DWORD>(
                std::min<std::size_t>(remaining, 1U << 20U));
            DWORD written = 0;
            // readonly-audit: 写入的是导出文件句柄 handle_
            if (WriteFile(handle_, cursor, chunk, &written, nullptr) == 0 ||
                written != chunk) {
                throw std::runtime_error("写入导出文件失败（Windows 错误 " +
                    std::to_string(GetLastError()) + "）");
            }
            cursor += chunk;
            remaining -= chunk;
        }
#else
        auto remaining = length;
        const auto* cursor = data;
        while (remaining > 0) {
            const auto count = ::write(fd_, cursor, remaining);
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::runtime_error("写入导出文件失败: " +
                                         std::string(std::strerror(errno)));
            }
            cursor += count;
            remaining -= static_cast<std::size_t>(count);
        }
#endif
        position_ += length;
    }

    // 跳过长 length 字节。无法使用稀疏文件时写零，保证内容仍然正确。
    void skip(std::uint64_t length) {
        if (length == 0) {
            return;
        }
        if (!sparse_) {
            static const std::array<std::byte, 64U * 1024U> zeros{};
            auto remaining = length;
            while (remaining > 0) {
                const auto chunk = static_cast<std::size_t>(
                    std::min<std::uint64_t>(remaining, zeros.size()));
                write(zeros.data(), chunk);
                remaining -= chunk;
            }
            return;
        }
#ifdef _WIN32
        LARGE_INTEGER distance{};
        distance.QuadPart = static_cast<LONGLONG>(length);
        if (SetFilePointerEx(handle_, distance, nullptr, FILE_CURRENT) == 0) {
            throw std::runtime_error("定位导出文件失败（Windows 错误 " +
                std::to_string(GetLastError()) + "）");
        }
#else
        if (::lseek(fd_, static_cast<off_t>(length), SEEK_CUR) < 0) {
            throw std::runtime_error("定位导出文件失败: " +
                                     std::string(std::strerror(errno)));
        }
#endif
        position_ += length;
    }

    // 设置最终长度并落盘。空洞部分的长度也在 position_ 里，所以两端一致。
    void finish(std::uint64_t size) {
#ifdef _WIN32
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(size);
        if (SetFilePointerEx(handle_, position, nullptr, FILE_BEGIN) == 0 ||
            // readonly-audit: 设定导出文件的长度
            SetEndOfFile(handle_) == 0) {
            throw std::runtime_error("设置导出文件长度失败（Windows 错误 " +
                std::to_string(GetLastError()) + "）");
        }
        release();
#else
        if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) {
            throw std::runtime_error("设置导出文件长度失败: " +
                                     std::string(std::strerror(errno)));
        }
        release();
#endif
    }

private:
    void release() noexcept {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
#else
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
#endif
    }

#ifdef _WIN32
    HANDLE handle_{INVALID_HANDLE_VALUE};
#else
    int fd_{-1};
#endif
    bool sparse_{};
    std::uint64_t position_{};
};

// 分配图是否恰好覆盖 [0,size)：首尾相接、不重叠、不越界。只有这种完整映射
// 才能用来跳过空洞；任何缺口都退回顺序读取，避免把数据写成零。
bool extents_cover_file(const std::vector<VFSExtent>& extents, std::uint64_t size) {
    if (extents.empty()) {
        return false;
    }
    std::uint64_t cursor = 0;
    for (const auto& extent : extents) {
        if (extent.length == 0 || extent.offset != cursor ||
            extent.length > size - cursor) {
            return false;
        }
        cursor += extent.length;
    }
    return cursor == size;
}

std::string write_stream_content(const VFS& filesystem,
                                 const std::string& vfs_path,
                                 const std::string* stream,
                                 std::uint64_t size,
                                 const std::filesystem::path& output_path,
                                 const ExtractOptions& options,
                                 std::uint64_t& completed,
                                 std::uint64_t total_bytes,
                                 const std::vector<VFSExtent>* extents = nullptr) {
    const auto label = stream == nullptr ? vfs_path : vfs_path + ":" + *stream;
    // 只有默认数据流、且分配图完整覆盖文件时才按空洞跳过读取；其余的（ADS、
    // 分配图缺失或截断、压缩文件的逻辑视图）一律顺序读，保证内容正确。
    const auto sparse = extents != nullptr && extents_cover_file(*extents, size);
    SparseWriter output(output_path, sparse);
    Sha256 source_hash;
    std::array<std::byte, 128U * 1024U> buffer{};
    std::array<std::byte, 64U * 1024U> zeros{};

    const auto read_range = [&](std::uint64_t offset, std::size_t requested) {
        const auto count = stream == nullptr
            ? filesystem.read(vfs_path, offset,
                              std::span<std::byte>(buffer).first(requested))
            : filesystem.read_stream(vfs_path, *stream, offset,
                                     std::span<std::byte>(buffer).first(requested));
        if (count == 0 || count > requested) {
            throw std::runtime_error("读取镜像文件内容失败或返回了无效长度: " + label);
        }
        return count;
    };

    if (sparse) {
        for (const auto& extent : *extents) {
            if (!extent.allocated) {
                // 空洞：按零计入哈希，但不在盘上占用空间。
                auto remaining = extent.length;
                while (remaining > 0) {
                    const auto chunk = static_cast<std::size_t>(
                        std::min<std::uint64_t>(remaining, zeros.size()));
                    source_hash.update(std::span<const std::byte>(zeros).first(chunk));
                    remaining -= chunk;
                }
                output.skip(extent.length);
                completed += extent.length;
                if (options.progress) {
                    options.progress(completed, total_bytes);
                }
                continue;
            }
            auto offset = extent.offset;
            auto remaining = extent.length;
            while (remaining > 0) {
                const auto requested = static_cast<std::size_t>(
                    std::min<std::uint64_t>(remaining, buffer.size()));
                const auto count = read_range(offset, requested);
                const auto chunk = std::span<const std::byte>(buffer).first(count);
                output.write(chunk.data(), chunk.size());
                source_hash.update(chunk);
                offset += count;
                remaining -= count;
                completed += count;
                if (options.progress) {
                    options.progress(completed, total_bytes);
                }
            }
        }
    } else {
        std::uint64_t offset = 0;
        while (offset < size) {
            const auto requested = static_cast<std::size_t>(
                std::min<std::uint64_t>(size - offset, buffer.size()));
            const auto count = read_range(offset, requested);
            const auto chunk = std::span<const std::byte>(buffer).first(count);
            output.write(chunk.data(), chunk.size());
            source_hash.update(chunk);
            offset += count;
            completed += count;
            if (options.progress) {
                options.progress(completed, total_bytes);
            }
        }
    }
    output.finish(size);

    const auto expected_hash = Sha256::to_hex(source_hash.finish());
    const auto actual_hash = hash_file(output_path);
    if (expected_hash != actual_hash) {
        throw std::runtime_error("导出文件 SHA-256 校验失败: " + label);
    }
    return expected_hash;
}

// 命名数据流导出为与主文件同级的侧车文件：<文件名>__ads__<编码后的流名>。
// 采用侧车文件而不是 Windows 原生 ADS 写入，保证 Windows/Linux/macOS 行为一致，
// 且导出目录可被普通工具直接校验与打包。
void export_alternate_streams(const VFS& filesystem,
                              const VFSNode& node,
                              const std::filesystem::path& relative,
                              const std::filesystem::path& root,
                              const ExtractOptions& options,
                              std::uint64_t total_bytes,
                              std::uint64_t& completed,
                              std::vector<ExtractedFile>& extracted) {
    if (node.metadata.alternate_streams.empty()) {
        return;
    }
    const auto file_name = to_utf8(relative.filename());
    const auto parent = relative.parent_path();
    for (const auto& stream : node.metadata.alternate_streams) {
        if (!stream.readable) {
            continue;
        }
        if (!stream.size_valid) {
            throw std::runtime_error("命名数据流大小元数据异常，拒绝导出: " +
                                     node.path + ":" + stream.name);
        }
        const auto stream_relative = parent /
            utf8_path(file_name + "__ads__" + encode_stream_component(stream.name));
        ensure_safe_destination(root, stream_relative, false);
        const auto output_path = root / stream_relative;
        std::error_code error;
        const auto status = std::filesystem::symlink_status(output_path, error);
        if (!error && status.type() != std::filesystem::file_type::not_found) {
            throw std::runtime_error("导出目标已存在，拒绝覆盖: " + to_utf8(output_path));
        }
        if (error && error != std::errc::no_such_file_or_directory) {
            throw std::filesystem::filesystem_error("检查导出目标失败", output_path, error);
        }
        const auto hash = write_stream_content(filesystem, node.path, &stream.name,
            stream.size, output_path, options, completed, total_bytes);
        extracted.push_back({node.path, stream.name, to_utf8(stream_relative),
                             hash, stream.size});
    }
}

// opt_in=true 用于"用户没开可选还原开关"这种默认路径：仍然记一条，
// 但调用方可以据此把它和真正的还原失败分开统计。
void report_metadata_issue(const ExtractOptions& options, const std::string& path,
                           std::string_view field, const std::string& reason,
                           bool opt_in = false) {
    if (options.metadata_note) {
        options.metadata_note(
            MetadataNote{path, std::string(field), reason, opt_in});
    }
}

#ifdef _WIN32
// SID 字符串（S-1-5-18）→ 二进制 SID；转换失败时保持空值并让调用方记录原因。
struct LocalSid {
    PSID value{};

    explicit LocalSid(const std::string& sddl) {
        if (!sddl.empty()) {
            (void)ConvertStringSidToSidA(sddl.c_str(), &value);
        }
    }

    ~LocalSid() {
        if (value != nullptr) {
            LocalFree(value);
        }
    }

    LocalSid(const LocalSid&) = delete;
    LocalSid& operator=(const LocalSid&) = delete;
};

// 还原所有者需要 SeRestorePrivilege。进程以管理员身份运行时装到令牌里就能启用；
// 普通账户拿不到该特权，AdjustTokenPrivileges 会失败，此时退回只写 DACL。
bool enable_restore_privilege() {
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(),
                         TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token) == 0) {
        return false;
    }
    LUID luid{};
    auto enabled = false;
    if (LookupPrivilegeValueW(nullptr, L"SeRestorePrivilege", &luid) != 0) {
        TOKEN_PRIVILEGES privileges{};
        privileges.PrivilegeCount = 1;
        privileges.Privileges[0].Luid = luid;
        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        enabled = AdjustTokenPrivileges(token, FALSE, &privileges,
                                        sizeof(privileges), nullptr, nullptr) != 0 &&
                  GetLastError() == ERROR_SUCCESS;
    }
    CloseHandle(token);
    return enabled;
}

// 把 $Secure:$SDS 解出的安全描述符写回导出文件：DACL 逐条重建，所有者/组按 SID 设置。
// 这是"ACL 重建"里唯一能忠实表达语义的平台——POSIX 权限位表不出 NT ACL 的
// 拒绝 ACE、继承标志与 SID 归属，硬映射只会制造假精确。
void apply_ntfs_security(const std::filesystem::path& path,
                         const VFSSecurityDescriptor& security,
                         const ExtractOptions& options,
                         const std::string& vfs_path) {
    if (!security.valid) {
        return;
    }
    if (!security.dacl_present) {
        // NULL DACL 意味着"所有人完全控制"。照搬到导出目录等于给证据目录放开权限，
        // 所以只记录不还原。
        report_metadata_issue(options, vfs_path, "dacl",
            "源对象使用 NULL DACL（所有人完全控制），为避免在导出目录里放开权限未还原");
        return;
    }
    std::vector<std::vector<std::byte>> ace_blobs;
    ace_blobs.reserve(security.dacl.size());
    for (const auto& ace : security.dacl) {
        if (ace.raw_type > SYSTEM_AUDIT_ACE_TYPE) {
            report_metadata_issue(options, vfs_path, "dacl",
                "跳过无法用基础 ACE 表达的对象 ACE（类型 " +
                std::to_string(ace.raw_type) + "）");
            continue;
        }
        LocalSid sid(ace.sid);
        if (sid.value == nullptr) {
            report_metadata_issue(options, vfs_path, "dacl",
                "SID 无法解析，跳过该 ACE: " + ace.sid);
            continue;
        }
        const auto sid_length = GetLengthSid(sid.value);
        const auto ace_size = static_cast<DWORD>(sizeof(ACE_HEADER) +
                                                 sizeof(ACCESS_MASK) + sid_length);
        std::vector<std::byte> blob(ace_size, std::byte{0});
        auto* header = reinterpret_cast<ACE_HEADER*>(blob.data());
        header->AceType = ace.raw_type;
        header->AceFlags = ace.inherited ? INHERITED_ACE : 0;
        header->AceSize = static_cast<WORD>(ace_size);
        auto* mask = reinterpret_cast<ACCESS_MASK*>(blob.data() + sizeof(ACE_HEADER));
        *mask = ace.mask;
        std::memcpy(blob.data() + sizeof(ACE_HEADER) + sizeof(ACCESS_MASK),
                    sid.value, sid_length);
        ace_blobs.push_back(std::move(blob));
    }

    DWORD acl_size = sizeof(ACL);
    for (const auto& blob : ace_blobs) {
        acl_size += static_cast<DWORD>(blob.size());
    }
    std::vector<std::byte> acl_buffer(acl_size, std::byte{0});
    auto* acl = reinterpret_cast<PACL>(acl_buffer.data());
    if (InitializeAcl(acl, acl_size, ACL_REVISION) == 0) {
        report_metadata_issue(options, vfs_path, "dacl",
            "初始化 ACL 失败（Windows 错误 " + std::to_string(GetLastError()) + "）");
        return;
    }
    for (auto& blob : ace_blobs) {
        if (AddAce(acl, ACL_REVISION, MAXDWORD, blob.data(),
                   static_cast<DWORD>(blob.size())) == 0) {
            report_metadata_issue(options, vfs_path, "dacl",
                "写入 ACE 失败（Windows 错误 " + std::to_string(GetLastError()) + "）");
            return;
        }
    }

    LocalSid owner(security.owner_sid);
    LocalSid group(security.group_sid);
    SECURITY_DESCRIPTOR descriptor{};
    if (InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) == 0 ||
        SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE) == 0) {
        report_metadata_issue(options, vfs_path, "dacl",
            "构造安全描述符失败（Windows 错误 " + std::to_string(GetLastError()) + "）");
        return;
    }
    if (owner.value != nullptr) {
        // 提权只需尝试一次：以管理员身份运行时这一次就能把 SeRestorePrivilege
        // 装进令牌，之后 SetFileSecurityW 才能改写所有者；普通账户拿不到该特权，
        // 下面的两次调用会退化成只写 DACL 并留下一条记录。
        [[maybe_unused]] static const auto restore_privilege =
            enable_restore_privilege();
        SetSecurityDescriptorOwner(&descriptor, owner.value, FALSE);
    }
    if (group.value != nullptr) {
        SetSecurityDescriptorGroup(&descriptor, group.value, FALSE);
    }

    // SE_DACL_PROTECTED：源对象自己的 ACL 就是最终 ACL，不让父目录的继承 ACE 混进来。
    const auto dacl_only = DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION;
    SECURITY_INFORMATION full = dacl_only;
    if (owner.value != nullptr) {
        full |= OWNER_SECURITY_INFORMATION;
    }
    if (group.value != nullptr) {
        full |= GROUP_SECURITY_INFORMATION;
    }
    if (SetFileSecurityW(path.c_str(), full, &descriptor) != 0) {
        return;
    }
    const auto first_error = GetLastError();
    if (SetFileSecurityW(path.c_str(), dacl_only, &descriptor) != 0) {
        report_metadata_issue(options, vfs_path, "owner/group",
            "已还原 DACL；所有者/组需要 SeRestorePrivilege（以管理员身份运行），"
            "当前账户没有该特权");
        return;
    }
    report_metadata_issue(options, vfs_path, "dacl",
        "设置 ACL 失败（Windows 错误 " + std::to_string(first_error) +
        " / " + std::to_string(GetLastError()) + "）");
}
#endif

void apply_metadata(const std::filesystem::path& path,
                    const VFSMetadata& metadata,
                    bool is_symlink,
                    const ExtractOptions& options,
                    const std::string& vfs_path) {
    // EFS 与 NTFS 压缩是卷级属性，导出到普通目录后无处安放：加密文件导出的是
    // 密文（$EFS 证书不在镜像里），压缩文件导出成明文。两者都得说清楚，
    // 否则"哈希对得上"会让人误以为拿到的是可读原文/等价的存储形态。
    if (metadata.encrypted) {
        report_metadata_issue(options, vfs_path, "encryption",
            "源对象是 EFS 加密文件，导出的是密文；解密所需的 $EFS 证书与私钥未随镜像导出");
    }
    if (metadata.compressed) {
        report_metadata_issue(options, vfs_path, "compression",
            "源对象带 NTFS 压缩属性，导出文件按未压缩存放（内容已按原始字节还原）");
    }
#ifdef _WIN32
    if (!is_symlink && metadata.mode != 0) {
        const auto permissions = static_cast<std::filesystem::perms>(metadata.mode & 0777U);
        std::error_code error;
        std::filesystem::permissions(path, permissions,
                                     std::filesystem::perm_options::replace, error);
        if (error) {
            report_metadata_issue(options, vfs_path, "permissions",
                "还原权限位失败: " + error.message());
        }
    }
    if (metadata.has_security && !is_symlink) {
        if (options.restore_acl) {
            apply_ntfs_security(path, metadata.security, options, vfs_path);
        } else {
            report_metadata_issue(options, vfs_path, "security_descriptor",
                "未启用 --restore-acl，源对象的所有者/组/DACL 未还原", true);
        }
    }
    if (metadata.has_created_time || metadata.has_accessed_time || metadata.has_modified_time) {
        // readonly-audit: 只改导出文件的时间戳，path 来自输出目录
        HANDLE handle = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS |
                (is_symlink ? FILE_FLAG_OPEN_REPARSE_POINT : 0),
            nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            report_metadata_issue(options, vfs_path, "timestamps",
                "无法打开导出文件以还原时间戳（Windows 错误 " +
                std::to_string(GetLastError()) + "）");
            return;
        }
        constexpr std::int64_t epoch_delta = 11644473600LL;
        auto to_filetime = [](std::chrono::system_clock::time_point point) {
            constexpr std::int64_t ticks_per_second = 10000000LL;
            const auto duration = point.time_since_epoch();
            const auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count()
                             / 100 + epoch_delta * ticks_per_second;
            ULARGE_INTEGER value{};
            value.QuadPart = static_cast<ULONGLONG>(ticks);
            FILETIME result{};
            result.dwLowDateTime = value.LowPart;
            result.dwHighDateTime = value.HighPart;
            return result;
        };
        FILETIME created{};
        FILETIME accessed{};
        FILETIME modified{};
        const FILETIME* created_ptr = nullptr;
        const FILETIME* accessed_ptr = nullptr;
        const FILETIME* modified_ptr = nullptr;
        if (metadata.has_created_time) {
            created = to_filetime(metadata.created);
            created_ptr = &created;
        }
        if (metadata.has_accessed_time) {
            accessed = to_filetime(metadata.accessed);
            accessed_ptr = &accessed;
        }
        if (metadata.has_modified_time) {
            modified = to_filetime(metadata.modified);
            modified_ptr = &modified;
        }
        const BOOL success = SetFileTime(handle, created_ptr, accessed_ptr, modified_ptr);
        CloseHandle(handle);
        if (!success) {
            report_metadata_issue(options, vfs_path, "timestamps",
                "还原时间戳失败（Windows 错误 " +
                std::to_string(GetLastError()) + "）");
        }
    }
    if (!metadata.extended_attributes.empty()) {
        std::string names;
        for (const auto& entry : metadata.extended_attributes) {
            names += (names.empty() ? "" : ", ") + entry.first;
        }
        report_metadata_issue(options, vfs_path, "extended_attributes",
            "Windows 没有等价的扩展属性接口，未还原: " + names);
    }
#else
    if (metadata.has_owner) {
        const auto owner_change = is_symlink
            ? lchown(path.c_str(), static_cast<uid_t>(metadata.uid),
                     static_cast<gid_t>(metadata.gid))
            : chown(path.c_str(), static_cast<uid_t>(metadata.uid),
                    static_cast<gid_t>(metadata.gid));
        if (owner_change != 0) {
            report_metadata_issue(options, vfs_path, "owner",
                "还原文件所有者失败（需要 root 权限）: " +
                std::string(std::strerror(errno)));
        }
    }
    if (metadata.has_security) {
        // NT ACL 无法在 POSIX 权限位上等价表达：拒绝 ACE、继承标志、SID 归属都无处可放。
        report_metadata_issue(options, vfs_path, "security_descriptor",
            "NTFS ACL 只能在 Windows 上还原；源对象的所有者/组/DACL 未应用");
    }
    if (!is_symlink && metadata.mode != 0 &&
        chmod(path.c_str(), static_cast<mode_t>(metadata.mode & 07777U)) != 0) {
        report_metadata_issue(options, vfs_path, "permissions",
            "还原文件权限失败: " + std::string(std::strerror(errno)));
    }

#if defined(__linux__) || defined(__APPLE__)
    // 扩展属性排在 chmod 之后：system.posix_acl_access 会改写权限位，
    // 先写 xattr 再 chmod 会把刚还原的 ACL 覆盖掉。
    if (!metadata.extended_attributes.empty()) {
        for (const auto& [name, value] : metadata.extended_attributes) {
            int result = 0;
#if defined(__APPLE__)
            // macOS 没有 lsetxattr：跟随/不跟随符号链接靠 XATTR_NOFOLLOW 选项表达，
            // setxattr 的签名也多一个 position 参数。
            result = setxattr(path.c_str(), name.c_str(), value.data(),
                              value.size(), 0,
                              is_symlink ? XATTR_NOFOLLOW : 0);
#else
            result = is_symlink
                ? lsetxattr(path.c_str(), name.c_str(), value.data(),
                            value.size(), 0)
                : setxattr(path.c_str(), name.c_str(), value.data(),
                           value.size(), 0);
#endif
            if (result != 0) {
                report_metadata_issue(options, vfs_path, "xattr:" + name,
                    "还原扩展属性失败: " + std::string(std::strerror(errno)));
            }
        }
    }
#else
    if (!metadata.extended_attributes.empty()) {
        report_metadata_issue(options, vfs_path, "extended_attributes",
            "当前平台的扩展属性还原尚未实现");
    }
#endif
    if (metadata.has_accessed_time || metadata.has_modified_time) {
        auto to_timespec = [](std::chrono::system_clock::time_point point) {
            const auto since_epoch = point.time_since_epoch();
            const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(since_epoch);
            const auto remainder = std::chrono::duration_cast<std::chrono::nanoseconds>(
                since_epoch - seconds);
            return timespec{static_cast<time_t>(seconds.count()),
                            static_cast<long>(remainder.count())};
        };
        timespec times[2]{};
        if (metadata.has_accessed_time) {
            times[0] = to_timespec(metadata.accessed);
        } else {
            times[0].tv_nsec = UTIME_OMIT;
        }
        if (metadata.has_modified_time) {
            times[1] = to_timespec(metadata.modified);
        } else {
            times[1].tv_nsec = UTIME_OMIT;
        }
        const auto flags = is_symlink ? AT_SYMLINK_NOFOLLOW : 0;
        if (utimensat(AT_FDCWD, path.c_str(), times, flags) != 0) {
            report_metadata_issue(options, vfs_path, "timestamps",
                "还原文件时间戳失败: " + std::string(std::strerror(errno)));
        }
    }
#endif
}

std::vector<ExtractedFile> perform_extract(const VFS& filesystem,
                                           const VFSNode& start,
                                           const std::filesystem::path& start_relative,
                                           const ExtractOptions& options) {
    if (options.output_directory.empty()) {
        throw std::invalid_argument("必须指定导出目录");
    }
    std::error_code error;
    std::filesystem::create_directories(options.output_directory, error);
    if (error) {
        throw std::filesystem::filesystem_error("创建导出目录失败",
                                                options.output_directory, error);
    }
    const auto root_status = std::filesystem::symlink_status(options.output_directory, error);
    if (error || std::filesystem::is_symlink(root_status) ||
        !std::filesystem::is_directory(root_status)) {
        throw std::runtime_error("导出根目录必须是实际目录且不能是符号链接");
    }
    const auto root = std::filesystem::canonical(options.output_directory, error);
    if (error) {
        throw std::filesystem::filesystem_error("解析导出根目录失败",
                                                options.output_directory, error);
    }

    std::vector<PendingNode> pending;
    if (start.type == NodeType::directory && start.path == "/" && start_relative.empty()) {
        for (auto child : filesystem.list_dir("/")) {
            const auto output_name = node_output_name(child);
            if (output_name.empty() || output_name == "." || output_name == ".." ||
                output_name.find('/') != std::string::npos ||
                output_name.find('\\') != std::string::npos ||
                output_name.find('\0') != std::string::npos ||
                output_name.find(':') != std::string::npos) {
                throw std::runtime_error("文件系统提供了不安全的文件名");
            }
            if (child.path.empty()) {
                child.path = "/" + output_name;
            }
            if (is_ntfs_system_file(output_name)) {
                continue;
            }
            collect_tree(filesystem, child, utf8_path(output_name), pending, 1);
        }
    } else {
        collect_tree(filesystem, start, start_relative, pending, 0);
    }
    std::uint64_t total_bytes = 0;
    for (const auto& item : pending) {
        if (item.node.type == NodeType::regular_file) {
            if (!item.node.metadata.size_valid) {
                throw std::runtime_error("文件大小元数据异常，拒绝导出: " +
                                         item.node.path);
            }
            if (item.node.metadata.size >
                std::numeric_limits<std::uint64_t>::max() - total_bytes) {
                throw std::runtime_error("待导出文件总大小溢出");
            }
            total_bytes += item.node.metadata.size;
            for (const auto& stream : item.node.metadata.alternate_streams) {
                if (!stream.readable) {
                    continue;
                }
                if (!stream.size_valid) {
                    throw std::runtime_error("命名数据流大小元数据异常，拒绝导出: " +
                                             item.node.path + ":" + stream.name);
                }
                if (stream.size >
                    std::numeric_limits<std::uint64_t>::max() - total_bytes) {
                    throw std::runtime_error("待导出文件总大小溢出");
                }
                total_bytes += stream.size;
            }
        }
    }

    std::vector<ExtractedFile> extracted;
    struct PendingDirectory {
        std::filesystem::path output;
        VFSMetadata metadata;
        std::string vfs_path;
    };
    std::vector<PendingDirectory> directories;
    std::map<std::string, std::filesystem::path> hardlinks;
    std::uint64_t completed = 0;
    if (options.progress) {
        options.progress(completed, total_bytes);
    }

    for (const auto& item : pending) {
        if (item.relative_path.empty() || item.relative_path.is_absolute()) {
            throw std::runtime_error("导出相对路径无效");
        }
        ensure_safe_destination(root, item.relative_path, true);
        const auto output_path = root / item.relative_path;
        if (item.node.type == NodeType::directory) {
            std::filesystem::create_directory(output_path, error);
            if (error && error != std::errc::file_exists) {
                throw std::filesystem::filesystem_error("创建导出目录失败", output_path, error);
            }
            const auto status = std::filesystem::symlink_status(output_path, error);
            if (error || std::filesystem::is_symlink(status) ||
                !std::filesystem::is_directory(status)) {
                throw std::runtime_error("导出目录与现有非目录文件冲突");
            }
            directories.push_back({output_path, item.node.metadata, item.node.path});
            continue;
        }
        const auto output_status = std::filesystem::symlink_status(output_path, error);
        if (!error && output_status.type() != std::filesystem::file_type::not_found) {
            throw std::runtime_error("导出目标已存在，拒绝覆盖: " + output_path.string());
        }
        if (error && error != std::errc::no_such_file_or_directory) {
            throw std::filesystem::filesystem_error("检查导出目标失败", output_path, error);
        }
        error.clear();

        if (item.node.type == NodeType::symbolic_link) {
            if (item.node.metadata.symlink_target.empty()) {
                throw std::runtime_error("文件系统未提供符号链接目标: " + item.node.path);
            }
            const auto& metadata = item.node.metadata;
            const bool junction = metadata.has_reparse_tag &&
                metadata.reparse_tag == kReparseTagMountPoint;
#ifdef _WIN32
            if (junction) {
                // junction 是目录重定向：必须重建成联接点，不能退化成文件符号链接。
                create_directory_junction(output_path, metadata.symlink_target);
                apply_metadata(output_path, metadata, true, options, item.node.path);
                continue;
            }
            // NTFS 的符号链接可能是目录链接：FILE_ATTRIBUTE_DIRECTORY 决定标志位，
            // 否则导出成指向目录的文件链接在 Windows 上不可用。
            const bool directory_link = metadata.has_file_attributes &&
                (metadata.file_attributes & 0x10U) != 0;
            create_windows_symlink(output_path, metadata.symlink_target, directory_link);
#else
            if (junction) {
                std::filesystem::create_directory_symlink(
                    utf8_path(metadata.symlink_target), output_path, error);
            } else {
                std::filesystem::create_symlink(
                    utf8_path(metadata.symlink_target), output_path, error);
            }
            if (error) {
                throw std::filesystem::filesystem_error("创建符号链接失败", output_path, error);
            }
#endif
            apply_metadata(output_path, metadata, true, options, item.node.path);
            continue;
        }
        if (item.node.type != NodeType::regular_file) {
            continue;
        }

        const auto link_key = hardlink_key(item.node.metadata);
        if (!link_key.empty()) {
            const auto existing = hardlinks.find(link_key);
            if (existing != hardlinks.end()) {
                std::filesystem::create_hard_link(existing->second, output_path, error);
                if (error) {
                    throw std::filesystem::filesystem_error("还原硬链接失败", output_path, error);
                }
                const auto hash = hash_file(existing->second);
                extracted.push_back({item.node.path, {}, to_utf8(item.relative_path),
                                     hash, item.node.metadata.size});
                completed += item.node.metadata.size;
                if (options.progress) {
                    options.progress(completed, total_bytes);
                }
                // 硬链接副本不重复导出命名数据流：NTFS 上同一 MFT 记录的流已由首个副本导出。
                continue;
            }
        }

        const auto output_relative = to_utf8(item.relative_path);
        const auto* extents = item.node.metadata.extents.empty()
            ? nullptr : &item.node.metadata.extents;
        const auto expected_hash = write_stream_content(filesystem, item.node.path,
            nullptr, item.node.metadata.size, output_path, options, completed,
            total_bytes, extents);
        apply_metadata(output_path, item.node.metadata, false, options, item.node.path);
        if (!link_key.empty()) {
            hardlinks.emplace(link_key, output_path);
        }
        extracted.push_back({item.node.path, {}, output_relative, expected_hash,
                             item.node.metadata.size});
        export_alternate_streams(filesystem, item.node, item.relative_path, root,
                                 options, total_bytes, completed, extracted);
    }

    for (auto directory = directories.rbegin(); directory != directories.rend(); ++directory) {
        apply_metadata(directory->output, directory->metadata, false, options,
                       directory->vfs_path);
    }
    return extracted;
}

} // namespace

std::vector<ExtractedFile> Extractor::extract(const VFS& filesystem,
                                              const std::string& source_path,
                                              const ExtractOptions& options) {
    const auto path = VFS::normalize_path(source_path);
    auto node = filesystem.stat(path);
    if (!filesystem.can_read_content()) {
        // 少数后端（如 ReFS/libfsrefs）只有目录枚举与元数据接口。这里直接拒绝而不是
        // 生成一堆空文件：取证场景下"看起来导出成功但内容为空"比失败更危险。
        throw std::runtime_error(
            "当前文件系统后端只能枚举目录与元数据，无法读取文件内容，因此不支持导出；"
            "请改用 list 查看目录结构");
    }
    if (path == "/") {
        // 导出根目录时按整卷遍历处理：内容直接落到输出目录，而不是再套一层 "/"。
        node.name = "root";
        node.path = "/";
        return perform_extract(filesystem, node, {}, options);
    }
    const auto relative = utf8_path(node.name.empty() ? "root" : node.name);
    return perform_extract(filesystem, node, relative, options);
}

std::vector<ExtractedFile> Extractor::restore(const VFS& filesystem,
                                              const ExtractOptions& options) {
    if (!filesystem.can_read_content()) {
        throw std::runtime_error(
            "当前文件系统后端只能枚举目录与元数据，无法读取文件内容，因此不支持整卷还原；"
            "请改用 list 查看目录结构");
    }
    VFSNode root;
    root.name = "root";
    root.path = "/";
    root.type = NodeType::directory;
    return perform_extract(filesystem, root, {}, options);
}

} // namespace uife
