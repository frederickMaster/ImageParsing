#include "uife/drivers/fs/RefsFsParser.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

#ifdef UIFE_WITH_LIBFSREFS

#include "BfioBridge.h"

#include <libfsrefs.h>

// 与 APFS 后端同样的原因：libfsrefs 的公共头只在 LIBFSREFS_HAVE_BFIO（链接外部
// libbfio）时声明该入口，而捆绑实现就在静态库里。
extern "C" int libfsrefs_volume_open_file_io_handle(
    libfsrefs_volume_t* volume,
    uife_bfio_handle_t* file_io_handle,
    int access_flags,
    libfsrefs_error_t** error);

namespace uife {
namespace {

[[noreturn]] void throw_fsrefs(const std::string& context, libfsrefs_error_t* error) {
    std::array<char, 1024> message{};
    if (error != nullptr) {
        libfsrefs_error_backtrace_sprint(error, message.data(), message.size());
    }
    throw std::runtime_error(context + ": " + message.data());
}

void free_error(libfsrefs_error_t*& error) {
    if (error != nullptr) {
        libfsrefs_error_free(&error);
        error = nullptr;
    }
}

using FileEntryPtr = std::unique_ptr<libfsrefs_file_entry_t,
                                     void (*)(libfsrefs_file_entry_t*)>;

FileEntryPtr make_file_entry(libfsrefs_file_entry_t* entry) {
    return FileEntryPtr(entry, [](libfsrefs_file_entry_t* value) {
        if (value != nullptr) {
            libfsrefs_file_entry_free(&value, nullptr);
        }
    });
}

std::string utf8_name(libfsrefs_file_entry_t* entry) {
    std::size_t size = 0;
    if (libfsrefs_file_entry_get_utf8_name_size(entry, &size, nullptr) != 1 ||
        size == 0) {
        return {};
    }
    std::string buffer(size, '\0');
    if (libfsrefs_file_entry_get_utf8_name(
            entry, reinterpret_cast<uint8_t*>(buffer.data()), size, nullptr) != 1) {
        return {};
    }
    return std::string(buffer.c_str());
}

// FILETIME：自 1601-01-01 起的 100 纳秒数。
std::chrono::system_clock::time_point from_filetime(std::uint64_t filetime) {
    constexpr std::uint64_t ticks_per_second = 10000000ULL;
    constexpr std::uint64_t epoch_difference = 11644473600ULL;
    const auto seconds = filetime / ticks_per_second;
    const auto remainder = filetime % ticks_per_second;
    if (seconds < epoch_difference) {
        return std::chrono::system_clock::time_point{};
    }
    return std::chrono::system_clock::time_point{
        std::chrono::seconds(static_cast<std::int64_t>(seconds - epoch_difference)) +
        std::chrono::nanoseconds(remainder * 100ULL)};
}

constexpr std::uint32_t attribute_directory = 0x10000000U;
constexpr std::uint32_t attribute_reparse_point = 0x00000400U;
constexpr std::uint32_t attribute_compressed = 0x00000800U;
constexpr std::uint32_t attribute_encrypted = 0x00004000U;
constexpr std::uint32_t attribute_sparse = 0x00000200U;

std::string ascii_lower(std::string_view value) {
    std::string lowered(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](char ch) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    });
    return lowered;
}

} // namespace

struct RefsFsParser::Impl {
    std::shared_ptr<IImageDriver> image;
    std::uint64_t base{};
    std::uint64_t length{};
    uife_bfio_handle_t* io_handle{};
    libfsrefs_volume_t* volume{};
    std::string volume_name{"ReFS"};
    std::uint32_t bytes_per_sector{};
    std::uint32_t cluster_size{};
    std::uint64_t serial_number{};
    std::uint32_t version{};

    ~Impl() { release(); }

    void release() noexcept {
        if (volume != nullptr) {
            libfsrefs_volume_free(&volume, nullptr);
            volume = nullptr;
        }
        bfio::destroy(io_handle);
        io_handle = nullptr;
    }

    [[nodiscard]] VFSNode make_node(libfsrefs_file_entry_t* entry,
                                    const std::string& path) const {
        VFSNode node;
        node.path = path;
        node.name = utf8_name(entry);
        if (node.name.empty()) {
            const auto separator = path.find_last_of('/');
            node.name = separator == std::string::npos ? path : path.substr(separator + 1);
        }

        std::uint32_t attributes = 0;
        if (libfsrefs_file_entry_get_file_attribute_flags(entry, &attributes,
                                                          nullptr) == 1) {
            node.metadata.file_attributes = attributes;
            node.metadata.has_file_attributes = true;
            node.metadata.compressed = (attributes & attribute_compressed) != 0;
            node.metadata.encrypted = (attributes & attribute_encrypted) != 0;
            node.metadata.sparse = (attributes & attribute_sparse) != 0;
            node.metadata.reparse_point = (attributes & attribute_reparse_point) != 0;
            node.type = (attributes & attribute_directory) != 0 ? NodeType::directory
                                                                : NodeType::regular_file;
        }

        // libfsrefs 只在头文件里声明了 libfsrefs_file_entry_get_size，库里没有实现，
        // 因此 ReFS 的文件大小目前无法报告，明确标注为不可用而不是填 0。
        node.metadata.size_valid = false;

        std::uint64_t filetime = 0;
        if (libfsrefs_file_entry_get_creation_time(entry, &filetime, nullptr) == 1) {
            node.metadata.created = from_filetime(filetime);
            node.metadata.has_created_time = true;
        }
        if (libfsrefs_file_entry_get_modification_time(entry, &filetime, nullptr) == 1) {
            node.metadata.modified = from_filetime(filetime);
            node.metadata.has_modified_time = true;
        }
        if (libfsrefs_file_entry_get_access_time(entry, &filetime, nullptr) == 1) {
            node.metadata.accessed = from_filetime(filetime);
            node.metadata.has_accessed_time = true;
        }
        return node;
    }

    // libfsrefs 没有按路径查找的接口，只能从根目录逐级按名称匹配。
    // ReFS 默认大小写不敏感，这里先做大小写不敏感比较，再退回精确比较。
    [[nodiscard]] FileEntryPtr open_entry(const std::string& path) const {
        libfsrefs_file_entry_t* root = nullptr;
        libfsrefs_error_t* error = nullptr;
        if (libfsrefs_volume_get_root_directory(volume, &root, &error) != 1) {
            throw_fsrefs("无法打开 ReFS 根目录", error);
        }
        auto current = make_file_entry(root);
        if (path.empty() || path == "/") {
            return current;
        }
        std::size_t begin = 0;
        while (begin < path.size()) {
            if (path[begin] == '/') {
                ++begin;
                continue;
            }
            auto end = path.find('/', begin);
            if (end == std::string::npos) {
                end = path.size();
            }
            const std::string component = path.substr(begin, end - begin);
            begin = end;

            int children = 0;
            if (libfsrefs_file_entry_get_number_of_sub_file_entries(
                    current.get(), &children, &error) != 1) {
                throw_fsrefs("无法枚举 ReFS 目录项: " + path, error);
            }
            const auto wanted = ascii_lower(component);
            FileEntryPtr next(nullptr, [](libfsrefs_file_entry_t*) {});
            for (int index = 0; index < children; ++index) {
                libfsrefs_file_entry_t* raw = nullptr;
                if (libfsrefs_file_entry_get_sub_file_entry_by_index(
                        current.get(), index, &raw, &error) != 1) {
                    free_error(error);
                    continue;
                }
                auto candidate = make_file_entry(raw);
                const auto name = utf8_name(candidate.get());
                if (name == component || ascii_lower(name) == wanted) {
                    next = std::move(candidate);
                    break;
                }
            }
            if (next == nullptr) {
                throw std::runtime_error("ReFS 中找不到路径: " + path);
            }
            current = std::move(next);
        }
        return current;
    }
};

RefsFsParser::RefsFsParser() : impl_(std::make_unique<Impl>()) {}

RefsFsParser::~RefsFsParser() = default;

void RefsFsParser::mount(std::shared_ptr<IImageDriver> image,
                         std::uint64_t byte_offset,
                         std::uint64_t byte_length) {
    if (!image) {
        throw std::runtime_error("ReFS 解析器需要有效的镜像对象");
    }
    impl_->release();
    impl_->image = std::move(image);
    impl_->base = byte_offset;
    const auto image_size = impl_->image->get_size();
    if (byte_offset > image_size) {
        throw std::runtime_error("ReFS 分区偏移超出镜像大小");
    }
    impl_->length = byte_length != 0 ? std::min(byte_length, image_size - byte_offset)
                                     : image_size - byte_offset;

    impl_->io_handle = bfio::create(*impl_->image, impl_->base, impl_->length);
    if (impl_->io_handle == nullptr) {
        throw std::runtime_error("无法为 ReFS 卷创建 libbfio 句柄");
    }

    libfsrefs_error_t* error = nullptr;
    if (libfsrefs_volume_initialize(&impl_->volume, &error) != 1) {
        throw_fsrefs("无法初始化 libfsrefs 卷", error);
    }
    if (libfsrefs_volume_open_file_io_handle(impl_->volume, impl_->io_handle,
                                             LIBFSREFS_OPEN_READ, &error) != 1) {
        std::array<char, 1024> message{};
        if (error != nullptr) {
            libfsrefs_error_backtrace_sprint(error, message.data(), message.size());
        }
        free_error(error);
        // 实测：libfsrefs 自己的 fsrefsinfo 对同一个卷报同样的错误，因此这不是适配层
        // 的问题——该库只实现了早期 ReFS 布局，Windows Server 2016 及以后格式化的卷
        // 会在 ministore/container tree 的元数据块签名处失败。
        throw std::runtime_error(
            std::string("无法打开 ReFS 卷。注意 libfsrefs 目前只支持早期 ReFS 布局，"
                        "Windows Server 2016 及以后创建的卷会在此失败: ") +
            message.data());
    }

    std::size_t name_size = 0;
    if (libfsrefs_volume_get_utf8_name_size(impl_->volume, &name_size, nullptr) == 1 &&
        name_size > 1) {
        std::string buffer(name_size, '\0');
        if (libfsrefs_volume_get_utf8_name(
                impl_->volume, reinterpret_cast<uint8_t*>(buffer.data()),
                buffer.size(), nullptr) == 1) {
            impl_->volume_name = std::string(buffer.c_str());
        }
    }
    std::uint16_t bytes_per_sector = 0;
    (void)libfsrefs_volume_get_bytes_per_sector(impl_->volume, &bytes_per_sector,
                                                nullptr);
    impl_->bytes_per_sector = bytes_per_sector;
    std::uint32_t cluster_size = 0;
    (void)libfsrefs_volume_get_cluster_block_size(impl_->volume, &cluster_size,
                                                 nullptr);
    impl_->cluster_size = cluster_size;

    // 根目录能打开才算真正挂载成功。
    auto root = impl_->open_entry("/");
    (void)root;
}

std::vector<VFSNode> RefsFsParser::list_dir(const std::string& path) const {
    auto entry = impl_->open_entry(path);
    int children = 0;
    libfsrefs_error_t* error = nullptr;
    if (libfsrefs_file_entry_get_number_of_sub_file_entries(entry.get(), &children,
                                                            &error) != 1) {
        free_error(error);
        children = 0;
    }
    std::vector<VFSNode> nodes;
    if (children <= 0) {
        nodes.push_back(impl_->make_node(entry.get(), path));
        return nodes;
    }
    nodes.reserve(static_cast<std::size_t>(children));
    const std::string base = path == "/" ? "" : path;
    for (int index = 0; index < children; ++index) {
        libfsrefs_file_entry_t* raw = nullptr;
        if (libfsrefs_file_entry_get_sub_file_entry_by_index(entry.get(), index, &raw,
                                                             &error) != 1) {
            free_error(error);
            continue;
        }
        auto child = make_file_entry(raw);
        auto node = impl_->make_node(child.get(), base);
        node.path = base + "/" + node.name;
        nodes.push_back(std::move(node));
    }
    return nodes;
}

VFSNode RefsFsParser::get_metadata(const std::string& path) const {
    auto entry = impl_->open_entry(path);
    return impl_->make_node(entry.get(), path);
}

std::size_t RefsFsParser::read_file(const std::string& path, std::uint64_t,
                                    std::span<std::byte>) const {
    throw std::runtime_error(
        "ReFS 后端只能枚举目录与元数据：libfsrefs 未提供读取文件内容的接口，"
        "无法导出内容: " + path);
}

std::string_view RefsFsParser::filesystem_name() const noexcept {
    return "ReFS";
}

} // namespace uife

#else

namespace uife {

struct RefsFsParser::Impl {};

RefsFsParser::RefsFsParser() : impl_(std::make_unique<Impl>()) {}
RefsFsParser::~RefsFsParser() = default;

void RefsFsParser::mount(std::shared_ptr<IImageDriver>, std::uint64_t, std::uint64_t) {
    throw std::runtime_error(
        "当前构建未启用 libfsrefs，无法读取 ReFS 卷；请安装 libfsrefs 后重新配置");
}

std::vector<VFSNode> RefsFsParser::list_dir(const std::string&) const {
    throw std::runtime_error("当前构建未启用 libfsrefs");
}

VFSNode RefsFsParser::get_metadata(const std::string&) const {
    throw std::runtime_error("当前构建未启用 libfsrefs");
}

std::size_t RefsFsParser::read_file(const std::string&, std::uint64_t,
                                    std::span<std::byte>) const {
    throw std::runtime_error("当前构建未启用 libfsrefs");
}

std::string_view RefsFsParser::filesystem_name() const noexcept { return "ReFS"; }

} // namespace uife

#endif
