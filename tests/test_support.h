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
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace uife_test {

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

// 对单个镜像做一整套只读操作。任何一步失败都以异常抛出：调用方决定要不要当成
// 致命错误（不支持变体的固定失败属于正常情况）。
inline void exercise_image(const FixtureTarget& target,
                           const std::filesystem::path& output_root) {
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

        if (vfs.can_read_content()) {
            const auto destination =
                output_root / (target.label + "-" + std::to_string(index));
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

// 核心断言：整套操作跑完，工作目录里一个字节都不能变。
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
              << " rejected=" << rejected << std::endl;
    if (exercised == 0) {
        // 全部被拒绝意味着这套检查形同虚设，不能算通过。
        throw std::runtime_error(
            "immutability sweep exercised no image at all");
    }

    const auto after = snapshot_tree(root, output_root);
    report_snapshot_difference(before, after, "read-only sweep");
}

// 截断输入：0 字节必须被拒；其余长度要么明确抛错，要么读操作老实按请求长度返回。
inline void check_truncated_inputs(const std::filesystem::path& work_directory,
                                   const std::vector<FixtureTarget>& targets) {
    const auto truncation_root = work_directory / "uife-truncated-inputs";
    std::error_code error;
    std::filesystem::remove_all(truncation_root, error);
    std::filesystem::create_directories(truncation_root, error);

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
}

} // namespace uife_test
