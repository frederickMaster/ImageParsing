#include "uife/drivers/fs/ApfsFsParser.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <memory>
#include <stdexcept>
#include <string>

#ifdef UIFE_WITH_LIBFSAPFS

#include "BfioBridge.h"

#include <libfsapfs.h>

// libfsapfs 的公共头只在 LIBFSAPFS_HAVE_BFIO（即链接外部 libbfio）时才声明这个入口，
// 而静态库里明明带了捆绑版 libbfio 的实现，所以这里按 libfsapfs.h 原型自行声明。
extern "C" int libfsapfs_container_open_file_io_handle(
    libfsapfs_container_t* container,
    uife_bfio_handle_t* file_io_handle,
    int access_flags,
    libfsapfs_error_t** error);

namespace uife {
namespace {

[[noreturn]] void throw_fsapfs(const std::string& context, libfsapfs_error_t* error) {
    std::array<char, 1024> message{};
    if (error != nullptr) {
        libfsapfs_error_backtrace_sprint(error, message.data(), message.size());
    }
    throw std::runtime_error(context + ": " + message.data());
}

void free_error(libfsapfs_error_t*& error) {
    if (error != nullptr) {
        libfsapfs_error_free(&error);
        error = nullptr;
    }
}

using FileEntryPtr = std::unique_ptr<libfsapfs_file_entry_t,
                                     void (*)(libfsapfs_file_entry_t*)>;
using AttributePtr = std::unique_ptr<libfsapfs_extended_attribute_t,
                                     void (*)(libfsapfs_extended_attribute_t*)>;

FileEntryPtr make_file_entry(libfsapfs_file_entry_t* entry) {
    return FileEntryPtr(entry, [](libfsapfs_file_entry_t* value) {
        if (value != nullptr) {
            libfsapfs_file_entry_free(&value, nullptr);
        }
    });
}

AttributePtr make_attribute(libfsapfs_extended_attribute_t* attribute) {
    return AttributePtr(attribute, [](libfsapfs_extended_attribute_t* value) {
        if (value != nullptr) {
            libfsapfs_extended_attribute_free(&value, nullptr);
        }
    });
}

std::string utf8_property(void* owner,
                          int (*size_getter)(void*, size_t*, libfsapfs_error_t**),
                          int (*string_getter)(void*, uint8_t*, size_t, libfsapfs_error_t**)) {
    std::size_t size = 0;
    if (size_getter(owner, &size, nullptr) != 1 || size == 0) {
        return {};
    }
    std::string buffer(size, '\0');
    if (string_getter(owner, reinterpret_cast<uint8_t*>(buffer.data()), size,
                      nullptr) != 1) {
        return {};
    }
    return std::string(buffer.c_str());
}

std::chrono::system_clock::time_point from_apfs_time(std::int64_t nanoseconds) {
    const auto seconds = static_cast<std::int64_t>(nanoseconds / 1000000000);
    const auto remainder = nanoseconds % 1000000000;
    return std::chrono::system_clock::time_point{
               std::chrono::seconds(seconds) + std::chrono::nanoseconds(remainder)};
}

NodeType node_type_from_mode(std::uint16_t mode) {
    switch (mode & 0xF000U) {
    case 0x4000U: return NodeType::directory;
    case 0x8000U: return NodeType::regular_file;
    case 0xA000U: return NodeType::symbolic_link;
    default: return NodeType::other;
    }
}

} // namespace

struct ApfsFsParser::Impl {
    std::string password;
    std::shared_ptr<IImageDriver> image;
    std::uint64_t base{};
    std::uint64_t length{};
    uife_bfio_handle_t* io_handle{};
    libfsapfs_container_t* container{};
    libfsapfs_volume_t* volume{};
    std::string volume_name{"APFS"};
    int volume_count{};

    ~Impl() { release(); }

    void release() noexcept {
        if (volume != nullptr) {
            libfsapfs_volume_free(&volume, nullptr);
            volume = nullptr;
        }
        if (container != nullptr) {
            libfsapfs_container_free(&container, nullptr);
            container = nullptr;
        }
        bfio::destroy(io_handle);
        io_handle = nullptr;
    }

    [[nodiscard]] std::string volume_label() const {
        if (volume_count > 1) {
            return "APFS（卷 " + volume_name + "）";
        }
        return "APFS";
    }

    [[nodiscard]] FileEntryPtr open_entry(const std::string& path) const {
        if (path.empty() || path == "/") {
            libfsapfs_file_entry_t* root = nullptr;
            libfsapfs_error_t* error = nullptr;
            if (libfsapfs_volume_get_root_directory(volume, &root, &error) != 1) {
                throw_fsapfs("无法打开 APFS 根目录", error);
            }
            return make_file_entry(root);
        }
        std::string normalized = path.front() == '/' ? path : "/" + path;
        libfsapfs_file_entry_t* entry = nullptr;
        libfsapfs_error_t* error = nullptr;
        if (libfsapfs_volume_get_file_entry_by_utf8_path(
                volume, reinterpret_cast<const uint8_t*>(normalized.c_str()),
                normalized.size(), &entry, &error) != 1) {
            free_error(error);
            throw std::runtime_error("APFS 中找不到路径: " + normalized);
        }
        return make_file_entry(entry);
    }

    [[nodiscard]] VFSNode make_node(libfsapfs_file_entry_t* entry,
                                    const std::string& path) const {
        VFSNode node;
        node.path = path;
        node.name = utf8_property(
            entry,
            [](void* owner, size_t* size, libfsapfs_error_t** error) {
                return libfsapfs_file_entry_get_utf8_name_size(
                    static_cast<libfsapfs_file_entry_t*>(owner), size, error);
            },
            [](void* owner, uint8_t* value, size_t size, libfsapfs_error_t** error) {
                return libfsapfs_file_entry_get_utf8_name(
                    static_cast<libfsapfs_file_entry_t*>(owner), value, size, error);
            });
        if (node.name.empty()) {
            const auto separator = path.find_last_of('/');
            node.name = separator == std::string::npos ? path : path.substr(separator + 1);
        }

        std::uint16_t mode = 0;
        if (libfsapfs_file_entry_get_file_mode(entry, &mode, nullptr) == 1) {
            node.type = node_type_from_mode(mode);
            node.metadata.mode = mode;
        }

        std::uint64_t identifier = 0;
        if (libfsapfs_file_entry_get_identifier(entry, &identifier, nullptr) == 1) {
            node.metadata.inode = identifier;
        }

        size64_t size = 0;
        if (libfsapfs_file_entry_get_size(entry, &size, nullptr) == 1) {
            node.metadata.size = size;
        } else {
            node.metadata.size_valid = false;
        }

        std::uint32_t owner_id = 0;
        if (libfsapfs_file_entry_get_owner_identifier(entry, &owner_id, nullptr) == 1) {
            node.metadata.uid = owner_id;
            node.metadata.has_owner = true;
        }
        std::uint32_t group_id = 0;
        if (libfsapfs_file_entry_get_group_identifier(entry, &group_id, nullptr) == 1) {
            node.metadata.gid = group_id;
            node.metadata.has_owner = true;
        }
        std::uint32_t links = 0;
        if (libfsapfs_file_entry_get_number_of_links(entry, &links, nullptr) == 1) {
            node.metadata.link_count = links;
        }

        std::int64_t timestamp = 0;
        if (libfsapfs_file_entry_get_creation_time(entry, &timestamp, nullptr) == 1) {
            node.metadata.created = from_apfs_time(timestamp);
            node.metadata.has_created_time = true;
        }
        if (libfsapfs_file_entry_get_modification_time(entry, &timestamp, nullptr) == 1) {
            node.metadata.modified = from_apfs_time(timestamp);
            node.metadata.has_modified_time = true;
        }
        if (libfsapfs_file_entry_get_access_time(entry, &timestamp, nullptr) == 1) {
            node.metadata.accessed = from_apfs_time(timestamp);
            node.metadata.has_accessed_time = true;
        }

        if (node.type == NodeType::symbolic_link) {
            node.metadata.symlink_target = utf8_property(
                entry,
                [](void* owner, size_t* size, libfsapfs_error_t** error) {
                    return libfsapfs_file_entry_get_utf8_symbolic_link_target_size(
                        static_cast<libfsapfs_file_entry_t*>(owner), size, error);
                },
                [](void* owner, uint8_t* value, size_t size,
                   libfsapfs_error_t** error) {
                    return libfsapfs_file_entry_get_utf8_symbolic_link_target(
                        static_cast<libfsapfs_file_entry_t*>(owner), value, size,
                        error);
                });
        }

        // APFS 的扩展属性等价于 NTFS 的备用数据流：com.apple.quarantine、
        // com.apple.FinderInfo、com.apple.metadata:* 都在这里。
        int attribute_count = 0;
        if (libfsapfs_file_entry_get_number_of_extended_attributes(
                entry, &attribute_count, nullptr) == 1) {
            for (int index = 0; index < attribute_count; ++index) {
                libfsapfs_extended_attribute_t* raw = nullptr;
                if (libfsapfs_file_entry_get_extended_attribute_by_index(
                        entry, index, &raw, nullptr) != 1) {
                    continue;
                }
                auto attribute = make_attribute(raw);
                VFSStream stream;
                stream.name = utf8_property(
                    attribute.get(),
                    [](void* owner, size_t* size, libfsapfs_error_t** error) {
                        return libfsapfs_extended_attribute_get_utf8_name_size(
                            static_cast<libfsapfs_extended_attribute_t*>(owner), size,
                            error);
                    },
                    [](void* owner, uint8_t* value, size_t size,
                       libfsapfs_error_t** error) {
                        return libfsapfs_extended_attribute_get_utf8_name(
                            static_cast<libfsapfs_extended_attribute_t*>(owner), value,
                            size, error);
                    });
                if (stream.name.empty()) {
                    continue;
                }
                size64_t attribute_size = 0;
                if (libfsapfs_extended_attribute_get_size(attribute.get(),
                                                          &attribute_size,
                                                          nullptr) == 1) {
                    stream.size = attribute_size;
                } else {
                    stream.size_valid = false;
                }
                node.metadata.alternate_streams.push_back(std::move(stream));
            }
        }
        return node;
    }
};

ApfsFsParser::ApfsFsParser(std::string password)
    : impl_(std::make_unique<Impl>()) {
    impl_->password = std::move(password);
}

ApfsFsParser::~ApfsFsParser() = default;

void ApfsFsParser::mount(std::shared_ptr<IImageDriver> image,
                         std::uint64_t byte_offset,
                         std::uint64_t byte_length) {
    if (!image) {
        throw std::runtime_error("APFS 解析器需要有效的镜像对象");
    }
    impl_->release();
    impl_->image = std::move(image);
    impl_->base = byte_offset;
    const auto image_size = impl_->image->get_size();
    if (byte_offset > image_size) {
        throw std::runtime_error("APFS 分区偏移超出镜像大小");
    }
    impl_->length = byte_length != 0 ? std::min(byte_length, image_size - byte_offset)
                                     : image_size - byte_offset;

    impl_->io_handle = bfio::create(*impl_->image, impl_->base, impl_->length);
    if (impl_->io_handle == nullptr) {
        throw std::runtime_error("无法为 APFS 容器创建 libbfio 句柄");
    }
    libfsapfs_error_t* error = nullptr;
    if (libfsapfs_container_initialize(&impl_->container, &error) != 1) {
        throw_fsapfs("无法初始化 libfsapfs 容器", error);
    }
    if (libfsapfs_container_open_file_io_handle(impl_->container, impl_->io_handle,
                                                LIBFSAPFS_OPEN_READ, &error) != 1) {
        throw_fsapfs("无法打开 APFS 容器", error);
    }
    int volumes = 0;
    if (libfsapfs_container_get_number_of_volumes(impl_->container, &volumes,
                                                  &error) != 1) {
        throw_fsapfs("无法读取 APFS 容器卷数量", error);
    }
    if (volumes <= 0) {
        throw std::runtime_error("APFS 容器中没有可读取的卷");
    }
    impl_->volume_count = volumes;
    if (libfsapfs_container_get_volume_by_index(impl_->container, 0, &impl_->volume,
                                                &error) != 1) {
        throw_fsapfs("无法读取 APFS 卷", error);
    }

    std::size_t name_size = 0;
    if (libfsapfs_volume_get_utf8_name_size(impl_->volume, &name_size, nullptr) == 1 &&
        name_size > 1) {
        std::string buffer(name_size, '\0');
        if (libfsapfs_volume_get_utf8_name(impl_->volume,
                                           reinterpret_cast<uint8_t*>(buffer.data()),
                                           buffer.size(), nullptr) == 1) {
            impl_->volume_name = std::string(buffer.c_str());
        }
    }

    const int locked = libfsapfs_volume_is_locked(impl_->volume, nullptr);
    if (locked == 1) {
        if (impl_->password.empty()) {
            throw std::runtime_error("APFS 卷已加密，需要使用 --password 提供解锁口令");
        }
        if (libfsapfs_volume_set_utf8_password(
                impl_->volume,
                reinterpret_cast<const uint8_t*>(impl_->password.c_str()),
                impl_->password.size(), &error) != 1) {
            throw_fsapfs("设置 APFS 卷口令失败", error);
        }
        if (libfsapfs_volume_unlock(impl_->volume, &error) != 1) {
            throw_fsapfs("APFS 卷解锁失败（口令可能不正确）", error);
        }
    }

    // 根目录能打开才算真正挂载成功，避免把损坏容器当成空卷报告。
    auto root = impl_->open_entry("/");
    (void)root;
}

std::vector<VFSNode> ApfsFsParser::list_dir(const std::string& path) const {
    auto entry = impl_->open_entry(path);
    int children = 0;
    libfsapfs_error_t* error = nullptr;
    if (libfsapfs_file_entry_get_number_of_sub_file_entries(entry.get(), &children,
                                                            &error) != 1) {
        free_error(error);
        children = 0;
    }
    std::vector<VFSNode> nodes;
    if (children <= 0) {
        // 路径指向单个文件时回退为返回自身，与其它后端保持一致。
        nodes.push_back(impl_->make_node(entry.get(), path));
        return nodes;
    }
    nodes.reserve(static_cast<std::size_t>(children));
    const std::string base = path == "/" ? "" : path;
    for (int index = 0; index < children; ++index) {
        libfsapfs_file_entry_t* raw = nullptr;
        if (libfsapfs_file_entry_get_sub_file_entry_by_index(entry.get(), index, &raw,
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

VFSNode ApfsFsParser::get_metadata(const std::string& path) const {
    auto entry = impl_->open_entry(path);
    return impl_->make_node(entry.get(), path);
}

std::size_t ApfsFsParser::read_file(const std::string& path, std::uint64_t offset,
                                    std::span<std::byte> destination) const {
    auto entry = impl_->open_entry(path);
    if (destination.empty()) {
        return 0;
    }
    libfsapfs_error_t* error = nullptr;
    const auto read = libfsapfs_file_entry_read_buffer_at_offset(
        entry.get(), destination.data(), destination.size(),
        static_cast<off64_t>(offset), &error);
    if (read < 0) {
        throw_fsapfs("读取 APFS 文件内容失败: " + path, error);
    }
    return static_cast<std::size_t>(read);
}

std::size_t ApfsFsParser::read_stream(const std::string& path,
                                      const std::string& stream,
                                      std::uint64_t offset,
                                      std::span<std::byte> destination) const {
    auto entry = impl_->open_entry(path);
    libfsapfs_extended_attribute_t* raw = nullptr;
    libfsapfs_error_t* error = nullptr;
    if (libfsapfs_file_entry_get_extended_attribute_by_utf8_name(
            entry.get(), reinterpret_cast<const uint8_t*>(stream.c_str()),
            stream.size(), &raw, &error) != 1) {
        throw_fsapfs("APFS 文件不存在该扩展属性: " + stream, error);
    }
    auto attribute = make_attribute(raw);
    if (destination.empty()) {
        return 0;
    }
    const auto read = libfsapfs_extended_attribute_read_buffer_at_offset(
        attribute.get(), destination.data(), destination.size(),
        static_cast<off64_t>(offset), &error);
    if (read < 0) {
        throw_fsapfs("读取 APFS 扩展属性内容失败: " + stream, error);
    }
    return static_cast<std::size_t>(read);
}

std::string_view ApfsFsParser::filesystem_name() const noexcept {
    return "APFS";
}

} // namespace uife

#else

namespace uife {

struct ApfsFsParser::Impl {};

ApfsFsParser::ApfsFsParser(std::string) : impl_(std::make_unique<Impl>()) {}
ApfsFsParser::~ApfsFsParser() = default;

void ApfsFsParser::mount(std::shared_ptr<IImageDriver>, std::uint64_t, std::uint64_t) {
    throw std::runtime_error(
        "当前构建未启用 libfsapfs，无法读取 APFS 卷；请安装 libfsapfs 后重新配置");
}

std::vector<VFSNode> ApfsFsParser::list_dir(const std::string&) const {
    throw std::runtime_error("当前构建未启用 libfsapfs");
}

VFSNode ApfsFsParser::get_metadata(const std::string&) const {
    throw std::runtime_error("当前构建未启用 libfsapfs");
}

std::size_t ApfsFsParser::read_file(const std::string&, std::uint64_t,
                                    std::span<std::byte>) const {
    throw std::runtime_error("当前构建未启用 libfsapfs");
}

std::size_t ApfsFsParser::read_stream(const std::string&, const std::string&,
                                      std::uint64_t, std::span<std::byte>) const {
    throw std::runtime_error("当前构建未启用 libfsapfs");
}

std::string_view ApfsFsParser::filesystem_name() const noexcept { return "APFS"; }

} // namespace uife

#endif
