#pragma once

#include "uife/vfs/VFS.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace uife {

// 一项未能还原的元数据。opt_in 表示"用户没有打开可选还原开关"（目前只有
// --restore-acl），这种情况属于既定默认行为而不是保真度缺陷：NTFS 镜像里
// 几乎每个对象都有安全描述符，逐条当成失败上报会把报告刷成几万行，
// 也掩盖了真正丢失的属性。
struct MetadataNote {
    std::string path;
    std::string field;
    std::string reason;
    bool opt_in{false};
};

struct ExtractOptions {
    std::filesystem::path output_directory;
    std::function<void(std::uint64_t, std::uint64_t)> progress;
    // 是否把源对象的 ACL 写回导出文件（目前仅 Windows 能表达 NT ACL）。
    // 默认关闭：还原出来的 DACL 可能连当前账户都读不了（例如归属 SYSTEM 的
    // 系统目录），现场取证时更常见的做法是先拿到内容，再按需还原权限。
    bool restore_acl{false};
    // 某项元数据无法忠实还原时的回调（平台不支持、权限不足、语义无法表达等）。
    // 只影响属性还原，导出内容本身已经写完并通过哈希校验。不设置时静默忽略；
    // CLI 用它汇总成后备元数据报告，避免"看起来成功但属性丢了"。
    std::function<void(const MetadataNote&)> metadata_note;
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
