#include "uife/drivers/fs/TskFsParser.h"

#include <algorithm>
#include <ctime>
#include <limits>
#include <stdexcept>

namespace {

void wipe_password(std::string& password) noexcept {
    volatile char* value = password.data();
    for (std::size_t index = 0; index < password.size(); ++index) {
        value[index] = '\0';
    }
    password.clear();
}

} // namespace

#ifdef UIFE_WITH_LIBTSK
#include <tsk/libtsk.h>

#include <cstdio>
#include <array>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

#include "uife/drivers/fs/NtfsSecurity.h"

#ifdef UIFE_WITH_TSK_APFS
#include <tsk/pool/tsk_pool.h>
#endif

namespace uife {
namespace {

struct ExternalImageInfo {
    TSK_IMG_INFO image_info{};
    IImageDriver* image{};
    std::uint64_t source_offset{};
    std::uint64_t source_length{};
};

struct ImageCloser {
    void operator()(TSK_IMG_INFO* value) const noexcept {
        if (value != nullptr) {
            tsk_img_close(value);
        }
    }
};

struct FsCloser {
    void operator()(TSK_FS_INFO* value) const noexcept {
        if (value != nullptr) {
            tsk_fs_close(value);
        }
    }
};

#ifdef UIFE_WITH_TSK_APFS
struct PoolCloser {
    void operator()(const TSK_POOL_INFO* value) const noexcept {
        if (value != nullptr) {
            tsk_pool_close(value);
        }
    }
};

using PoolPtr = std::unique_ptr<const TSK_POOL_INFO, PoolCloser>;
#endif

struct FileCloser {
    void operator()(TSK_FS_FILE* value) const noexcept {
        if (value != nullptr) {
            tsk_fs_file_close(value);
        }
    }
};

struct DirectoryCloser {
    void operator()(TSK_FS_DIR* value) const noexcept {
        if (value != nullptr) {
            tsk_fs_dir_close(value);
        }
    }
};

using ImagePtr = std::unique_ptr<TSK_IMG_INFO, ImageCloser>;
using FsPtr = std::unique_ptr<TSK_FS_INFO, FsCloser>;
using FilePtr = std::unique_ptr<TSK_FS_FILE, FileCloser>;
using DirectoryPtr = std::unique_ptr<TSK_FS_DIR, DirectoryCloser>;

#ifdef UIFE_WITH_TSK_APFS
struct ApfsVolume {
    std::string name;
    std::string path_component;
    std::string open_error;
    bool encrypted{};
    ImagePtr image;
    FsPtr filesystem;
};
#endif

bool is_valid_file_size(const TSK_FS_INFO* filesystem,
                        const TSK_FS_META* metadata) noexcept {
    if (filesystem == nullptr || metadata == nullptr || metadata->size < 0) {
        return false;
    }

    auto maximum_size =
        static_cast<std::uint64_t>(std::numeric_limits<TSK_OFF_T>::max());
    if (TSK_FS_TYPE_ISEXT(filesystem->ftype)) {
        if (filesystem->block_size == 0) {
            return false;
        }
        constexpr std::uint64_t max_logical_blocks = std::uint64_t{1} << 32U;
        const auto block_size = static_cast<std::uint64_t>(filesystem->block_size);
        if (block_size > maximum_size / max_logical_blocks) {
            return false;
        }
        maximum_size = std::min(maximum_size, block_size * max_logical_blocks);
    }
#ifdef UIFE_WITH_TSK_APFS
    if (TSK_FS_TYPE_ISAPFS(filesystem->ftype) &&
        filesystem->img_info != nullptr && filesystem->img_info->size > 0) {
        maximum_size = std::min(
            maximum_size,
            static_cast<std::uint64_t>(filesystem->img_info->size));
    }
#endif
    return static_cast<std::uint64_t>(metadata->size) <= maximum_size;
}

ssize_t external_image_read(TSK_IMG_INFO* image_info, TSK_OFF_T offset,
                            char* buffer, size_t length) noexcept {
    auto* external = reinterpret_cast<ExternalImageInfo*>(image_info);
    if (external == nullptr || external->image == nullptr || offset < 0 ||
        (length != 0 && buffer == nullptr)) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_IMG_ARG);
        tsk_error_set_errstr("UIFE external image read received invalid arguments");
        return -1;
    }
    if (length > static_cast<size_t>(std::numeric_limits<ssize_t>::max())) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_IMG_READ);
        tsk_error_set_errstr("UIFE external image read length exceeds ssize_t");
        return -1;
    }

    const auto image_offset = static_cast<std::uint64_t>(offset);
    if (image_offset > external->source_length ||
        length > external->source_length - image_offset ||
        external->source_offset >
            std::numeric_limits<std::uint64_t>::max() - image_offset) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_IMG_READ);
        tsk_error_set_errstr("UIFE read exceeded the selected image extent");
        return -1;
    }

    try {
        const auto bytes = external->image->read_at(
            external->source_offset + image_offset,
            std::span<std::byte>(reinterpret_cast<std::byte*>(buffer), length));
        return static_cast<ssize_t>(bytes);
    } catch (const std::exception& error) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_IMG_READ);
        tsk_error_set_errstr("UIFE image driver read failed: %s", error.what());
        return -1;
    } catch (...) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_IMG_READ);
        tsk_error_set_errstr("UIFE image driver read failed with an unknown error");
        return -1;
    }
}

void external_image_close(TSK_IMG_INFO* image_info) {
    delete reinterpret_cast<ExternalImageInfo*>(image_info);
}

void external_image_stat(TSK_IMG_INFO*, FILE* output) {
    std::fputs("UIFE read-only image driver\n", output);
}

std::string normalized_path(std::string path) {
    if (path.empty()) {
        return "/";
    }
    if (path.front() != '/') {
        throw std::invalid_argument("VFS 路径必须以 '/' 开始");
    }
    if (path.find('\\') != std::string::npos || path.find("/../") != std::string::npos ||
        path.ends_with("/..") || path.find("/./") != std::string::npos ||
        path.ends_with("/.")) {
        throw std::invalid_argument("VFS 路径包含不支持的路径分量");
    }
    return path;
}

#ifdef UIFE_WITH_TSK_APFS
std::string encode_apfs_path_component(const std::string& name) {    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(name.size());
    for (std::size_t index = 0; index < name.size(); ++index) {
        const auto character = name[index];
        const auto value = static_cast<unsigned char>(character);
        const bool dot_component =
            (name == "." || name == "..") && character == '.';
        if (!dot_component && value >= 0x20 && value != 0x7f &&
            value != '%' && value != ':' && value != '\\' && value != '/' &&
            ((value >= 'a' && value <= 'z') ||
            (value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9') || value == '_' || value == '-' ||
            value == ' ' || value >= 0x80)) {
            result.push_back(character);
        } else {
            result.push_back('%');
            result.push_back(hex[value >> 4U]);
            result.push_back(hex[value & 0x0fU]);
        }
    }
    return result.empty() ? "volume" : result;
}

VFSNode make_synthetic_directory(std::string name, std::string path) {
    VFSNode node;
    node.name = std::move(name);
    node.path = std::move(path);
    node.type = NodeType::directory;
    node.metadata.mode = 0555;
    return node;
}
#endif

// ---------------------------------------------------------------------------
// NTFS 扩展元数据
//
// libtsk 只把常用元数据放进 TSK_FS_META；以下信息仍需按属性读取：
//   * $STANDARD_INFORMATION: FileAttributes 位与 security_id
//   * 命名 $DATA 属性: 备用数据流（ADS）清单
//   * $REPARSE_POINT: 符号链接/联接点目标与重解析标签
//   * $Secure:$SDS: 解码为 security_id → 安全描述符（owner/group SID、
//     DACL/SACL ACE），由 NtfsSecurity 模块完成；libtsk 本身不解析该文件。
// ---------------------------------------------------------------------------
constexpr std::uint32_t kReparseTagMountPoint = 0xA0000003U;
constexpr std::uint32_t kReparseTagSymlink = 0xA000000CU;

std::uint32_t read_le32(const std::byte* data) noexcept {
    return static_cast<std::uint32_t>(std::to_integer<unsigned char>(data[0])) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(data[1])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(data[2])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(data[3])) << 24U);
}

std::uint16_t read_le16(const std::byte* data) noexcept {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(std::to_integer<unsigned char>(data[0])) |
        (static_cast<std::uint16_t>(std::to_integer<unsigned char>(data[1])) << 8U));
}

void append_utf8_code_point(std::string& output, std::uint32_t code_point) {
    if (code_point < 0x80U) {
        output.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800U) {
        output.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else if (code_point < 0x10000U) {
        output.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else {
        output.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    }
}

// UTF-16LE → UTF-8；无效代理项替换为 U+FFFD，避免元数据损坏被当成有效路径。
std::string utf16le_to_utf8(const std::byte* data, std::size_t byte_count) {
    std::string output;
    output.reserve(byte_count / 2);
    for (std::size_t offset = 0; offset + 1 < byte_count; offset += 2) {
        const auto unit = read_le16(data + offset);
        std::uint32_t code_point = unit;
        if (unit >= 0xD800U && unit <= 0xDBFFU) {
            if (offset + 3 < byte_count) {
                const auto low = read_le16(data + offset + 2);
                if (low >= 0xDC00U && low <= 0xDFFFU) {
                    code_point = 0x10000U +
                        ((static_cast<std::uint32_t>(unit) - 0xD800U) << 10U) +
                        (static_cast<std::uint32_t>(low) - 0xDC00U);
                    offset += 2;
                } else {
                    code_point = 0xFFFDU;
                }
            } else {
                code_point = 0xFFFDU;
            }
        } else if (unit >= 0xDC00U && unit <= 0xDFFFU) {
            code_point = 0xFFFDU;
        }
        if (code_point == 0) {
            break;
        }
        append_utf8_code_point(output, code_point);
    }
    return output;
}

// 去掉 NTFS 目标路径的 \??\ 与 \\?\ 前缀，并把分隔符规范为 '/'，
// 使导出结果在 POSIX 平台上仍是可用的相对/绝对路径。
std::string normalize_reparse_target(std::string target) {
    for (const auto* prefix : {"\\??\\", "\\\\?\\"}) {
        const std::string value(prefix);
        if (target.starts_with(value)) {
            target.erase(0, value.size());
            break;
        }
    }
    for (auto& character : target) {
        if (character == '\\') {
            character = '/';
        }
    }
    return target;
}

// NTFS $Secure:$SDS 的延迟加载缓存。首次需要时整流读入并解码；$Secure
// 不可读（损坏、权限异常）时返回空索引，节点退回仅报告 security_id。
std::map<std::uint32_t, VFSSecurityDescriptor> load_ntfs_security_map(
    TSK_FS_INFO* filesystem) {
    std::map<std::uint32_t, VFSSecurityDescriptor> index;
    if (filesystem == nullptr || !TSK_FS_TYPE_ISNTFS(filesystem->ftype)) {
        return index;
    }
    FilePtr secure(tsk_fs_file_open(filesystem, nullptr, "/$Secure"));
    if (!secure || secure->meta == nullptr) {
        return index;
    }
    const auto attribute_count = tsk_fs_file_attr_getsize(secure.get());
    const TSK_FS_ATTR* sds = nullptr;
    for (int attribute_index = 0; attribute_index < attribute_count;
         ++attribute_index) {
        const auto* attribute =
            tsk_fs_file_attr_get_idx(secure.get(), attribute_index);
        if (attribute != nullptr &&
            attribute->type == TSK_FS_ATTR_TYPE_NTFS_DATA &&
            attribute->name != nullptr &&
            std::string_view(attribute->name) == "$SDS") {
            sds = attribute;
            break;
        }
    }
    if (sds == nullptr || sds->size <= 0) {
        return index;
    }
    // $SDS 理论上可以增长到很大；256 MiB 的上限只是防御明显损坏的元数据，
    // 正常系统的 $SDS 在数 MB 量级。
    constexpr std::uint64_t kMaxSdsBytes = 256ULL * 1024ULL * 1024ULL;
    const auto total = static_cast<std::uint64_t>(sds->size);
    if (total > kMaxSdsBytes) {
        return index;
    }
    std::vector<std::byte> buffer(static_cast<std::size_t>(total));
    constexpr std::size_t kChunk = 1024ULL * 1024ULL;
    std::size_t filled = 0;
    while (filled < buffer.size()) {
        const auto request = static_cast<std::size_t>(
            std::min<std::uint64_t>(kChunk, buffer.size() - filled));
        const auto count = tsk_fs_attr_read(sds, static_cast<TSK_OFF_T>(filled),
            reinterpret_cast<char*>(buffer.data() + filled), request,
            TSK_FS_FILE_READ_FLAG_NONE);
        if (count <= 0) {
            return index;  // 读取中断：宁可返回空索引也不给下游残缺的 ACL
        }
        filled += static_cast<std::size_t>(count);
    }
    return ntfs_security::parse_sds(std::span<const std::byte>(buffer));
}

void read_ntfs_metadata(
    TSK_FS_FILE* file, VFSMetadata& metadata,
    const std::map<std::uint32_t, VFSSecurityDescriptor>* security) {
    // 分配映射（runlist）最多保留的区间数：避免重度碎片文件把列表撑爆。
    constexpr std::size_t maximum_extents = 4096;
    const auto block_size = file->fs_info != nullptr &&
            file->fs_info->block_size > 0
        ? static_cast<std::uint64_t>(file->fs_info->block_size) : 0;
    const auto attribute_count = tsk_fs_file_attr_getsize(file);
    for (int index = 0; index < attribute_count; ++index) {
        const auto* attribute = tsk_fs_file_attr_get_idx(file, index);
        if (attribute == nullptr) {
            continue;
        }
        if (attribute->type == TSK_FS_ATTR_TYPE_NTFS_SI) {
            std::array<std::byte, 72> information{};
            const auto read = tsk_fs_attr_read(attribute, 0,
                reinterpret_cast<char*>(information.data()), information.size(),
                TSK_FS_FILE_READ_FLAG_NONE);
            if (read >= 36) {
                metadata.file_attributes = read_le32(information.data() + 32);
                metadata.has_file_attributes = true;
                metadata.sparse =
                    (metadata.file_attributes & ntfs_attribute::sparse) != 0;
                metadata.compressed =
                    (metadata.file_attributes & ntfs_attribute::compressed) != 0;
                metadata.encrypted =
                    (metadata.file_attributes & ntfs_attribute::encrypted) != 0;
                metadata.reparse_point =
                    (metadata.file_attributes & ntfs_attribute::reparse_point) != 0;
            }
            if (read >= 56) {
                metadata.security_id = read_le32(information.data() + 52);
                metadata.has_security_id = true;
                if (security != nullptr) {
                    if (const auto descriptor = security->find(
                            metadata.security_id);
                        descriptor != security->end()) {
                        metadata.security = descriptor->second;
                        metadata.has_security = true;
                    }
                }
            }
            continue;
        }
        if (attribute->type == TSK_FS_ATTR_TYPE_NTFS_DATA && attribute->name != nullptr &&
            attribute->name[0] != '\0') {
            VFSStream stream;
            stream.name = attribute->name;
            if (attribute->size >= 0) {
                stream.size = static_cast<std::uint64_t>(attribute->size);
            } else {
                stream.size_valid = false;
            }
            metadata.alternate_streams.push_back(std::move(stream));
            continue;
        }
        if (attribute->type == TSK_FS_ATTR_TYPE_NTFS_DATA &&
            (attribute->name == nullptr || attribute->name[0] == '\0')) {
            // 只有未压缩、未加密的非驻留默认数据流，runlist 才与逻辑偏移一一对应。
            if ((attribute->flags & TSK_FS_ATTR_NONRES) == 0 || block_size == 0 ||
                (attribute->flags & (TSK_FS_ATTR_COMP | TSK_FS_ATTR_ENC)) != 0) {
                continue;
            }
            std::uint64_t logical_offset = 0;
            for (const auto* run = attribute->nrd.run; run != nullptr;
                 run = run->next) {
                const auto length =
                    static_cast<std::uint64_t>(run->len) * block_size;
                if (length == 0) {
                    continue;
                }
                VFSExtent extent;
                extent.offset = logical_offset;
                extent.length = length;
                extent.allocated =
                    (run->flags & TSK_FS_ATTR_RUN_FLAG_SPARSE) == 0 &&
                    run->addr != 0;
                if (!extent.allocated) {
                    metadata.sparse = true;
                }
                metadata.extents.push_back(extent);
                logical_offset += length;
                if (metadata.extents.size() >= maximum_extents) {
                    metadata.extents_truncated = true;
                    break;
                }
            }
            continue;
        }
        if (attribute->type == TSK_FS_ATTR_TYPE_NTFS_REPARSE) {
            constexpr std::uint64_t maximum_reparse_size = 64U * 1024U;
            if (attribute->size < 8 || attribute->size < 0) {
                continue;
            }
            const auto size = std::min<std::uint64_t>(
                static_cast<std::uint64_t>(attribute->size), maximum_reparse_size);
            std::vector<std::byte> buffer(static_cast<std::size_t>(size));
            const auto read = tsk_fs_attr_read(attribute, 0,
                reinterpret_cast<char*>(buffer.data()), buffer.size(),
                TSK_FS_FILE_READ_FLAG_NONE);
            if (read < 8) {
                continue;
            }
            buffer.resize(static_cast<std::size_t>(read));
            const auto tag = read_le32(buffer.data());
            metadata.reparse_tag = tag;
            metadata.has_reparse_tag = true;
            metadata.reparse_point = true;
            if (tag != kReparseTagSymlink && tag != kReparseTagMountPoint) {
                continue;
            }
            // REPARSE_DATA_BUFFER: tag(4) data_length(2) reserved(2)
            //   substitute_offset(2) substitute_length(2)
            //   print_offset(2) print_length(2) flags(4) path_buffer(..)
            constexpr std::size_t path_buffer_offset = 20;
            if (buffer.size() < path_buffer_offset) {
                continue;
            }
            const auto substitute_offset = read_le16(buffer.data() + 8);
            const auto substitute_length = read_le16(buffer.data() + 10);
            const auto start = path_buffer_offset + substitute_offset;
            if (start > buffer.size() || substitute_length > buffer.size() - start) {
                continue;
            }
            auto target = utf16le_to_utf8(buffer.data() + start, substitute_length);
            if (!target.empty()) {
                metadata.symlink_target = normalize_reparse_target(std::move(target));
            }
        }
    }
}

VFSNode make_node(TSK_FS_FILE* file, const TSK_FS_INFO* filesystem,
                  const std::string& path,
                  const std::map<std::uint32_t, VFSSecurityDescriptor>*
                      security = nullptr) {
    if (file == nullptr) {
        throw std::runtime_error("libtsk 返回了无效的文件目录项");
    }
    VFSNode node;
    if (file->name != nullptr && file->name->name != nullptr) {
        node.name = file->name->name;
    } else {
        // 通过 inode 打开的节点（例如根目录）没有名称，用路径末段兜底。
        const auto separator = path.find_last_of('/');
        node.name = separator == std::string::npos || separator + 1 >= path.size()
            ? path : path.substr(separator + 1);
    }
    node.path = path;
    if (file->meta == nullptr) {
        node.type = NodeType::other;
        return node;
    }
    switch (file->meta->type) {
    case TSK_FS_META_TYPE_REG:
        node.type = NodeType::regular_file;
        break;
    case TSK_FS_META_TYPE_DIR:
        node.type = NodeType::directory;
        break;
    case TSK_FS_META_TYPE_LNK:
        node.type = NodeType::symbolic_link;
        break;
    default:
        node.type = NodeType::other;
        break;
    }
    if (file->meta->type == TSK_FS_META_TYPE_REG) {
        node.metadata.size_valid = is_valid_file_size(filesystem, file->meta);
    }
    if (file->meta->size >= 0) {
        node.metadata.size = static_cast<std::uint64_t>(file->meta->size);
    }
    node.metadata.inode = static_cast<std::uint64_t>(file->meta->addr);
    node.metadata.hard_link_group = file->meta->nlink > 1
        ? static_cast<std::uint64_t>(file->meta->addr) : 0;
    node.metadata.link_count = file->meta->nlink;
    node.metadata.mode = static_cast<std::uint32_t>(file->meta->mode);
    node.metadata.uid = static_cast<std::uint32_t>(file->meta->uid);
    node.metadata.gid = static_cast<std::uint32_t>(file->meta->gid);
    node.metadata.has_owner = true;
    if (file->meta->type == TSK_FS_META_TYPE_LNK && file->meta->link != nullptr) {
        node.metadata.symlink_target = file->meta->link;
    }
    const bool has_nanoseconds = file->fs_info != nullptr &&
        (file->fs_info->flags & TSK_FS_INFO_FLAG_HAVE_NANOSEC) != 0;
    const auto creation_nanoseconds =
        has_nanoseconds ? file->meta->crtime_nano : 0U;
    const auto access_nanoseconds =
        has_nanoseconds ? file->meta->atime_nano : 0U;
    const auto modification_nanoseconds =
        has_nanoseconds ? file->meta->mtime_nano : 0U;
    auto timestamp = [](std::time_t seconds, std::uint32_t nanoseconds) {
        return std::chrono::system_clock::from_time_t(seconds) +
               std::chrono::nanoseconds(nanoseconds);
    };
    if (file->meta->crtime > 0) {
        node.metadata.created = timestamp(file->meta->crtime, creation_nanoseconds);
        node.metadata.has_created_time = true;
    }
    if (file->meta->atime > 0) {
        node.metadata.accessed = timestamp(file->meta->atime, access_nanoseconds);
        node.metadata.has_accessed_time = true;
    }
    if (file->meta->mtime > 0) {
        node.metadata.modified = timestamp(file->meta->mtime, modification_nanoseconds);
        node.metadata.has_modified_time = true;
    }
    if (filesystem != nullptr && TSK_FS_TYPE_ISNTFS(filesystem->ftype)) {
        read_ntfs_metadata(file, node.metadata, security);
    }
    return node;
}

} // namespace

TSK_FS_TYPE_ENUM fs_type_from_name(const std::string& name) {
    if (name == "NTFS") {
        return TSK_FS_TYPE_NTFS;
    }
    if (name == "FAT") {
        return TSK_FS_TYPE_FAT_DETECT;
    }
    if (name == "exFAT") {
        return TSK_FS_TYPE_EXFAT;
    }
    if (name == "HFS+") {
        return TSK_FS_TYPE_HFS;
    }
    if (name == "ext") {
        return TSK_FS_TYPE_EXT_DETECT;
    }
#ifdef UIFE_WITH_TSK_APFS
    if (name == "APFS") {
        return TSK_FS_TYPE_APFS;
    }
#endif
#ifdef UIFE_WITH_TSK_XFS
    if (name == "XFS") {
        return TSK_FS_TYPE_XFS;
    }
#endif
    throw std::invalid_argument("libtsk 不支持的文件系统类型: " + name);
}

struct TskFsParser::Impl {
    std::shared_ptr<IImageDriver> source_image;
    ImagePtr image;
#ifdef UIFE_WITH_TSK_APFS
    PoolPtr pool;
    std::vector<ApfsVolume> apfs_volumes;
#endif
    FsPtr filesystem;
    std::uint64_t offset{};
    std::uint64_t length{};
    // NTFS $Secure:$SDS 解码缓存：security_attempted 保证失败只尝试一次。
    std::map<std::uint32_t, VFSSecurityDescriptor> security;
    bool security_attempted{false};

    const std::map<std::uint32_t, VFSSecurityDescriptor>* security_map() {
        if (!security_attempted) {
            security_attempted = true;
            try {
                security = load_ntfs_security_map(filesystem.get());
            } catch (const std::exception&) {
                security.clear();
            }
        }
        return &security;
    }
};

TskFsParser::TskFsParser(std::string filesystem_name,
                         std::string_view password)
    : filesystem_name_(std::move(filesystem_name)) {
    if (filesystem_name_ == "APFS") {
        password_.assign(password);
    }
}

TskFsParser::~TskFsParser() {
    wipe_password(password_);
}

void TskFsParser::mount(std::shared_ptr<IImageDriver> image,
                        std::uint64_t byte_offset,
                        std::uint64_t byte_length) {
    if (!image || byte_offset > image->get_size() ||
        byte_length > image->get_size() - byte_offset) {
        throw std::invalid_argument(filesystem_name_ + " 挂载范围超出镜像边界");
    }
    const auto max_image_offset =
        static_cast<std::uint64_t>(std::numeric_limits<TSK_OFF_T>::max());
    if (byte_offset > max_image_offset || image->get_size() > max_image_offset) {
        throw std::invalid_argument(filesystem_name_ + " 镜像范围超出 libtsk 支持范围");
    }
    auto state = std::make_unique<Impl>();
    auto external_image = std::make_unique<ExternalImageInfo>();
    external_image->image = image.get();
    external_image->source_offset = byte_offset;
    external_image->source_length = byte_length;
    external_image->image_info.num_img = 1;
    external_image->image_info.page_size = 2048;
    external_image->image_info.spare_size = 64;
    state->image.reset(tsk_img_open_external(
        external_image.get(), static_cast<TSK_OFF_T>(byte_length),
        image->sector_size(), external_image_read, external_image_close,
        external_image_stat));
    if (!state->image) {
        throw std::runtime_error("libtsk 无法打开只读镜像驱动");
    }
    external_image.release();
#ifdef UIFE_WITH_TSK_APFS
    if (filesystem_name_ == "APFS") {
        state->pool.reset(tsk_pool_open_img_sing(
            state->image.get(), 0, TSK_POOL_TYPE_APFS));
        if (!state->pool) {
            const auto* error = tsk_error_get_errstr();
            throw std::runtime_error(
                "libtsk 无法读取 APFS 容器" +
                (error != nullptr ? ": " + std::string(error) : ""));
        }
        if (state->pool->tag != TSK_POOL_INFO_TAG ||
            state->pool->ctype != TSK_POOL_TYPE_APFS ||
            state->pool->block_size < 512 ||
            state->pool->block_size > 65536 ||
            (state->pool->block_size & (state->pool->block_size - 1U)) != 0 ||
            state->pool->num_blocks == 0 ||
            state->pool->get_img_info == nullptr ||
            state->pool->num_blocks >
                byte_length / state->pool->block_size ||
            state->pool->num_vols <= 0 || state->pool->num_vols > 100 ||
            state->pool->vol_list == nullptr) {
            throw std::runtime_error(
                "APFS 容器声明了无效的块几何或卷目录");
        }

        std::unordered_set<std::string> used_paths;
        auto* pool_volume = state->pool->vol_list;
        for (int index = 0; index < state->pool->num_vols; ++index) {
            if (pool_volume == nullptr ||
                pool_volume->tag != TSK_POOL_VOL_INFO_TAG ||
                pool_volume->num_blocks == 0 ||
                pool_volume->block > state->pool->num_blocks ||
                pool_volume->num_blocks >
                    state->pool->num_blocks - pool_volume->block) {
                throw std::runtime_error(
                    "APFS 卷目录中的卷范围无效，拒绝继续读取");
            }

            ApfsVolume volume;
            volume.name = pool_volume->desc != nullptr &&
                                  pool_volume->desc[0] != '\0'
                ? pool_volume->desc
                : "APFS 卷 " + std::to_string(index + 1);
            volume.path_component = encode_apfs_path_component(volume.name);
            if (!used_paths.insert(volume.path_component).second) {
                const auto original_name = volume.name;
                auto suffix = index + 1;
                do {
                    volume.name = original_name + " [卷 " +
                                  std::to_string(suffix++) + "]";
                    volume.path_component =
                        encode_apfs_path_component(volume.name);
                } while (!used_paths.insert(volume.path_component).second);
            }
            volume.encrypted =
                (pool_volume->flags & TSK_POOL_VOLUME_FLAG_ENCRYPTED) != 0;
            if (volume.encrypted && password_.empty()) {
                volume.open_error =
                    "该 APFS 卷已加密；请通过 --password-stdin 提供密码";
            } else if (volume.encrypted) {
                volume.filesystem.reset(tsk_fs_open_pool_decrypt(
                    state->pool.get(),
                    static_cast<TSK_DADDR_T>(pool_volume->block),
                    TSK_FS_TYPE_APFS, password_.c_str()));
                if (!volume.filesystem) {
                    const auto* error = tsk_error_get_errstr();
                    volume.open_error =
                        "无法解密或打开 APFS 卷（密码错误或卷格式不受支持）" +
                        (error != nullptr ? ": " + std::string(error) : "");
                }
            } else {
                volume.image.reset(state->pool->get_img_info(
                    state->pool.get(), static_cast<TSK_DADDR_T>(pool_volume->block)));
                if (!volume.image) {
                    const auto* error = tsk_error_get_errstr();
                    volume.open_error =
                        "libtsk 无法创建 APFS 卷图像句柄" +
                        (error != nullptr ? ": " + std::string(error) : "");
                } else {
                    volume.filesystem.reset(tsk_fs_open_img(
                        volume.image.get(), 0, TSK_FS_TYPE_APFS));
                    if (!volume.filesystem) {
                        const auto* error = tsk_error_get_errstr();
                        volume.open_error =
                            "libtsk 无法打开 APFS 卷" +
                            (error != nullptr ? ": " + std::string(error) : "");
                    }
                }
            }
            if (volume.filesystem &&
                (volume.filesystem->block_size != state->pool->block_size ||
                 volume.filesystem->block_count == 0 ||
                 static_cast<std::uint64_t>(volume.filesystem->block_count) >
                     state->pool->num_blocks ||
                 static_cast<std::uint64_t>(volume.filesystem->last_block_act) >=
                     state->pool->num_blocks)) {
                volume.filesystem.reset();
                volume.open_error =
                    "APFS 卷文件系统几何超出容器范围，拒绝读取";
            }
            state->apfs_volumes.push_back(std::move(volume));
            pool_volume = pool_volume->next;
        }
        if (pool_volume != nullptr) {
            throw std::runtime_error("APFS 卷目录长度与声明数量不一致");
        }
        if (!password_.empty()) {
            const auto failed_encrypted_volume = std::find_if(
                state->apfs_volumes.begin(), state->apfs_volumes.end(),
                [](const ApfsVolume& volume) {
                    return volume.encrypted && !volume.filesystem;
                });
            if (failed_encrypted_volume != state->apfs_volumes.end()) {
                throw std::runtime_error(
                    failed_encrypted_volume->open_error);
            }
        }
        state->source_image = std::move(image);
        state->offset = byte_offset;
        state->length = byte_length;
        impl_ = std::move(state);
        wipe_password(password_);
        return;
    }
#endif
    state->filesystem.reset(tsk_fs_open_img(state->image.get(),
        0, fs_type_from_name(filesystem_name_)));
    if (!state->filesystem) {
        const auto* error = tsk_error_get_errstr();
        throw std::runtime_error(
            "libtsk 无法挂载该 " + filesystem_name_ + " 文件系统" +
            (error != nullptr ? ": " + std::string(error) : ""));
    }
    if (state->filesystem->block_size == 0 ||
        state->filesystem->block_count == 0) {
        throw std::runtime_error(filesystem_name_ +
                                 " 文件系统声明了无效的块大小或块数量");
    }
    const auto block_count =
        static_cast<std::uint64_t>(state->filesystem->block_count);
    const auto block_size =
        static_cast<std::uint64_t>(state->filesystem->block_size);
    if (block_count > std::numeric_limits<std::uint64_t>::max() / block_size) {
        throw std::runtime_error(filesystem_name_ + " 文件系统容量计算溢出");
    }
    if (block_count * block_size > byte_length) {
        throw std::runtime_error(filesystem_name_ +
                                 " 声明的文件系统容量超出镜像/分区边界");
    }
    const auto last_block =
        static_cast<std::uint64_t>(state->filesystem->last_block_act);
    if (filesystem_name_ != "APFS" && last_block >= block_count) {
        throw std::runtime_error(filesystem_name_ +
                                 " 最后可用块超出声明的文件系统范围");
    }
    state->source_image = std::move(image);
    state->offset = byte_offset;
    state->length = byte_length;
    impl_ = std::move(state);
    wipe_password(password_);
}

std::vector<VFSNode> TskFsParser::list_dir(const std::string& input_path) const {
    if (!impl_) {
        throw std::runtime_error(filesystem_name_ + " 尚未挂载");
    }
    const auto path = normalized_path(input_path);
#ifdef UIFE_WITH_TSK_APFS
    if (filesystem_name_ == "APFS") {
        if (path == "/") {
            std::vector<VFSNode> volumes;
            volumes.reserve(impl_->apfs_volumes.size());
            for (const auto& volume : impl_->apfs_volumes) {
                auto node = make_synthetic_directory(
                    volume.name, "/" + volume.path_component);
                if (!volume.filesystem) {
                    node.type = NodeType::other;
                    node.name += volume.encrypted ? " [加密]" : " [不可读取]";
                }
                volumes.push_back(std::move(node));
            }
            return volumes;
        }
        const auto separator = path.find('/', 1);
        const auto component = path.substr(
            1, separator == std::string::npos ? std::string::npos : separator - 1);
        const auto volume = std::find_if(
            impl_->apfs_volumes.begin(), impl_->apfs_volumes.end(),
            [&component](const ApfsVolume& candidate) {
                return candidate.path_component == component;
            });
        if (volume == impl_->apfs_volumes.end()) {
            throw std::runtime_error("APFS 虚拟卷路径不存在: " + path);
        }
        if (!volume->filesystem) {
            throw std::runtime_error(volume->open_error);
        }
        const auto internal_path = separator == std::string::npos
            ? "/" : path.substr(separator);
        DirectoryPtr directory(tsk_fs_dir_open(
            volume->filesystem.get(), internal_path.c_str()));
        if (!directory) {
            FilePtr single(tsk_fs_file_open(volume->filesystem.get(), nullptr,
                                            internal_path.c_str()));
            if (!single) {
                throw std::runtime_error(
                    "无法打开 APFS 卷目录: " + internal_path);
            }
            std::vector<VFSNode> entries;
            entries.push_back(make_node(
                single.get(), volume->filesystem.get(),
                "/" + volume->path_component + internal_path));
            return entries;
        }
        std::vector<VFSNode> entries;
        const auto count = tsk_fs_dir_getsize(directory.get());
        for (std::size_t index = 0; index < count; ++index) {
            FilePtr file(tsk_fs_dir_get(directory.get(), index));
            if (!file || file->name == nullptr || file->name->name == nullptr) {
                continue;
            }
            const std::string name = file->name->name;
            if (name == "." || name == ".." ||
                (file->name->flags & TSK_FS_NAME_FLAG_UNALLOC) != 0) {
                continue;
            }
            const auto child_path = internal_path == "/"
                ? "/" + name : internal_path + "/" + name;
            entries.push_back(make_node(
                file.get(), volume->filesystem.get(),
                "/" + volume->path_component + child_path));
        }
        return entries;
    }
#else
    if (!impl_->filesystem) {
        throw std::runtime_error(filesystem_name_ + " 尚未挂载");
    }
#endif
    DirectoryPtr directory(tsk_fs_dir_open(impl_->filesystem.get(), path.c_str()));
    if (!directory) {
        // 路径可能指向单个文件而非目录：回退为按路径打开文件节点。
        FilePtr single(tsk_fs_file_open(impl_->filesystem.get(), nullptr,
                                        path.c_str()));
        if (!single) {
            throw std::runtime_error(
                "无法打开 " + filesystem_name_ + " 目录: " + path);
        }
        std::vector<VFSNode> entries;
        entries.push_back(make_node(single.get(), impl_->filesystem.get(),
                                    path, impl_->security_map()));
        return entries;
    }
    std::vector<VFSNode> entries;
    const auto count = tsk_fs_dir_getsize(directory.get());
    for (std::size_t index = 0; index < count; ++index) {
        FilePtr file(tsk_fs_dir_get(directory.get(), index));
        if (!file || file->name == nullptr || file->name->name == nullptr) {
            continue;
        }
        const std::string name = file->name->name;
        if (name == "." || name == ".." ||
            (file->name->flags & TSK_FS_NAME_FLAG_UNALLOC) != 0) {
            continue;
        }
        const auto child_path = path == "/" ? "/" + name : path + "/" + name;
        entries.push_back(make_node(file.get(), impl_->filesystem.get(),
                                    child_path, impl_->security_map()));
    }
    return entries;
}

VFSNode TskFsParser::get_metadata(const std::string& input_path) const {
    if (!impl_) {
        throw std::runtime_error(filesystem_name_ + " 尚未挂载");
    }
    const auto path = normalized_path(input_path);
#ifdef UIFE_WITH_TSK_APFS
    if (filesystem_name_ == "APFS") {
        if (path == "/") {
            return make_synthetic_directory("/", "/");
        }
        const auto separator = path.find('/', 1);
        const auto component = path.substr(
            1, separator == std::string::npos ? std::string::npos : separator - 1);
        const auto volume = std::find_if(
            impl_->apfs_volumes.begin(), impl_->apfs_volumes.end(),
            [&component](const ApfsVolume& candidate) {
                return candidate.path_component == component;
            });
        if (volume == impl_->apfs_volumes.end()) {
            throw std::runtime_error("APFS 虚拟卷路径不存在: " + path);
        }
        if (!volume->filesystem) {
            throw std::runtime_error(volume->open_error);
        }
        const auto internal_path = separator == std::string::npos
            ? "/" : path.substr(separator);
        FilePtr file(internal_path == "/"
            ? tsk_fs_file_open_meta(volume->filesystem.get(), nullptr,
                                    volume->filesystem->root_inum)
            : tsk_fs_file_open(volume->filesystem.get(), nullptr,
                               internal_path.c_str()));
        if (!file) {
            throw std::runtime_error("无法读取 APFS 卷元数据: " + path);
        }
        auto node = make_node(file.get(), volume->filesystem.get(), path);
        if (internal_path == "/") {
            node.name = volume->name;
        }
        return node;
    }
#else
    if (!impl_->filesystem) {
        throw std::runtime_error(filesystem_name_ + " 尚未挂载");
    }
#endif
    FilePtr file(path == "/"
        ? tsk_fs_file_open_meta(impl_->filesystem.get(), nullptr,
                                impl_->filesystem->root_inum)
        : tsk_fs_file_open(impl_->filesystem.get(), nullptr, path.c_str()));
    if (!file) {
        throw std::runtime_error("无法打开 " + filesystem_name_ + " 文件: " + path);
    }
    auto node = make_node(file.get(), impl_->filesystem.get(), path,
                          impl_->security_map());
    if (path == "/") {
        node.name = "/";
    }
    return node;
}

std::size_t TskFsParser::read_file(const std::string& input_path,
                                    std::uint64_t offset,
                                    std::span<std::byte> destination) const {
    if (!impl_) {
        throw std::runtime_error(filesystem_name_ + " 尚未挂载");
    }
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<TSK_OFF_T>::max())) {
        throw std::out_of_range(filesystem_name_ + " 文件偏移超出 libtsk 支持范围");
    }
    auto* filesystem = impl_->filesystem.get();
#ifdef UIFE_WITH_TSK_APFS
    auto path = normalized_path(input_path);
    if (filesystem_name_ == "APFS") {
        const auto separator = path.find('/', 1);
        const auto component = path.substr(
            1, separator == std::string::npos ? std::string::npos : separator - 1);
        const auto volume = std::find_if(
            impl_->apfs_volumes.begin(), impl_->apfs_volumes.end(),
            [&component](const ApfsVolume& candidate) {
                return candidate.path_component == component;
            });
        if (volume == impl_->apfs_volumes.end()) {
            throw std::runtime_error("APFS 虚拟卷路径不存在: " + path);
        }
        if (!volume->filesystem) {
            throw std::runtime_error(volume->open_error);
        }
        filesystem = volume->filesystem.get();
        path = separator == std::string::npos ? "/" : path.substr(separator);
    }
#else
    if (!impl_->filesystem) {
        throw std::runtime_error(filesystem_name_ + " 尚未挂载");
    }
    const auto path = normalized_path(input_path);
#endif
    FilePtr file(tsk_fs_file_open(filesystem, nullptr, path.c_str()));
    if (!file || file->meta == nullptr || file->meta->type != TSK_FS_META_TYPE_REG) {
        throw std::runtime_error(filesystem_name_ + " 路径不是可读取的普通文件: " + input_path);
    }
    if (!is_valid_file_size(filesystem, file->meta)) {
        throw std::runtime_error(filesystem_name_ + " 文件大小元数据异常，拒绝读取: " +
                                 input_path);
    }
    const auto file_size = static_cast<std::uint64_t>(file->meta->size);
    if (offset > file_size) {
        throw std::out_of_range(filesystem_name_ + " 文件读取偏移超出文件大小");
    }
    const auto request = static_cast<std::size_t>(
        std::min<std::uint64_t>(destination.size(), file_size - offset));
    if (request == 0) {
        return 0;
    }
    const auto count = tsk_fs_file_read(file.get(), static_cast<TSK_OFF_T>(offset),
        reinterpret_cast<char*>(destination.data()), request,
        TSK_FS_FILE_READ_FLAG_NONE);
    if (count < 0) {
        throw std::runtime_error("libtsk 读取 " + filesystem_name_ +
                                 " 文件内容失败: " + input_path);
    }
    return static_cast<std::size_t>(count);
}

std::size_t TskFsParser::read_stream(const std::string& input_path,
                                     const std::string& stream,
                                     std::uint64_t offset,
                                     std::span<std::byte> destination) const {
    if (!impl_) {
        throw std::runtime_error(filesystem_name_ + " 尚未挂载");
    }
#ifdef UIFE_WITH_TSK_APFS
    if (filesystem_name_ == "APFS" && !impl_->filesystem) {
        throw std::runtime_error("APFS 后端当前不提供命名数据流读取");
    }
#endif
    if (!impl_->filesystem) {
        throw std::runtime_error(filesystem_name_ + " 尚未挂载");
    }
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<TSK_OFF_T>::max())) {
        throw std::out_of_range(filesystem_name_ + " 数据流偏移超出 libtsk 支持范围");
    }
    const auto path = normalized_path(input_path);
    FilePtr file(tsk_fs_file_open(impl_->filesystem.get(), nullptr, path.c_str()));
    if (!file || file->meta == nullptr) {
        throw std::runtime_error("无法打开 " + filesystem_name_ + " 文件: " + path);
    }
    const TSK_FS_ATTR* target = nullptr;
    const auto attribute_count = tsk_fs_file_attr_getsize(file.get());
    for (int index = 0; index < attribute_count; ++index) {
        const auto* attribute = tsk_fs_file_attr_get_idx(file.get(), index);
        if (attribute != nullptr &&
            attribute->type == TSK_FS_ATTR_TYPE_NTFS_DATA &&
            attribute->name != nullptr && stream == attribute->name) {
            target = attribute;
            break;
        }
    }
    if (target == nullptr) {
        throw std::runtime_error("文件不包含该命名数据流: " + path + ":" + stream);
    }
    if (target->size < 0) {
        throw std::runtime_error("命名数据流大小元数据异常，拒绝读取: " +
                                 path + ":" + stream);
    }
    const auto stream_size = static_cast<std::uint64_t>(target->size);
    if (stream_size > impl_->length) {
        throw std::runtime_error("命名数据流声明大小超出镜像范围，拒绝读取: " +
                                 path + ":" + stream);
    }
    if (offset > stream_size) {
        throw std::out_of_range("命名数据流读取偏移超出流大小");
    }
    const auto request = static_cast<std::size_t>(
        std::min<std::uint64_t>(destination.size(), stream_size - offset));
    if (request == 0) {
        return 0;
    }
    const auto count = tsk_fs_attr_read(target, static_cast<TSK_OFF_T>(offset),
        reinterpret_cast<char*>(destination.data()), request,
        TSK_FS_FILE_READ_FLAG_NONE);
    if (count < 0) {
        throw std::runtime_error("libtsk 读取命名数据流失败: " + path + ":" + stream);
    }
    return static_cast<std::size_t>(count);
}

std::string_view TskFsParser::filesystem_name() const noexcept {
#ifdef UIFE_WITH_LIBTSK
    if (impl_ && impl_->filesystem) {
        switch (impl_->filesystem->ftype) {
        case TSK_FS_TYPE_EXT2: return "ext2";
        case TSK_FS_TYPE_EXT3: return "ext3";
        case TSK_FS_TYPE_EXT4: return "ext4";
#ifdef UIFE_WITH_TSK_XFS
        case TSK_FS_TYPE_XFS: return "XFS";
#endif
        default: break;
        }
    }
#ifdef UIFE_WITH_TSK_APFS
    if (impl_ && !impl_->apfs_volumes.empty()) {
        return "APFS";
    }
#endif
#endif
    return filesystem_name_;
}

} // namespace uife

#else

namespace uife {

struct TskFsParser::Impl {};

TskFsParser::TskFsParser(std::string filesystem_name,
                         std::string_view password)
    : filesystem_name_(std::move(filesystem_name)) {
    if (filesystem_name_ == "APFS") {
        password_.assign(password);
    }
}

TskFsParser::~TskFsParser() {
    wipe_password(password_);
}

void TskFsParser::mount(std::shared_ptr<IImageDriver>, std::uint64_t, std::uint64_t) {
    throw std::runtime_error("当前构建未启用 libtsk 后端");
}

std::vector<VFSNode> TskFsParser::list_dir(const std::string&) const {
    throw std::runtime_error("当前构建未启用 libtsk 后端");
}

VFSNode TskFsParser::get_metadata(const std::string&) const {
    throw std::runtime_error("当前构建未启用 libtsk 后端");
}

std::size_t TskFsParser::read_file(const std::string&, std::uint64_t,
                                   std::span<std::byte>) const {
    throw std::runtime_error("当前构建未启用 libtsk 后端");
}

std::size_t TskFsParser::read_stream(const std::string&, const std::string&,
                                     std::uint64_t,
                                     std::span<std::byte>) const {
    throw std::runtime_error("当前构建未启用 libtsk 后端");
}

std::string_view TskFsParser::filesystem_name() const noexcept {
    return filesystem_name_;
}

} // namespace uife

#endif
