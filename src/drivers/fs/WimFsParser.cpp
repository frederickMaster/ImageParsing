#include "uife/drivers/fs/WimFsParser.h"

#include "uife/drivers/fs/NtfsSecurity.h"

#include <span>
#include <stdexcept>

#ifdef UIFE_WITH_LIBWIM
#include <wimlib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace uife {
namespace {

using WimString = std::basic_string<wimlib_tchar>;

std::string to_utf8(const wimlib_tchar* value) {
    if (value == nullptr) {
        return {};
    }
#ifdef _WIN32
    const auto length = static_cast<int>(std::char_traits<wchar_t>::length(value));
    if (length == 0) {
        return {};
    }
    const auto required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value, length, nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        throw std::runtime_error("无法将 WIM 文件名转换为 UTF-8");
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, length,
                            result.data(), required, nullptr, nullptr) != required) {
        throw std::runtime_error("无法将 WIM 文件名转换为 UTF-8");
    }
    return result;
#else
    return value;
#endif
}

std::string encode_component(std::string_view value) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(value.size());
    for (const unsigned char character : value) {
        if (character >= 0x20 && character != 0x7f &&
            character != '%' && character != '/' && character != '\\' &&
            character != ':' && character != '<' && character != '>' &&
            character != '"' && character != '|' && character != '?' &&
            character != '*') {
            result.push_back(static_cast<char>(character));
        } else {
            result.push_back('%');
            result.push_back(hex[character >> 4U]);
            result.push_back(hex[character & 0x0fU]);
        }
    }
    if (result == "." || result == "..") {
        std::string encoded;
        encoded.reserve(result.size() * 3);
        for (const char character : result) {
            const auto value_byte = static_cast<unsigned char>(character);
            encoded.push_back('%');
            encoded.push_back(hex[value_byte >> 4U]);
            encoded.push_back(hex[value_byte & 0x0fU]);
        }
        return encoded;
    }
    return result.empty() ? "%00" : result;
}

std::string normalize_vfs_path(std::string path) {
    if (path.empty() || path == "/") {
        return "/";
    }
    if (path.front() != '/' || path.find('\\') != std::string::npos ||
        path.find("//") != std::string::npos || path.ends_with('/') ||
        path.find("/../") != std::string::npos || path.ends_with("/..") ||
        path.find("/./") != std::string::npos || path.ends_with("/.")) {
        throw std::invalid_argument("VFS 路径包含不支持的路径分量");
    }
    return path;
}

std::string parent_path(std::string_view path) {
    const auto separator = path.find_last_of('/');
    if (separator == 0 || separator == std::string_view::npos) {
        return "/";
    }
    return std::string(path.substr(0, separator));
}

std::chrono::system_clock::time_point to_time_point(
    const wimlib_timespec& value, std::int32_t high_seconds) {
    const auto seconds = static_cast<std::int64_t>(high_seconds) *
                             (std::int64_t{1} << 32U) +
                         static_cast<std::int64_t>(value.tv_sec);
    // system_clock::duration 精度因平台而异（libc++ 为微秒），先收敛再构造。
    return std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::seconds(seconds) +
            std::chrono::nanoseconds(value.tv_nsec)));
}

void initialize_wimlib() {
    static std::once_flag initialized;
    static int initialization_result = 0;
    std::call_once(initialized, [] {
        initialization_result = wimlib_global_init(0);
    });
    if (initialization_result != 0) {
        throw std::runtime_error(
            "初始化 wimlib 失败，错误码 " +
            std::to_string(initialization_result));
    }
}

std::vector<std::filesystem::path> find_split_wim_parts(
    const std::filesystem::path& first_part, std::uint16_t total_parts) {
    using NativeString = std::filesystem::path::string_type;
    const auto primary_stem = first_part.stem().native();
    const auto directory = first_part.parent_path().empty()
                               ? std::filesystem::path(".")
                               : first_part.parent_path();
    std::map<std::uint16_t, std::filesystem::path> numbered_parts;
    std::error_code error;
    std::filesystem::directory_iterator item(directory, error);
    if (error) {
        throw std::filesystem::filesystem_error(
            "无法扫描 WIM 分卷目录", directory, error);
    }

    for (const std::filesystem::directory_iterator end; item != end;
         item.increment(error)) {
        if (error) {
            throw std::filesystem::filesystem_error(
                "扫描 WIM 分卷目录失败", directory, error);
        }
        auto extension = item->path().extension().native();
        const auto equals_ascii_case_insensitive = [](auto actual,
                                                       char expected) {
            if (actual >= static_cast<NativeString::value_type>('A') &&
                actual <= static_cast<NativeString::value_type>('Z')) {
                actual = static_cast<NativeString::value_type>(
                    actual + ('a' - 'A'));
            }
            return actual == static_cast<NativeString::value_type>(expected);
        };
        if (extension.size() != 4 ||
            extension[0] != static_cast<NativeString::value_type>('.') ||
            !equals_ascii_case_insensitive(extension[1], 's') ||
            !equals_ascii_case_insensitive(extension[2], 'w') ||
            !equals_ascii_case_insensitive(extension[3], 'm')) {
            continue;
        }

        const auto candidate_stem = item->path().stem().native();
        if (candidate_stem.size() <= primary_stem.size() ||
            candidate_stem.compare(0, primary_stem.size(), primary_stem) != 0) {
            continue;
        }
        const auto suffix = candidate_stem.substr(primary_stem.size());
        std::uint32_t part_number = 0;
        bool numeric_suffix = true;
        for (const auto character : suffix) {
            if (character < static_cast<NativeString::value_type>('0') ||
                character > static_cast<NativeString::value_type>('9')) {
                numeric_suffix = false;
                break;
            }
            const auto digit = static_cast<std::uint32_t>(
                character - static_cast<NativeString::value_type>('0'));
            if (part_number >
                (std::numeric_limits<std::uint16_t>::max() - digit) / 10U) {
                numeric_suffix = false;
                break;
            }
            part_number = part_number * 10U + digit;
        }
        if (!numeric_suffix || part_number < 2 ||
            part_number > total_parts) {
            continue;
        }

        const auto status = item->symlink_status(error);
        if (error) {
            throw std::filesystem::filesystem_error(
                "读取 WIM 分卷文件状态失败", item->path(), error);
        }
        if (!std::filesystem::is_regular_file(status)) {
            throw std::runtime_error(
                "WIM 分卷路径不是普通文件: " + item->path().string());
        }
        if (!numbered_parts.emplace(
                static_cast<std::uint16_t>(part_number), item->path()).second) {
            throw std::runtime_error(
                "WIM 分卷编号重复: " + std::to_string(part_number));
        }
    }
    if (error) {
        throw std::filesystem::filesystem_error(
            "扫描 WIM 分卷目录失败", directory, error);
    }

    std::vector<std::filesystem::path> parts;
    parts.reserve(total_parts);
    parts.push_back(first_part);
    for (std::uint32_t part_number = 2; part_number <= total_parts;
         ++part_number) {
        const auto found =
            numbered_parts.find(static_cast<std::uint16_t>(part_number));
        if (found == numbered_parts.end()) {
            throw std::runtime_error(
                "WIM 分卷不完整，缺少第 " + std::to_string(part_number) +
                "/" + std::to_string(total_parts) + " 卷");
        }
        parts.push_back(found->second);
    }
    return parts;
}

std::string error_text(int error) {
    const auto* message = wimlib_get_error_string(
        static_cast<wimlib_error_code>(error));
    try {
        const auto text = to_utf8(message);
        if (!text.empty()) {
            return text;
        }
    } catch (const std::exception&) {
    }
    return "wimlib 错误码 " + std::to_string(error);
}

class TemporaryDirectoryCleanup {
public:
    explicit TemporaryDirectoryCleanup(std::filesystem::path path)
        : path_(std::move(path)) {}

    ~TemporaryDirectoryCleanup() {
        if (!path_.empty()) {
            std::error_code error;
            // readonly-audit: 只清理自己创建的临时目录，源目录不经过这里
            std::filesystem::remove_all(path_, error);
        }
    }

    void release() noexcept {
        path_.clear();
    }

private:
    std::filesystem::path path_;
};

std::filesystem::path create_staging_directory() {
    static std::atomic<std::uint64_t> next_id{0};
    std::random_device random;
    const auto root = std::filesystem::temp_directory_path();
    for (unsigned int attempt = 0; attempt < 32; ++attempt) {
        const auto token =
            (static_cast<std::uint64_t>(random()) << 32U) ^
            static_cast<std::uint64_t>(random()) ^
            next_id.fetch_add(1, std::memory_order_relaxed);
        std::array<char, 16> text{};
        const auto converted = std::to_chars(
            text.data(), text.data() + text.size(), token, 16);
        const auto candidate = root /
            ("uife-wim-" + std::string(text.data(), converted.ptr));
        std::error_code error;
        if (std::filesystem::create_directory(candidate, error)) {
            return candidate;
        }
        if (error && error != std::errc::file_exists) {
            throw std::filesystem::filesystem_error(
                "无法创建 WIM 临时提取目录", candidate, error);
        }
    }
    throw std::runtime_error("无法分配唯一的 WIM 临时提取目录");
}

struct IterationContext {
    std::string image_prefix;
    int image_index{};
    std::map<std::string, VFSNode>* nodes{};
    struct ImagePath {
        int image_index{};
        WimString path;
    };
    std::map<std::string, ImagePath>* image_paths{};
    std::exception_ptr exception;
};

int add_directory_entry(const wimlib_dir_entry* entry, void* opaque) {
    auto& context = *static_cast<IterationContext*>(opaque);
    try {
        if (entry == nullptr || entry->full_path == nullptr ||
            entry->depth == 0) {
            return 0;
        }
        const auto wim_path_text = to_utf8(entry->full_path);
        std::string relative;
        relative.reserve(wim_path_text.size());
        for (const char character : wim_path_text) {
            if (character == WIMLIB_WIM_PATH_SEPARATOR) {
                relative.push_back('/');
            } else {
                relative.push_back(character);
            }
        }
        while (!relative.empty() && relative.front() == '/') {
            relative.erase(relative.begin());
        }
        if (relative.empty()) {
            return 0;
        }

        std::string vfs_path = context.image_prefix;
        std::size_t component_start = 0;
        while (component_start < relative.size()) {
            const auto separator = relative.find('/', component_start);
            const auto component_end = separator == std::string::npos
                ? relative.size() : separator;
            const auto component = std::string_view(relative).substr(
                component_start, component_end - component_start);
            vfs_path.push_back('/');
            vfs_path += encode_component(component);
            component_start = component_end + 1;
        }

        VFSNode node;
        const auto slash = vfs_path.find_last_of('/');
        node.name = vfs_path.substr(slash + 1);
        node.path = vfs_path;
        if ((entry->attributes & WIMLIB_FILE_ATTRIBUTE_DIRECTORY) != 0) {
            node.type = NodeType::directory;
            node.metadata.size_valid = false;
        } else if ((entry->attributes & WIMLIB_FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            node.type = NodeType::other;
            node.metadata.size_valid = false;
        } else {
            node.type = NodeType::regular_file;
            node.metadata.size =
                entry->streams[0].resource.uncompressed_size;
            node.metadata.size_valid = true;
        }
        node.metadata.inode = entry->hard_link_group_id;
        node.metadata.hard_link_group = entry->hard_link_group_id;
        node.metadata.link_count = std::max<std::uint32_t>(entry->num_links, 1U);
        // WIM 目录项里存的就是 Windows 的 FILE_ATTRIBUTE_*，与 NTFS
        // $STANDARD_INFORMATION.FileAttributes 同一套位，因此直接复用同一组常量，
        // 让下游按同一份标志位判断稀疏/压缩/加密。
        node.metadata.file_attributes =
            static_cast<std::uint32_t>(entry->attributes);
        node.metadata.has_file_attributes = true;
        node.metadata.sparse =
            (node.metadata.file_attributes & ntfs_attribute::sparse) != 0;
        node.metadata.compressed =
            (node.metadata.file_attributes & ntfs_attribute::compressed) != 0;
        node.metadata.encrypted =
            (node.metadata.file_attributes & ntfs_attribute::encrypted) != 0;
        node.metadata.reparse_point =
            (node.metadata.file_attributes & ntfs_attribute::reparse_point) != 0;
        // WIM 的 security data 条目就是自相对 SECURITY_DESCRIPTOR，与
        // $Secure:$SDS 里的描述符同构，直接复用同一个解码器；wimlib 未捕获到
        // 描述符时（例如 POSIX 上打包，或打包时用了 NO_ACLS）指针为空。
        if (entry->security_descriptor != nullptr &&
            entry->security_descriptor_size != 0) {
            auto descriptor = ntfs_security::parse_security_descriptor(
                std::span<const std::byte>(
                    reinterpret_cast<const std::byte*>(entry->security_descriptor),
                    entry->security_descriptor_size));
            if (descriptor.valid) {
                node.metadata.has_security = true;
                node.metadata.security = std::move(descriptor);
            }
        }
        if (entry->unix_mode != 0) {
            node.metadata.mode = entry->unix_mode;
            node.metadata.uid = entry->unix_uid;
            node.metadata.gid = entry->unix_gid;
            node.metadata.has_owner = true;
        } else if (node.type == NodeType::directory) {
            node.metadata.mode =
                (entry->attributes & WIMLIB_FILE_ATTRIBUTE_READONLY) != 0
                    ? 0555U : 0755U;
        } else {
            node.metadata.mode =
                (entry->attributes & WIMLIB_FILE_ATTRIBUTE_READONLY) != 0
                    ? 0444U : 0644U;
        }
        node.metadata.created =
            to_time_point(entry->creation_time, entry->creation_time_high);
        node.metadata.modified =
            to_time_point(entry->last_write_time, entry->last_write_time_high);
        node.metadata.accessed =
            to_time_point(entry->last_access_time, entry->last_access_time_high);
        node.metadata.has_created_time = true;
        node.metadata.has_modified_time = true;
        node.metadata.has_accessed_time = true;
        // WIM 命名数据流：wimlib 的目录项会给出流名与大小，但公共 API 不提供
        // 单流提取，因此这里只报告元数据（readable=false），导出时跳过。
        for (std::uint32_t index = 1; index <= entry->num_named_streams; ++index) {
            const auto& wim_stream = entry->streams[index];
            if (wim_stream.stream_name == nullptr) {
                continue;
            }
            VFSStream stream;
            stream.name = to_utf8(wim_stream.stream_name);
            stream.size = wim_stream.resource.uncompressed_size;
            stream.readable = false;
            node.metadata.alternate_streams.push_back(std::move(stream));
        }

        context.nodes->insert_or_assign(vfs_path, std::move(node));
        if ((entry->attributes & (WIMLIB_FILE_ATTRIBUTE_DIRECTORY |
                                  WIMLIB_FILE_ATTRIBUTE_REPARSE_POINT)) == 0) {
            context.image_paths->insert_or_assign(
                vfs_path,
                IterationContext::ImagePath{
                    context.image_index, WimString(entry->full_path)});
        }
        return 0;
    } catch (...) {
        context.exception = std::current_exception();
        return -1;
    }
}

} // namespace

struct WimFsParser::Impl {
    WIMStruct* wim{};
    std::map<std::string, VFSNode> nodes;
    std::map<std::string, IterationContext::ImagePath> image_paths;
    mutable std::map<std::string, std::filesystem::path> staged_files;
    mutable std::filesystem::path staging_directory;
    mutable std::mutex mutex;

    ~Impl() {
        if (wim != nullptr) {
            wimlib_free(wim);
        }
        if (!staging_directory.empty()) {
            std::error_code error;
            // readonly-audit: 只清理暂存目录，源镜像与导出目录不在其中
            std::filesystem::remove_all(staging_directory, error);
        }
    }
};

WimFsParser::WimFsParser() = default;
WimFsParser::~WimFsParser() = default;

void WimFsParser::mount(std::shared_ptr<IImageDriver> image,
                        std::uint64_t byte_offset,
                        std::uint64_t byte_length) {
#ifdef UIFE_WITH_LIBWIM
    if (!image || byte_offset != 0 || byte_length != image->get_size()) {
        throw std::invalid_argument("WIM 必须作为完整镜像从偏移 0 打开");
    }
    std::array<std::byte, 8> signature{};
    if (image->read_at(0, signature) != signature.size() ||
        std::memcmp(signature.data(), "MSWIM", 5) != 0) {
        throw std::runtime_error("输入文件不是有效的 WIM/ESD 镜像");
    }

    initialize_wimlib();
    auto candidate = std::make_unique<Impl>();
    const auto source = image->source_path();
    const auto result =
        wimlib_open_wim(source.c_str(), 0, &candidate->wim);
    if (result != 0) {
        throw std::runtime_error("只读打开 WIM/ESD 失败: " +
                                 error_text(result));
    }

    wimlib_wim_info info{};
    const auto info_result = wimlib_get_wim_info(candidate->wim, &info);
    if (info_result != 0 || info.image_count == 0) {
        throw std::runtime_error(
            info_result != 0
                ? "读取 WIM 镜像信息失败: " + error_text(info_result)
                : "WIM 中没有可浏览的镜像");
    }
    if (info.total_parts == 0 || info.part_number == 0 ||
        info.part_number > info.total_parts) {
        throw std::runtime_error("WIM 分卷头中的卷编号无效");
    }
    if (info.part_number != 1) {
        throw std::runtime_error("请选择 WIM 首卷（通常为 .swm 文件）打开");
    }
    if (info.total_parts > 1) {
        const auto parts = find_split_wim_parts(source, info.total_parts);
        std::vector<const wimlib_tchar*> resource_paths;
        resource_paths.reserve(parts.size() - 1);
        for (std::size_t index = 1; index < parts.size(); ++index) {
            resource_paths.push_back(parts[index].c_str());
        }
        const auto reference_result = wimlib_reference_resource_files(
            candidate->wim, resource_paths.data(),
            static_cast<unsigned>(resource_paths.size()), 0, 0);
        if (reference_result != 0) {
            throw std::runtime_error(
                "引用 WIM 其他分卷失败（分卷可能不匹配或损坏）: " +
                error_text(reference_result));
        }
    }

    VFSNode root;
    root.name = "/";
    root.path = "/";
    root.type = NodeType::directory;
    root.metadata.mode = 0555;
    candidate->nodes.emplace(root.path, std::move(root));

    VFSNode images;
    images.name = "images";
    images.path = "/images";
    images.type = NodeType::directory;
    images.metadata.mode = 0555;
    candidate->nodes.emplace(images.path, std::move(images));

    for (std::uint32_t index = 1; index <= info.image_count; ++index) {
        const auto image_index = static_cast<int>(index);
        const auto image_root = "/images/" + std::to_string(index);
        const auto* image_name =
            wimlib_get_image_name(candidate->wim, image_index);
        VFSNode image_node;
        image_node.name = std::to_string(index);
        const auto readable_name = to_utf8(image_name);
        if (!readable_name.empty()) {
            image_node.name += " - " + readable_name;
        }
        image_node.path = image_root;
        image_node.type = NodeType::directory;
        image_node.metadata.mode = 0555;
        candidate->nodes.emplace(image_root, std::move(image_node));

        IterationContext context{
            image_root, image_index, &candidate->nodes,
            &candidate->image_paths, {}};
        const auto iterate_result = wimlib_iterate_dir_tree(
            candidate->wim, image_index, WIMLIB_WIM_ROOT_PATH,
            WIMLIB_ITERATE_DIR_TREE_FLAG_RECURSIVE,
            add_directory_entry, &context);
        if (context.exception) {
            std::rethrow_exception(context.exception);
        }
        if (iterate_result != 0) {
            throw std::runtime_error("枚举 WIM 文件树失败: " +
                                     error_text(iterate_result));
        }
    }
    impl_ = std::move(candidate);
#else
    (void)image;
    (void)byte_offset;
    (void)byte_length;
    throw std::runtime_error("当前构建未启用 wimlib");
#endif
}

std::vector<VFSNode> WimFsParser::list_dir(const std::string& path) const {
#ifdef UIFE_WITH_LIBWIM
    if (!impl_) {
        throw std::runtime_error("WIM 镜像尚未打开");
    }
    const auto normalized = normalize_vfs_path(path);
    const auto directory = impl_->nodes.find(normalized);
    if (directory == impl_->nodes.end() ||
        directory->second.type != NodeType::directory) {
        throw std::runtime_error("WIM 路径不是目录: " + normalized);
    }
    std::vector<VFSNode> result;
    for (const auto& [node_path, node] : impl_->nodes) {
        if (node_path != normalized && parent_path(node_path) == normalized) {
            result.push_back(node);
        }
    }
    return result;
#else
    (void)path;
    throw std::runtime_error("当前构建未启用 wimlib");
#endif
}

VFSNode WimFsParser::get_metadata(const std::string& path) const {
#ifdef UIFE_WITH_LIBWIM
    if (!impl_) {
        throw std::runtime_error("WIM 镜像尚未打开");
    }
    const auto normalized = normalize_vfs_path(path);
    const auto node = impl_->nodes.find(normalized);
    if (node == impl_->nodes.end()) {
        throw std::runtime_error("WIM 路径不存在: " + normalized);
    }
    return node->second;
#else
    (void)path;
    throw std::runtime_error("当前构建未启用 wimlib");
#endif
}

std::size_t WimFsParser::read_file(
    const std::string& path, std::uint64_t offset,
    std::span<std::byte> destination) const {
#ifdef UIFE_WITH_LIBWIM
    if (!impl_ || impl_->wim == nullptr) {
        throw std::runtime_error("WIM 镜像尚未打开");
    }
    const auto normalized = normalize_vfs_path(path);
    const auto node = impl_->nodes.find(normalized);
    if (node == impl_->nodes.end() ||
        node->second.type != NodeType::regular_file ||
        !node->second.metadata.size_valid) {
        throw std::runtime_error("WIM 路径不是可读取的普通文件: " + normalized);
    }
    if (offset > node->second.metadata.size) {
        throw std::out_of_range("WIM 文件读取偏移超出文件范围");
    }
    if (destination.empty() || offset == node->second.metadata.size) {
        return 0;
    }

    std::scoped_lock lock(impl_->mutex);
    auto staged = impl_->staged_files.find(normalized);
    if (staged == impl_->staged_files.end()) {
        const auto source_path = impl_->image_paths.find(normalized);
        if (source_path == impl_->image_paths.end()) {
            throw std::runtime_error("WIM 文件缺少可读取的数据流");
        }
        if (impl_->staging_directory.empty()) {
            impl_->staging_directory = create_staging_directory();
        }
        const auto file_directory =
            impl_->staging_directory /
            ("file-" + std::to_string(impl_->staged_files.size()));
        std::error_code create_error;
        if (!std::filesystem::create_directory(file_directory, create_error)) {
            throw std::filesystem::filesystem_error(
                "无法创建 WIM 文件临时目录", file_directory, create_error);
        }
        TemporaryDirectoryCleanup cleanup(file_directory);
        const auto target = file_directory.native();
        const wimlib_tchar* requested_path = source_path->second.path.c_str();
        const auto result = wimlib_extract_paths(
            impl_->wim, source_path->second.image_index, target.c_str(),
            &requested_path, 1,
            WIMLIB_EXTRACT_FLAG_NO_ATTRIBUTES |
                WIMLIB_EXTRACT_FLAG_NO_PRESERVE_DIR_STRUCTURE);
        if (result != 0) {
            throw std::runtime_error("从 WIM 读取文件失败: " +
                                     error_text(result));
        }
        std::filesystem::path extracted_file;
        for (const auto& item :
             std::filesystem::directory_iterator(file_directory)) {
            if (!item.is_regular_file()) {
                continue;
            }
            if (!extracted_file.empty()) {
                throw std::runtime_error(
                    "wimlib 临时提取产生了多个文件，拒绝读取不确定结果");
            }
            extracted_file = item.path();
        }
        if (extracted_file.empty()) {
            throw std::runtime_error("wimlib 未生成预期的临时文件");
        }
        staged = impl_->staged_files.emplace(
            normalized, std::move(extracted_file)).first;
        cleanup.release();
    }

    std::ifstream input(staged->second, std::ios::binary);
    if (!input) {
        throw std::runtime_error("无法读取 WIM 文件的临时数据");
    }
    input.seekg(static_cast<std::streamoff>(offset));
    if (!input) {
        throw std::runtime_error("无法定位 WIM 文件的临时数据");
    }
    const auto available = node->second.metadata.size - offset;
    const auto requested = static_cast<std::size_t>(
        std::min<std::uint64_t>(available, destination.size()));
    input.read(reinterpret_cast<char*>(destination.data()),
               static_cast<std::streamsize>(requested));
    const auto count = static_cast<std::size_t>(input.gcount());
    if (count != requested) {
        throw std::runtime_error("WIM 临时文件读取发生意外短读");
    }
    return count;
#else
    (void)path;
    (void)offset;
    (void)destination;
    throw std::runtime_error("当前构建未启用 wimlib");
#endif
}

std::string_view WimFsParser::filesystem_name() const noexcept {
    return "WIM/ESD";
}

} // namespace uife

#else

namespace uife {
struct WimFsParser::Impl {};
WimFsParser::WimFsParser() = default;
WimFsParser::~WimFsParser() = default;
void WimFsParser::mount(std::shared_ptr<IImageDriver>, std::uint64_t,
                        std::uint64_t) {
    throw std::runtime_error("当前构建未启用 wimlib");
}
std::vector<VFSNode> WimFsParser::list_dir(const std::string&) const {
    throw std::runtime_error("当前构建未启用 wimlib");
}
VFSNode WimFsParser::get_metadata(const std::string&) const {
    throw std::runtime_error("当前构建未启用 wimlib");
}
std::size_t WimFsParser::read_file(const std::string&, std::uint64_t,
                                   std::span<std::byte>) const {
    throw std::runtime_error("当前构建未启用 wimlib");
}
std::string_view WimFsParser::filesystem_name() const noexcept {
    return "WIM/ESD";
}
} // namespace uife

#endif
