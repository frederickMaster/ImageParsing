#include "uife/drivers/fs/Iso9660FsParser.h"

#include "uife/drivers/fs/Iso9660.h"

#include <stdexcept>
#include <utility>

namespace uife {

Iso9660FsParser::Iso9660FsParser() = default;
Iso9660FsParser::~Iso9660FsParser() = default;

void Iso9660FsParser::mount(std::shared_ptr<IImageDriver> image,
                            std::uint64_t byte_offset,
                            std::uint64_t byte_length) {
    if (!image || byte_offset > image->get_size() ||
        byte_length > image->get_size() - byte_offset) {
        throw std::invalid_argument("ISO9660 挂载范围超出镜像边界");
    }
    iso_ = std::make_unique<iso9660::Iso9660>(
        image->get_size(),
        [image](
            std::uint64_t offset, std::span<std::byte> destination) {
            return image->read_at(offset, destination);
        },
        byte_offset,
        byte_length);
}

std::vector<VFSNode> Iso9660FsParser::list_dir(const std::string& path) const {
    if (!iso_) {
        throw std::runtime_error("ISO9660 尚未挂载");
    }
    std::vector<VFSNode> nodes;
    for (const auto& entry : iso_->list(path)) {
        VFSNode node;
        node.name = entry.name;
        node.path = path == "/" ? "/" + entry.name : path + "/" + entry.name;
        node.type = entry.is_directory ? NodeType::directory
            : entry.is_symlink ? NodeType::symbolic_link
                               : NodeType::regular_file;
        node.metadata.size = entry.size;
        node.metadata.inode = entry.extent_lba;
        node.metadata.mode = entry.is_directory ? 0555U : 0444U;
        if (entry.has_posix_metadata) {
            node.metadata.mode = entry.mode;
            node.metadata.uid = entry.uid;
            node.metadata.gid = entry.gid;
            node.metadata.has_owner = true;
            node.metadata.link_count = entry.link_count;
            node.metadata.hard_link_group = entry.hard_link_group;
        }
        node.metadata.modified = entry.modified;
        node.metadata.has_modified_time = entry.has_modified_time;
        node.metadata.symlink_target = entry.symlink_target;
        nodes.push_back(std::move(node));
    }
    return nodes;
}

VFSNode Iso9660FsParser::get_metadata(const std::string& path) const {
    if (!iso_) {
        throw std::runtime_error("ISO9660 尚未挂载");
    }
    const auto entry = iso_->stat(path);
    VFSNode node;
    node.name = path == "/" ? "/" : entry.name;
    node.path = path;
    node.type = entry.is_directory ? NodeType::directory
        : entry.is_symlink ? NodeType::symbolic_link
                           : NodeType::regular_file;
    node.metadata.size = entry.size;
    node.metadata.inode = entry.extent_lba;
    node.metadata.mode = entry.is_directory ? 0555U : 0444U;
    if (entry.has_posix_metadata) {
        node.metadata.mode = entry.mode;
        node.metadata.uid = entry.uid;
        node.metadata.gid = entry.gid;
        node.metadata.has_owner = true;
        node.metadata.link_count = entry.link_count;
        node.metadata.hard_link_group = entry.hard_link_group;
    }
    node.metadata.modified = entry.modified;
    node.metadata.has_modified_time = entry.has_modified_time;
    node.metadata.symlink_target = entry.symlink_target;
    return node;
}

std::size_t Iso9660FsParser::read_file(const std::string& path,
                                       std::uint64_t offset,
                                       std::span<std::byte> destination) const {
    if (!iso_) {
        throw std::runtime_error("ISO9660 尚未挂载");
    }
    return iso_->read_file(path, offset,
        reinterpret_cast<char*>(destination.data()), destination.size());
}

std::string_view Iso9660FsParser::filesystem_name() const noexcept {
    return "ISO9660";
}

} // namespace uife
