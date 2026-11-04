#include "uife/vfs/VFS.h"

#include <stdexcept>

namespace uife {

VFS::VFS(const IFSParser& parser) noexcept : parser_(parser) {}

std::vector<VFSNode> VFS::list_dir(const std::string& path) const {
    return parser_.list_dir(normalize_path(path));
}

VFSNode VFS::stat(const std::string& path) const {
    const auto normalized = normalize_path(path);
    auto node = parser_.get_metadata(normalized);
    if (node.path.empty()) {
        node.path = normalized;
    } else {
        node.path = normalize_path(node.path);
    }
    return node;
}

std::size_t VFS::read(const std::string& path, std::uint64_t offset,
                      std::span<std::byte> destination) const {
    return parser_.read_file(normalize_path(path), offset, destination);
}

std::size_t VFS::read_stream(const std::string& path, const std::string& stream,
                             std::uint64_t offset,
                             std::span<std::byte> destination) const {
    if (stream.empty() || stream.size() > 255 ||
        stream.find('/') != std::string::npos ||
        stream.find('\\') != std::string::npos ||
        stream.find('\0') != std::string::npos) {
        throw std::invalid_argument("数据流名称非法");
    }
    return parser_.read_stream(normalize_path(path), stream, offset, destination);
}

std::string_view VFS::filesystem_name() const noexcept {
    return parser_.filesystem_name();
}

bool VFS::can_read_content() const noexcept {
    return parser_.can_read_content();
}

std::string VFS::normalize_path(const std::string& path) {
    if (path.empty() || path.front() != '/') {
        throw std::invalid_argument("VFS 路径必须是以 '/' 开始的 POSIX 风格路径");
    }
    std::string normalized;
    normalized.reserve(path.size());
    bool previous_separator = false;
    for (const char character : path) {
        if (character == '\\' || character == '\0') {
            throw std::invalid_argument("VFS 路径包含非法字符");
        }
        if (character == '/') {
            if (!previous_separator) {
                normalized.push_back('/');
            }
            previous_separator = true;
        } else {
            normalized.push_back(character);
            previous_separator = false;
        }
    }
    if (normalized.size() > 1 && normalized.back() == '/') {
        normalized.pop_back();
    }
    std::size_t start = 1;
    while (start < normalized.size()) {
        const auto end = normalized.find('/', start);
        const auto part = normalized.substr(start, end == std::string::npos
            ? std::string::npos : end - start);
        if (part.empty() || part == "." || part == ".." ||
            part.find(':') != std::string::npos) {
            throw std::invalid_argument("VFS 路径包含不安全的路径分量");
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return normalized;
}

} // namespace uife
