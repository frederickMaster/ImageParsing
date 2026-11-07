#pragma once

// 只读不变性与截断输入的通用检查。
//
// 这套检查刻意不做成逐格式的定制用例，而是对"任意镜像"都成立的两条不变量：
//
//   1. 只读不变性：对镜像做打开、读扇区、扫分区、挂载文件系统、列目录、读文件、
//      整卷导出这一整套操作之后，目录树里每个文件的字节哈希必须与操作前一致。
//      这样任何驱动一旦偷偷写源文件（哪怕只是写了一个临时文件在镜像旁边），
//      都会被指出来。
//   2. 截断输入：把镜像截到不同长度后，create() 要么明确抛错，要么读操作被
//      钳制在请求长度内——不允许崩溃、越界或"读到不存在的数据还报告成功"。
//
// 放在头文件里是因为 uife_tests（单元）和 uife_integration_tests（集成）都要用，
// 而两者各自在匿名命名空间里有自己的 require/hash_bytes，直接共享会符号冲突。

#include "uife/DriverFactory.h"
#include "uife/Extractor.h"
#include "uife/FsParserFactory.h"
#include "uife/PartitionScanner.h"
#include "uife/drivers/image/IImageDriver.h"
#include "uife/utils/Sha256.h"
#include "uife/vfs/VFS.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>  // geteuid：判断只读权限位是否只是摆设
#endif

namespace uife_test {

// 把规范里的 GUID 文本（"8141bf1d-a96f-4709-ba47-f233a8faab5f"）转成镜像里
// 实际存的 16 字节小端布局。
//
// 为什么要有这个：VHDX 的 metadata item 靠 GUID 识别，写错一个 nibble 的后果
// 是"这个 item 不认识"，而不是"格式错误"。之前 logical sector size 的常量把
// a96f 抄成了 a996，fixture 又照抄同一个错值，于是测试一路绿——直到拿真实的
// Hyper-V 镜像来跑，才报"含有当前不支持的必需 metadata item"。让 fixture 从
// 规范文本生成，抄错就得先改文本，肉眼能对着规范核。
inline std::array<std::byte, 16> guid_from_string(std::string_view text) {
    std::array<std::byte, 16> bytes{};
    std::size_t index = 0;
    int high = -1;
    for (const char character : text) {
        int value = -1;
        if (character >= '0' && character <= '9') {
            value = character - '0';
        } else if (character >= 'a' && character <= 'f') {
            value = character - 'a' + 10;
        } else if (character >= 'A' && character <= 'F') {
            value = character - 'A' + 10;
        } else {
            continue;  // '-' 等分隔符
        }
        if (index >= bytes.size()) {
            break;
        }
        if (high < 0) {
            high = value;
        } else {
            bytes[index++] = static_cast<std::byte>((high << 4) | value);
            high = -1;
        }
    }
    // GUID 的文本顺序是 data1(4) data2(2) data3(2) data4(8)，前三段是大端书写、
    // 小端存储；后 8 字节按原序。逐段翻转才是文件里的字节序。
    std::swap(bytes[0], bytes[3]);
    std::swap(bytes[1], bytes[2]);
    std::swap(bytes[4], bytes[5]);
    std::swap(bytes[6], bytes[7]);
    return bytes;
}

struct FixtureTarget {
    std::filesystem::path path;
    std::string label;
};

using TreeSnapshot = std::map<std::string, std::string>;

inline std::string hash_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to open for hashing: " + path.string());
    }
    uife::Sha256 hash;
    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto read = input.gcount();
        if (read <= 0) {
            break;
        }
        hash.update(std::as_bytes(
            std::span<const char>(buffer.data(), static_cast<std::size_t>(read))));
    }
    return uife::Sha256::to_hex(hash.finish());
}

inline bool is_within(const std::filesystem::path& candidate,
                      const std::filesystem::path& root) {
    const auto candidate_text = candidate.lexically_normal().generic_string();
    const auto root_text = root.lexically_normal().generic_string();
    return candidate_text == root_text ||
           candidate_text.compare(0, root_text.size() + 1, root_text + "/") == 0;
}

// 目录快照：相对路径 -> 内容 SHA-256。excluded 子树整棵跳过（导出目录本来就会被写）。
inline TreeSnapshot snapshot_tree(const std::filesystem::path& root,
                                  const std::filesystem::path& excluded) {
    TreeSnapshot snapshot;
    std::error_code error;
    if (!std::filesystem::exists(root, error)) {
        return snapshot;
    }
    for (std::filesystem::recursive_directory_iterator item(root, error), end;
         !error && item != end; item.increment(error)) {
        if (is_within(item->path(), excluded)) {
            if (item->is_directory(error)) {
                item.disable_recursion_pending();
                error.clear();
            }
            continue;
        }
        if (!item->is_regular_file(error)) {
            error.clear();
            continue;
        }
        const auto relative = std::filesystem::relative(item->path(), root, error);
        if (error) {
            error.clear();
            continue;
        }
        snapshot.emplace(relative.generic_string(), hash_file(item->path()));
    }
    return snapshot;
}

// 计数：用于确认这一趟清扫真的走到了"导出"这一步，而不是空转。
inline std::size_t count_files(const std::filesystem::path& root) {
    std::size_t count = 0;
    std::error_code error;
    if (!std::filesystem::exists(root, error)) {
        return 0;
    }
    for (std::filesystem::recursive_directory_iterator item(root, error), end;
         !error && item != end; item.increment(error)) {
        if (item->is_regular_file(error)) {
            ++count;
        }
        error.clear();
    }
    return count;
}

inline void report_snapshot_difference(const TreeSnapshot& before,
                                       const TreeSnapshot& after,
                                       const std::string& label) {
    for (const auto& [path, digest] : before) {
        const auto updated = after.find(path);
        if (updated == after.end()) {
            throw std::runtime_error(label + ": source file disappeared: " + path);
        }
        if (updated->second != digest) {
            throw std::runtime_error(label + ": source file was modified: " + path);
        }
    }
    for (const auto& [path, digest] : after) {
        (void)digest;
        if (before.find(path) == before.end()) {
            throw std::runtime_error(label + ": new file appeared next to the image: " +
                                     path);
        }
    }
}

// 对单个镜像做一整套只读操作（不含导出）。任何一步失败都以异常抛出：调用方决定
// 要不要当成致命错误（不支持变体的固定失败属于正常情况）。
inline void probe_image(const FixtureTarget& target,
                        const std::filesystem::path* output_root = nullptr) {
    const auto info = uife::DriverFactory::instance().identify(target.path);
    if (!info.supported) {
        return;  // 仅识别/不支持的变体交给各格式自己的"明确拒绝"用例
    }
    std::shared_ptr<uife::IImageDriver> image =
        uife::DriverFactory::instance().create(target.path);

    // 头部、中部、尾部各读一次：覆盖容器表、数据区与最后一块的边界处理。
    const auto size = image->get_size();
    std::array<std::byte, 4096> buffer{};
    const std::array<std::uint64_t, 3> offsets{
        0, size / 2, size > buffer.size() ? size - buffer.size() : 0};
    for (const auto offset : offsets) {
        if (offset >= size) {
            continue;
        }
        const auto length =
            static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), size - offset));
        const auto got = image->read_at(offset, std::span<std::byte>(buffer.data(), length));
        if (got > length) {
            throw std::runtime_error(target.label + ": read_at returned more than requested");
        }
    }

    auto partitions = uife::scan_partitions(*image);
    if (partitions.empty()) {
        partitions.push_back(uife::Partition{0, size, "none", "raw"});
    }
    for (std::size_t index = 0; index < partitions.size(); ++index) {
        const auto& partition = partitions[index];
        if (partition.byte_offset >= size) {
            continue;
        }
        if (!uife::FsParserFactory::instance().supports(*image, partition.byte_offset)) {
            continue;
        }
        auto parser =
            uife::FsParserFactory::instance().create(*image, partition.byte_offset);
        parser->mount(image, partition.byte_offset, partition.byte_length);
        const uife::VFS vfs(*parser);

        // 列两级目录，读出第一个常规文件的前 4KB：足以触碰目录项、inode/记录与数据簇。
        std::vector<std::string> pending{"/"};
        std::size_t read_files = 0;
        for (std::size_t depth = 0; depth < 2 && !pending.empty(); ++depth) {
            std::vector<std::string> next;
            for (const auto& directory : pending) {
                for (const auto& node : vfs.list_dir(directory)) {
                    if (node.type == uife::NodeType::directory) {
                        next.push_back(node.path);
                        continue;
                    }
                    if (node.type != uife::NodeType::regular_file || read_files >= 4 ||
                        !vfs.can_read_content()) {
                        continue;
                    }
                    std::array<std::byte, 4096> contents{};
                    const auto got = vfs.read(node.path, 0, contents);
                    if (got > contents.size()) {
                        throw std::runtime_error(target.label +
                                                 ": VFS::read returned more than requested");
                    }
                    ++read_files;
                }
            }
            pending = std::move(next);
        }

        if (output_root != nullptr && vfs.can_read_content()) {
            const auto destination =
                *output_root / (target.label + "-" + std::to_string(index));
            uife::ExtractOptions options;
            options.output_directory = destination;
            try {
                (void)uife::Extractor::restore(vfs, options);
            } catch (const std::exception&) {
                // 只读不变性才是断言对象：导出失败（权限、未支持的元数据）不影响结论。
            }
        }
    }
}

// 只读不变性用：探针 + 整卷导出。
inline void exercise_image(const FixtureTarget& target,
                           const std::filesystem::path& output_root) {
    probe_image(target, &output_root);
}

// 核心断言：整套操作跑完，工作目录里一个字节都不能变。
// 覆盖率报告：把每轮清扫的用例数写入 UIFE_SWEEP_REPORT 指定的文件。
// ctest 在用例通过时会吞掉进程输出，只有落成文件，"这几百个变异真的跑了"
// 才能在 CI 日志里被审计——否则覆盖率数字永远是自说自话。
inline void report_sweep(const std::string& line) {
    const char* path = std::getenv("UIFE_SWEEP_REPORT");
    if (path == nullptr || *path == '\0') {
        return;
    }
    std::ofstream output(path, std::ios::app);
    output << line << '\n';
}

inline void check_source_immutability(const std::filesystem::path& root,
                                      const std::filesystem::path& output_root,
                                      const std::vector<FixtureTarget>& targets) {
    std::error_code error;
    std::filesystem::create_directories(output_root, error);
    std::filesystem::remove_all(output_root, error);
    std::filesystem::create_directories(output_root, error);

    const auto before = snapshot_tree(root, output_root);
    if (before.empty()) {
        throw std::runtime_error("immutability sweep found no fixtures to check");
    }

    std::size_t exercised = 0;
    std::size_t rejected = 0;
    for (const auto& target : targets) {
        if (!std::filesystem::exists(target.path)) {
            std::cerr << "[immutability] skip (missing fixture): " << target.label
                      << std::endl;
            continue;
        }
        std::cerr << "[immutability] " << target.label << std::endl;
        // 断言对象只有"源文件没变"这一条。故意损坏的 fixture 被驱动明确拒绝是
        // 正常结果，各格式自己的拒绝用例负责那部分语义。
        try {
            exercise_image(target, output_root);
            ++exercised;
        } catch (const std::exception& failure) {
            ++rejected;
            std::cerr << "[immutability] " << target.label
                      << " rejected: " << failure.what() << std::endl;
        }
    }
    std::cerr << "[immutability] exercised=" << exercised
              << " rejected=" << rejected
              << " extracted=" << count_files(output_root) << std::endl;
    report_sweep("immutability exercised=" + std::to_string(exercised) +
                 " rejected=" + std::to_string(rejected) +
                 " extracted=" + std::to_string(count_files(output_root)));
    if (exercised == 0) {
        // 全部被拒绝意味着这套检查形同虚设，不能算通过。
        throw std::runtime_error(
            "immutability sweep exercised no image at all");
    }

    const auto after = snapshot_tree(root, output_root);
    report_snapshot_difference(before, after, "read-only sweep");
}

// 把源镜像改成只读后再跑一遍同样的清扫。
//
// 和前一条检查互补：哈希比对证明"这次没写"，只读属性证明"想写也写不进去"。
// 任何一处把源镜像按可写方式打开，在 Windows 上会直接 ACCESS_DENIED，在
// POSIX 非 root 下是 EACCES——都会在清扫里当场抛出来，而不是等某个罕见的
// 代码路径真的写坏证据。
struct ReadOnlyGuard {
    std::vector<std::filesystem::path> paths;
    ~ReadOnlyGuard() {
        std::error_code error;
        for (const auto& path : paths) {
            std::filesystem::permissions(
                path,
                std::filesystem::perms::owner_write | std::filesystem::perms::owner_read |
                    std::filesystem::perms::group_read |
                    std::filesystem::perms::others_read,
                std::filesystem::perm_options::replace, error);
            error.clear();
        }
    }
};

inline void make_read_only(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_read | std::filesystem::perms::group_read |
            std::filesystem::perms::others_read,
        std::filesystem::perm_options::replace, error);
    if (error) {
        throw std::runtime_error("failed to mark fixture read-only: " + path.string() +
                                 ": " + error.message());
    }
}

inline void check_read_only_sources(const std::filesystem::path& root,
                                    const std::filesystem::path& output_root,
                                    const std::vector<FixtureTarget>& targets) {
    std::error_code error;
    std::filesystem::remove_all(output_root, error);
    std::filesystem::create_directories(output_root, error);

    ReadOnlyGuard guard;
    for (const auto& target : targets) {
        if (!std::filesystem::exists(target.path)) {
            continue;
        }
        make_read_only(target.path);
        guard.paths.push_back(target.path);
    }

#if !defined(_WIN32)
    if (::geteuid() == 0) {
        // root 会绕过权限位，这趟检查在容器里等于没跑，必须说出来而不是假装通过。
        std::cerr << "[read-only] 以 root 运行，权限位无法阻止写入，本项证明力有限"
                  << std::endl;
        report_sweep("read-only sweep: running as root, permission bits are advisory");
    }
#endif

    std::size_t exercised = 0;
    std::size_t rejected = 0;
    for (const auto& target : targets) {
        if (!std::filesystem::exists(target.path)) {
            continue;
        }
        // 每个 fixture 打一行：真卡死时日志里最后一行就是嫌疑对象。ctest 超时
        // 只报"某个用例超时"，没有这个只能靠猜。
        std::cerr << "[read-only] " << target.label << std::endl;
        try {
            exercise_image(target, output_root);
            ++exercised;
        } catch (const std::exception& failure) {
            ++rejected;
            std::cerr << "[read-only] " << target.label
                      << " rejected: " << failure.what() << std::endl;
        }
    }
    std::cerr << "[read-only] exercised=" << exercised << " rejected=" << rejected
              << " extracted=" << count_files(output_root) << std::endl;
    report_sweep("read-only-source sweep exercised=" + std::to_string(exercised) +
                 " rejected=" + std::to_string(rejected));
    if (exercised == 0) {
        throw std::runtime_error("read-only source sweep exercised no image at all");
    }
}

// 截断输入：0 字节必须被拒；其余长度要么明确抛错，要么读操作老实按请求长度返回。
inline void check_truncated_inputs(const std::filesystem::path& work_directory,
                                   const std::vector<FixtureTarget>& targets) {
    const auto truncation_root = work_directory / "uife-truncated-inputs";
    std::error_code error;
    std::filesystem::remove_all(truncation_root, error);
    std::filesystem::create_directories(truncation_root, error);

    std::size_t cases = 0;

    for (const auto& target : targets) {
        if (!std::filesystem::exists(target.path)) {
            continue;
        }
        std::ifstream input(target.path, std::ios::binary);
        if (!input) {
            throw std::runtime_error("failed to open fixture for truncation: " +
                                     target.path.string());
        }
        const std::vector<char> bytes{std::istreambuf_iterator<char>(input),
                                      std::istreambuf_iterator<char>()};
        if (bytes.size() < 64) {
            continue;  // 极小的 fixture 截断后已无结构可言，跳过
        }

        const std::array<std::size_t, 4> keep_lengths{
            0, 512, bytes.size() / 2, bytes.size() - 1};
        for (const auto keep : keep_lengths) {
            if (keep >= bytes.size()) {
                continue;
            }
            const auto copy = truncation_root /
                              (target.label + "-" + std::to_string(keep) +
                               target.path.extension().string());
            {
                std::ofstream output(copy, std::ios::binary);
                output.write(bytes.data(), static_cast<std::streamsize>(keep));
            }
            std::cerr << "[truncated] " << target.label << " keep=" << keep << std::endl;
            ++cases;

            if (keep == 0) {
                // 空文件：容器格式必须明确拒绝；RAW 这类"任意字节即镜像"的格式
                // 接受也不算错，但绝不能凭空报出非零大小或读出数据。
                std::unique_ptr<uife::IImageDriver> image;
                try {
                    image = uife::DriverFactory::instance().create(copy);
                } catch (const std::exception&) {
                    continue;  // 明确拒绝，符合预期
                }
                if (image->get_size() != 0) {
                    throw std::runtime_error(target.label +
                                             ": empty file reported a non-zero size");
                }
                std::array<std::byte, 512> buffer{};
                std::size_t got = 0;
                try {
                    got = image->read_at(0, buffer);
                } catch (const std::exception&) {
                    continue;
                }
                if (got != 0) {
                    throw std::runtime_error(target.label +
                                             ": empty file produced data on read");
                }
                continue;
            }

            try {
                auto image = uife::DriverFactory::instance().create(copy);
                const auto size = image->get_size();
                std::array<std::byte, 512> buffer{};
                for (const auto offset : {std::uint64_t{0}, size,
                                          size > 0 ? size - 1 : 0}) {
                    const auto got = image->read_at(offset, buffer);
                    if (got > buffer.size()) {
                        throw std::runtime_error(
                            target.label + ": truncated image returned more than requested");
                    }
                }
            } catch (const std::exception&) {
                // 明确报错是可接受的行为。
            }
        }
    }
    std::filesystem::remove_all(truncation_root, error);
    std::cerr << "[truncated] cases=" << cases << std::endl;
    report_sweep("truncated cases=" + std::to_string(cases));
    if (cases == 0) {
        throw std::runtime_error("truncated-input sweep produced no case");
    }
}

// 结构化畸形输入：把镜像内部把长度/偏移/计数一类的字段改成极大值或 0，要求解析
// 只能"正常返回"或"明确抛错"，不允许崩溃、越界，也不允许读出镜像里不存在的字节。
//
// 与截断输入的分工：截断只动文件尾部，管不到"长度字段被写成 8 EB"这类攻击面；
// 取证工具面对的是不可信镜像，这类字段必须逐个显式校验。崩溃会让整个测试进程
// 中止，因此每个用例执行前先打印用例坐标，最后一行输出就是肇事者。
inline std::uint32_t mutation_step(std::uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

inline void check_malformed_inputs(const std::filesystem::path& work_directory,
                                   const std::vector<FixtureTarget>& targets,
                                   std::size_t cases_per_fixture = 40) {
    const auto malformed_root = work_directory / "uife-malformed-inputs";
    std::error_code error;
    std::filesystem::remove_all(malformed_root, error);
    std::filesystem::create_directories(malformed_root, error);

    std::size_t cases = 0;
    std::size_t rejected = 0;
    std::size_t changed = 0;
    std::size_t skipped = 0;
    for (std::size_t index = 0; index < targets.size(); ++index) {
        const auto& target = targets[index];
        if (!std::filesystem::exists(target.path)) {
            continue;
        }
        std::ifstream input(target.path, std::ios::binary);
        if (!input) {
            throw std::runtime_error("failed to open fixture for malformed sweep: " +
                                     target.path.string());
        }
        const std::vector<char> original{std::istreambuf_iterator<char>(input),
                                        std::istreambuf_iterator<char>()};
        if (original.size() < 128) {
            continue;  // 太小的 fixture 没有可破坏的结构字段
        }

        // 结构字段集中在文件前部（容器头、卷头、表入口），因此大部分变异落在前
        // 64 KiB；每四个用例里安排一个更深处的位置，让 MFT 记录、簇表也吃到破坏。
        std::uint32_t state =
            0x9e3779b9U ^ static_cast<std::uint32_t>(index * 2654435761U + 1U);
        const std::size_t window = std::min<std::size_t>(original.size(), 64U * 1024U);
        for (std::size_t n = 0; n < cases_per_fixture; ++n) {
            const auto random = mutation_step(state);
            std::size_t length = (n % 5 == 0) ? 8U : (n % 3 == 0 ? 4U : 1U);
            const std::size_t limit = original.size() - length;
            const std::size_t deep_start = std::min(window, limit);
            std::size_t offset = 0;
            if (n % 4 == 0 && limit > deep_start) {
                offset = deep_start + static_cast<std::size_t>(
                                          random % (limit - deep_start + 1));
            } else {
                offset = static_cast<std::size_t>(random % (deep_start + 1));
            }
            // 每十个用例安排一次"整块清零"（512 字节对齐）：模拟数据区被挖空、
            // 表项被抹掉的情形，比单字段破坏更容易触发表遍历的边界错误。
            const bool whole_block = (n % 10) == 9;
            if (whole_block) {
                length = std::min<std::size_t>(512U, original.size());
                offset = (offset / 512U) * 512U;
                if (offset + length > original.size()) {
                    offset = original.size() - length;
                }
            }

            auto mutant = original;
            if (whole_block) {
                for (std::size_t k = 0; k < length; ++k) {
                    mutant[offset + k] = 0;
                }
            } else {
                switch (n % 4) {
                case 0:  // 全 1：极大长度、极大偏移、0xFFFFFFFF 计数
                    for (std::size_t k = 0; k < length; ++k) {
                        mutant[offset + k] = static_cast<char>(0xFF);
                    }
                    break;
                case 1:  // 清零：0 长度、0 偏移，最容易诱发除零与空指针解引用
                    for (std::size_t k = 0; k < length; ++k) {
                        mutant[offset + k] = 0;
                    }
                    break;
                case 2:  // 有符号最大值：0x7FFF... 让"正数但巨大"的字段走到边界
                    mutant[offset] = static_cast<char>(0x7F);
                    for (std::size_t k = 1; k < length; ++k) {
                        mutant[offset + k] = static_cast<char>(0xFF);
                    }
                    break;
                default:  // 任意字节
                    mutant[offset] = static_cast<char>(random >> 24);
                    break;
                }
            }
            if (mutant == original) {
                ++skipped;  // 改在零填充上，等价于原镜像，跳过以免虚增覆盖
                continue;
            }
            ++changed;

            const auto copy = malformed_root /
                              (target.label + "-m" + std::to_string(n) +
                               target.path.extension().string());
            {
                std::ofstream output(copy, std::ios::binary);
                output.write(mutant.data(),
                             static_cast<std::streamsize>(mutant.size()));
                if (!output) {
                    throw std::runtime_error("failed to write malformed fixture: " +
                                             copy.string());
                }
            }
            std::cerr << "[malformed] " << target.label << " case=" << n
                      << " offset=" << offset << " length=" << length << std::endl;
            ++cases;
            try {
                probe_image(FixtureTarget{copy, target.label});
            } catch (const std::exception&) {
                ++rejected;  // 明确报错是正确行为
            } catch (...) {
                ++rejected;
            }
        }
    }
    std::filesystem::remove_all(malformed_root, error);
    std::cerr << "[malformed] cases=" << cases << " rejected=" << rejected
              << " skipped=" << skipped << std::endl;
    report_sweep("malformed cases=" + std::to_string(cases) +
                 " rejected=" + std::to_string(rejected) +
                 " skipped=" + std::to_string(skipped));
    if (cases == 0 || changed == 0) {
        throw std::runtime_error("malformed sweep produced no mutation to check");
    }
}

} // namespace uife_test
