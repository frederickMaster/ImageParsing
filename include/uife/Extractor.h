#pragma once

#include "uife/vfs/VFS.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

namespace uife {

// 超出资源预算时抛出。单独一个类型，是为了让 CLI 给出独立退出码（5），
// 批处理脚本据此区分"镜像/后端有问题"和"预算给得太小"。
class ResourceLimitError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// 资源上限。0 都表示不限。
//
// 为什么需要它：目录遍历目前会把整棵树的节点累积在内存里再开始写盘，
// 复杂度是 O(节点数)。遇到上千万条目的人工构造镜像（或损坏的族表导致
// 成环）时，进程会在 OOM 里被杀掉，现场既没有错误信息也没有退出码。
// 有了预算，行为变成"要么在预算内跑完，要么明确失败"。
struct ResourceLimits {
    std::size_t max_entries{};           // 目录树节点总数上限
    std::chrono::seconds time_limit{};   // 墙钟预算，从遍历开始计
};

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
    // 遍历/复制过程中生效的资源上限。超限直接中止整次导出：部分证据比明确的
    // 失败更危险，调用方无法分辨"少了几个文件"和"本来就这些文件"。
    ResourceLimits limits{};
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
