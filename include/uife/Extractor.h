#pragma once

#include "uife/vfs/VFS.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace uife {

struct ExtractOptions {
    std::filesystem::path output_directory;
    std::function<void(std::uint64_t, std::uint64_t)> progress;
};

struct ExtractedFile {
    std::string path;    // 源文件系统内的 VFS 路径（默认数据流）
    std::string stream;  // 命名数据流名称；空表示默认数据流
    std::string output;  // 相对输出目录的导出路径（UTF-8，'/' 分隔）
    std::string sha256;
    std::uint64_t size{};
};

class Extractor {
public:
    [[nodiscard]] static std::vector<ExtractedFile> extract(
        const VFS& filesystem,
        const std::string& source_path,
        const ExtractOptions& options);
    [[nodiscard]] static std::vector<ExtractedFile> restore(
        const VFS& filesystem,
        const ExtractOptions& options);
};

} // namespace uife
