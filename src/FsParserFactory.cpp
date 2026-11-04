#include "uife/FsParserFactory.h"

#include "uife/drivers/fs/ApfsFsParser.h"
#include "uife/drivers/fs/BtrfsSendFsParser.h"
#include "uife/drivers/fs/TskCliFsParser.h"
#include "uife/drivers/fs/Iso9660FsParser.h"
#include "uife/drivers/fs/RefsFsParser.h"
#include "uife/drivers/fs/TskFsParser.h"
#include "uife/drivers/fs/WimFsParser.h"

#include <array>
#include <stdexcept>

namespace uife {
namespace {

bool is_ntfs(const IImageDriver& image, std::uint64_t byte_offset) {
    if (byte_offset > image.get_size() ||
        image.sector_size() > image.get_size() - byte_offset) {
        return false;
    }
    std::vector<std::byte> sector(image.sector_size());
    if (image.read_at(byte_offset, sector) != sector.size()) {
        return false;
    }
    constexpr std::string_view signature = "NTFS    ";
    if (sector.size() < 13) {
        return false;
    }
    for (std::size_t index = 0; index < signature.size(); ++index) {
        if (std::to_integer<unsigned char>(sector[index + 3]) !=
            static_cast<unsigned char>(signature[index])) {
            return false;
        }
    }
    const auto bytes_per_sector =
        std::to_integer<unsigned char>(sector[11]) |
        (std::to_integer<unsigned char>(sector[12]) << 8U);
    return bytes_per_sector >= 512 && bytes_per_sector <= 4096 &&
           (bytes_per_sector & (bytes_per_sector - 1U)) == 0;
}

bool is_refs(const IImageDriver& image, std::uint64_t byte_offset) {
    constexpr std::size_t refs_signature_offset = 3;
    constexpr std::string_view refs_signature = "ReFS";
    if (byte_offset > image.get_size() ||
        512 > image.get_size() - byte_offset) {
        return false;
    }
    std::array<std::byte, 512> boot_sector{};
    if (image.read_at(byte_offset, boot_sector) != boot_sector.size()) {
        return false;
    }
    for (std::size_t index = 0; index < refs_signature.size(); ++index) {
        if (std::to_integer<unsigned char>(
                boot_sector[refs_signature_offset + index]) !=
            static_cast<unsigned char>(refs_signature[index])) {
            return false;
        }
    }
    // 注意：ReFS 的 512 字节 VBR 不填充 NTFS 风格的 BPB 字段（偏移 11-12 常为 0），
    // 因此这里只以 OEM 签名判定；扇区大小在实际解析卷头时再取。
    return true;
}

bool is_iso9660(const IImageDriver& image, std::uint64_t byte_offset) {
    constexpr std::uint64_t descriptor_offset = 16U * 2048U;
    constexpr std::size_t signature_size = 7;
    if (byte_offset > image.get_size() ||
        descriptor_offset > image.get_size() - byte_offset ||
        signature_size > image.get_size() - byte_offset - descriptor_offset) {
        return false;
    }
    std::array<std::byte, signature_size> signature{};
    if (image.read_at(byte_offset + descriptor_offset, signature) != signature.size()) {
        return false;
    }
    return signature[0] == std::byte{1} &&
           signature[1] == std::byte{'C'} &&
           signature[2] == std::byte{'D'} &&
           signature[3] == std::byte{'0'} &&
           signature[4] == std::byte{'0'} &&
           signature[5] == std::byte{'1'} &&
           signature[6] == std::byte{1};
}

std::string fat_filesystem_name(const IImageDriver& image,
                                std::uint64_t byte_offset) {
    if (byte_offset > image.get_size() ||
        512 > image.get_size() - byte_offset) {
        return {};
    }
    std::array<std::byte, 512> boot_sector{};
    if (image.read_at(byte_offset, boot_sector) != boot_sector.size() ||
        boot_sector[510] != std::byte{0x55} ||
        boot_sector[511] != std::byte{0xaa}) {
        return {};
    }
    constexpr std::string_view exfat_signature = "EXFAT   ";
    bool is_exfat = true;
    for (std::size_t index = 0; index < exfat_signature.size(); ++index) {
        if (std::to_integer<unsigned char>(boot_sector[index + 3]) !=
            static_cast<unsigned char>(exfat_signature[index])) {
            is_exfat = false;
            break;
        }
    }
    if (is_exfat) {
        const auto sector_shift =
            std::to_integer<unsigned char>(boot_sector[108]);
        const auto cluster_shift =
            std::to_integer<unsigned char>(boot_sector[109]);
        return sector_shift >= 9 && sector_shift <= 12 &&
                       cluster_shift <= 25 - sector_shift
            ? "exFAT" : std::string{};
    }

    const auto bytes_per_sector =
        std::to_integer<unsigned char>(boot_sector[11]) |
        (std::to_integer<unsigned char>(boot_sector[12]) << 8U);
    const auto sectors_per_cluster =
        std::to_integer<unsigned char>(boot_sector[13]);
    const auto reserved_sectors =
        std::to_integer<unsigned char>(boot_sector[14]) |
        (std::to_integer<unsigned char>(boot_sector[15]) << 8U);
    const auto fat_count = std::to_integer<unsigned char>(boot_sector[16]);
    const auto root_entries =
        std::to_integer<unsigned char>(boot_sector[17]) |
        (std::to_integer<unsigned char>(boot_sector[18]) << 8U);
    const auto total_sectors_16 =
        std::to_integer<unsigned char>(boot_sector[19]) |
        (std::to_integer<unsigned char>(boot_sector[20]) << 8U);
    const auto total_sectors_32 =
        std::to_integer<unsigned char>(boot_sector[32]) |
        (std::to_integer<unsigned char>(boot_sector[33]) << 8U) |
        (std::to_integer<unsigned char>(boot_sector[34]) << 16U) |
        (std::to_integer<unsigned char>(boot_sector[35]) << 24U);
    const auto fat_size_16 =
        std::to_integer<unsigned char>(boot_sector[22]) |
        (std::to_integer<unsigned char>(boot_sector[23]) << 8U);
    const auto fat_size_32 =
        std::to_integer<unsigned char>(boot_sector[36]) |
        (std::to_integer<unsigned char>(boot_sector[37]) << 8U) |
        (std::to_integer<unsigned char>(boot_sector[38]) << 16U) |
        (std::to_integer<unsigned char>(boot_sector[39]) << 24U);
    const auto total_sectors = total_sectors_16 != 0
        ? total_sectors_16 : total_sectors_32;
    const auto fat_size = fat_size_16 != 0 ? fat_size_16 : fat_size_32;
    if (bytes_per_sector < 512 || bytes_per_sector > 4096 ||
        (bytes_per_sector & (bytes_per_sector - 1U)) != 0 ||
        sectors_per_cluster == 0 || sectors_per_cluster > 128 ||
        (sectors_per_cluster & (sectors_per_cluster - 1U)) != 0 ||
        reserved_sectors == 0 || fat_count == 0 || fat_count > 2 ||
        total_sectors == 0 || fat_size == 0) {
        return {};
    }
    if ((fat_size_16 == 0) != (root_entries == 0)) {
        return {};
    }
    return "FAT";
}

bool is_wim(const IImageDriver& image, std::uint64_t byte_offset) {
    constexpr std::array<std::byte, 5> signature{
        std::byte{'M'}, std::byte{'S'}, std::byte{'W'},
        std::byte{'I'}, std::byte{'M'}};
    if (byte_offset > image.get_size() ||
        signature.size() > image.get_size() - byte_offset) {
        return false;
    }
    std::array<std::byte, signature.size()> header{};
    return image.read_at(byte_offset, header) == header.size() &&
           std::equal(signature.begin(), signature.end(), header.begin());
}

bool is_btrfs_send_stream(const IImageDriver& image,
                          std::uint64_t byte_offset) {
    constexpr std::array<std::byte, 13> magic{
        std::byte{'b'}, std::byte{'t'}, std::byte{'r'}, std::byte{'f'},
        std::byte{'s'}, std::byte{'-'}, std::byte{'s'}, std::byte{'t'},
        std::byte{'r'}, std::byte{'e'}, std::byte{'a'}, std::byte{'m'},
        std::byte{0}};
    if (byte_offset > image.get_size() ||
        magic.size() > image.get_size() - byte_offset) {
        return false;
    }
    std::array<std::byte, magic.size()> header{};
    return image.read_at(byte_offset, header) == header.size() &&
           std::equal(magic.begin(), magic.end(), header.begin());
}

bool is_hfs_plus(const IImageDriver& image, std::uint64_t byte_offset) {
    constexpr std::uint64_t volume_header_offset = 1024;
    constexpr std::size_t volume_header_size = 512;
    if (byte_offset > image.get_size() ||
        volume_header_offset > image.get_size() - byte_offset ||
        volume_header_size > image.get_size() - byte_offset - volume_header_offset) {
        return false;
    }
    std::array<std::byte, volume_header_size> header{};
    if (image.read_at(byte_offset + volume_header_offset, header) != header.size()) {
        return false;
    }
    const auto signature = (std::to_integer<unsigned char>(header[0]) << 8U) |
                           std::to_integer<unsigned char>(header[1]);
    const auto version = (std::to_integer<unsigned char>(header[2]) << 8U) |
                         std::to_integer<unsigned char>(header[3]);
    if (!((signature == 0x482bU && version == 4U) ||
          (signature == 0x4858U && version == 5U))) {
        return false;
    }
    auto read_be32 = [&header](std::size_t offset) {
        return (std::to_integer<std::uint32_t>(header[offset]) << 24U) |
               (std::to_integer<std::uint32_t>(header[offset + 1]) << 16U) |
               (std::to_integer<std::uint32_t>(header[offset + 2]) << 8U) |
               std::to_integer<std::uint32_t>(header[offset + 3]);
    };
    const auto block_size = read_be32(40);
    const auto total_blocks = read_be32(44);
    const auto free_blocks = read_be32(48);
    if (block_size < 512 || block_size > 1024U * 1024U ||
        (block_size & (block_size - 1U)) != 0 ||
        total_blocks == 0 || free_blocks > total_blocks) {
        return false;
    }
    const auto volume_size =
        static_cast<std::uint64_t>(block_size) * total_blocks;
    return volume_size <= image.get_size() - byte_offset;
}

bool is_xfs(const IImageDriver& image, std::uint64_t byte_offset) {
    constexpr std::size_t superblock_size = 512;
    if (byte_offset > image.get_size() ||
        superblock_size > image.get_size() - byte_offset) {
        return false;
    }
    std::array<std::byte, superblock_size> superblock{};
    if (image.read_at(byte_offset, superblock) != superblock.size() ||
        superblock[0] != std::byte{'X'} || superblock[1] != std::byte{'F'} ||
        superblock[2] != std::byte{'S'} || superblock[3] != std::byte{'B'}) {
        return false;
    }
    const auto read_be32 = [&superblock](std::size_t offset) {
        return (std::to_integer<std::uint32_t>(superblock[offset]) << 24U) |
               (std::to_integer<std::uint32_t>(superblock[offset + 1]) << 16U) |
               (std::to_integer<std::uint32_t>(superblock[offset + 2]) << 8U) |
               std::to_integer<std::uint32_t>(superblock[offset + 3]);
    };
    const auto read_be64 = [&read_be32](std::size_t offset) {
        return (static_cast<std::uint64_t>(read_be32(offset)) << 32U) |
               read_be32(offset + 4);
    };
    const auto block_size = read_be32(4);
    const auto data_blocks = read_be64(8);
    const auto sector_size = static_cast<std::uint32_t>(
        (std::to_integer<unsigned char>(superblock[102]) << 8U) |
        std::to_integer<unsigned char>(superblock[103]));
    const auto inode_size = static_cast<std::uint32_t>(
        (std::to_integer<unsigned char>(superblock[104]) << 8U) |
        std::to_integer<unsigned char>(superblock[105]));
    if (block_size < 512 || block_size > 65536 ||
        (block_size & (block_size - 1U)) != 0 || data_blocks == 0 ||
        sector_size < 512 || sector_size > 4096 ||
        (sector_size & (sector_size - 1U)) != 0 ||
        inode_size < 256 || inode_size > block_size ||
        (inode_size & (inode_size - 1U)) != 0 ||
        data_blocks > (image.get_size() - byte_offset) / block_size) {
        return false;
    }
    return true;
}

bool is_apfs_container(const IImageDriver& image, std::uint64_t byte_offset) {
    constexpr std::uint64_t nx_superblock_header_size = 48;
    if (byte_offset > image.get_size() ||
        nx_superblock_header_size > image.get_size() - byte_offset) {
        return false;
    }
    std::array<std::byte, nx_superblock_header_size> header{};
    if (image.read_at(byte_offset, header) != header.size() ||
        header[32] != std::byte{'N'} || header[33] != std::byte{'X'} ||
        header[34] != std::byte{'S'} || header[35] != std::byte{'B'}) {
        return false;
    }
    const auto read_le32 = [&header](std::size_t offset) {
        return std::to_integer<std::uint32_t>(header[offset]) |
               (std::to_integer<std::uint32_t>(header[offset + 1]) << 8U) |
               (std::to_integer<std::uint32_t>(header[offset + 2]) << 16U) |
               (std::to_integer<std::uint32_t>(header[offset + 3]) << 24U);
    };
    const auto read_le64 = [&read_le32](std::size_t offset) {
        return static_cast<std::uint64_t>(read_le32(offset)) |
               (static_cast<std::uint64_t>(read_le32(offset + 4)) << 32U);
    };
    const auto block_size = read_le32(36);
    const auto block_count = read_le64(40);
    if (block_size < 512 || block_size > 65536 ||
        (block_size & (block_size - 1U)) != 0 || block_count == 0) {
        return false;
    }
    return block_count <=
           (image.get_size() - byte_offset) / block_size;
}

std::string ext_filesystem_name(const IImageDriver& image, std::uint64_t byte_offset) {
    constexpr std::uint64_t superblock_offset = 1024;
    constexpr std::size_t superblock_size = 1024;
    if (byte_offset > image.get_size() ||
        superblock_offset > image.get_size() - byte_offset ||
        superblock_size > image.get_size() - byte_offset - superblock_offset) {
        return {};
    }
    std::array<std::byte, superblock_size> superblock{};
    if (image.read_at(byte_offset + superblock_offset, superblock) != superblock.size()) {
        return {};
    }
    if (superblock[56] != std::byte{0x53} || superblock[57] != std::byte{0xef}) {
        return {};
    }
    const auto log_block_size =
        std::to_integer<unsigned char>(superblock[24]) |
        (std::to_integer<unsigned char>(superblock[25]) << 8U);
    if (log_block_size > 6) {
        return {};
    }
    const auto compat =
        std::to_integer<unsigned char>(superblock[92]) |
        (std::to_integer<unsigned char>(superblock[93]) << 8U) |
        (std::to_integer<unsigned char>(superblock[94]) << 16U) |
        (std::to_integer<unsigned char>(superblock[95]) << 24U);
    const auto incompat =
        std::to_integer<unsigned char>(superblock[96]) |
        (std::to_integer<unsigned char>(superblock[97]) << 8U) |
        (std::to_integer<unsigned char>(superblock[98]) << 16U) |
        (std::to_integer<unsigned char>(superblock[99]) << 24U);
    if ((incompat & 0x40U) != 0) {
        return "ext4";
    }
    return (compat & 0x04U) != 0 ? "ext3" : "ext2";
}

} // namespace

FsParserFactory& FsParserFactory::instance() {
    static FsParserFactory factory;
    return factory;
}

FsParserFactory::FsParserFactory() {
    register_parser("Btrfs send stream",
        [](const IImageDriver& image, std::uint64_t offset) {
            return offset == 0 && is_btrfs_send_stream(image, offset);
        },
        [](std::string_view) {
            return std::make_unique<BtrfsSendFsParser>();
        });
    register_parser("ISO9660",
        [](const IImageDriver& image, std::uint64_t offset) {
            return is_iso9660(image, offset);
        },
        [](std::string_view) { return std::make_unique<Iso9660FsParser>(); });
#ifdef UIFE_WITH_LIBWIM
    register_parser("WIM/ESD",
        [](const IImageDriver& image, std::uint64_t offset) {
            return offset == 0 && is_wim(image, offset);
        },
        [](std::string_view) { return std::make_unique<WimFsParser>(); });
#endif
#ifdef UIFE_WITH_LIBFSREFS
    register_parser("ReFS",
        [](const IImageDriver& image, std::uint64_t offset) {
            return is_refs(image, offset);
        },
        [](std::string_view) { return std::make_unique<RefsFsParser>(); });
#endif
#ifdef UIFE_WITH_LIBFSAPFS
    register_parser("APFS",
        [](const IImageDriver& image, std::uint64_t offset) {
            return is_apfs_container(image, offset);
        },
        [](std::string_view password) {
            return std::make_unique<ApfsFsParser>(std::string(password));
        });
#endif
#ifdef UIFE_WITH_LIBTSK
    register_parser("NTFS", [](const IImageDriver& image, std::uint64_t offset) {
        return is_ntfs(image, offset);
    },
                    [](std::string_view password) {
                        return std::make_unique<TskFsParser>("NTFS", password);
                    });
    register_parser("FAT",
        [](const IImageDriver& image, std::uint64_t offset) {
            return fat_filesystem_name(image, offset) == "FAT";
        },
        [](std::string_view password) {
            return std::make_unique<TskFsParser>("FAT", password);
        });
    register_parser("exFAT",
        [](const IImageDriver& image, std::uint64_t offset) {
            return fat_filesystem_name(image, offset) == "exFAT";
        },
        [](std::string_view password) {
            return std::make_unique<TskFsParser>("exFAT", password);
        });
    register_parser("HFS+",
        [](const IImageDriver& image, std::uint64_t offset) {
            return is_hfs_plus(image, offset);
        },
        [](std::string_view password) {
            return std::make_unique<TskFsParser>("HFS+", password);
        });
    register_parser("ext2/3/4",
        [](const IImageDriver& image, std::uint64_t offset) {
            return !ext_filesystem_name(image, offset).empty();
        },
        [](std::string_view password) {
            return std::make_unique<TskFsParser>("ext", password);
        });
#ifdef UIFE_WITH_TSK_XFS
    register_parser("XFS",
        [](const IImageDriver& image, std::uint64_t offset) {
            return is_xfs(image, offset);
        },
        [](std::string_view password) {
            return std::make_unique<TskFsParser>("XFS", password);
        });
#endif
#ifdef UIFE_WITH_TSK_APFS
    register_parser("APFS",
        [](const IImageDriver& image, std::uint64_t offset) {
            return is_apfs_container(image, offset);
        },
        [](std::string_view password) {
            return std::make_unique<TskFsParser>("APFS", password);
        });
#endif
#elif defined(_WIN32)
    if (TskCliFsParser::available()) {
        register_parser("NTFS",
            [](const IImageDriver& image, std::uint64_t offset) {
                return image.format_name() == "RAW" && is_ntfs(image, offset);
            },
            [](std::string_view) {
                return std::make_unique<TskCliFsParser>("NTFS");
            });
        register_parser("FAT",
            [](const IImageDriver& image, std::uint64_t offset) {
                return image.format_name() == "RAW" &&
                       fat_filesystem_name(image, offset) == "FAT";
            },
            [](std::string_view) {
                return std::make_unique<TskCliFsParser>("FAT");
            });
        register_parser("exFAT",
            [](const IImageDriver& image, std::uint64_t offset) {
                return image.format_name() == "RAW" &&
                       fat_filesystem_name(image, offset) == "exFAT";
            },
            [](std::string_view) {
                return std::make_unique<TskCliFsParser>("exFAT");
            });
        register_parser("HFS+",
            [](const IImageDriver& image, std::uint64_t offset) {
                return image.format_name() == "RAW" && is_hfs_plus(image, offset);
            },
            [](std::string_view) {
                return std::make_unique<TskCliFsParser>("HFS+");
            });
        register_parser("ext2/3/4",
            [](const IImageDriver& image, std::uint64_t offset) {
                return image.format_name() == "RAW" &&
                       !ext_filesystem_name(image, offset).empty();
            },
            [](std::string_view) {
                return std::make_unique<TskCliFsParser>("ext4");
            });
    }
#endif
}

void FsParserFactory::register_parser(std::string name, Probe probe, Creator creator) {
    registrations_.push_back({std::move(name), std::move(probe), std::move(creator)});
}

bool FsParserFactory::supports(const IImageDriver& image,
                               std::uint64_t byte_offset) const {
    for (const auto& registration : registrations_) {
        if (registration.probe(image, byte_offset)) {
            return true;
        }
    }
    return false;
}

std::string FsParserFactory::identify(const IImageDriver& image,
                                      std::uint64_t byte_offset) const {
    if (byte_offset > image.get_size()) {
        return "未知";
    }
    if (is_btrfs_send_stream(image, byte_offset)) {
        return "Btrfs send stream";
    }
    if (image.sector_size() > image.get_size() - byte_offset) {
        return "未知";
    }
    if (is_wim(image, byte_offset)) {
#ifdef UIFE_WITH_LIBWIM
        return "WIM/ESD";
#else
        return "WIM/ESD（已识别；当前构建未启用 wimlib）";
#endif
    }
    if (is_ntfs(image, byte_offset)) {
#ifdef UIFE_WITH_LIBTSK
        return "NTFS";
#else
#ifdef _WIN32
        if (image.format_name() != "RAW") {
            return "NTFS（已识别；Sleuth Kit CLI 后备仅支持 RAW）";
        }
        return TskCliFsParser::available()
            ? "NTFS" : "NTFS（已识别；未找到 Sleuth Kit 命令行工具）";
#else
        if (image.format_name() != "RAW") {
            return "NTFS（已识别；当前构建仅支持 RAW 后端）";
        }
        return "NTFS（已识别；未构建 libtsk 后端）";
#endif
#endif
    }
    if (is_refs(image, byte_offset)) {
        for (const auto& registration : registrations_) {
            if (registration.name == "ReFS" &&
                registration.probe(image, byte_offset)) {
                return "ReFS（可浏览目录与元数据；libfsrefs 不提供文件内容读取）";
            }
        }
        return "ReFS（已识别；当前未接入 ReFS 解析后端）";
    }
    if (is_apfs_container(image, byte_offset)) {
        for (const auto& registration : registrations_) {
            if (registration.name == "APFS" &&
                registration.probe(image, byte_offset)) {
                return "APFS";
            }
        }
#ifdef UIFE_WITH_LIBTSK
        return "APFS（已识别；当前 libtsk 未提供可用的 APFS pool API）";
#else
        return "APFS（已识别；未构建 libfsapfs 后端）";
#endif
    }
    if (is_iso9660(image, byte_offset)) {
        return "ISO9660";
    }
    const auto fat_name = fat_filesystem_name(image, byte_offset);
    if (!fat_name.empty()) {
        for (const auto& registration : registrations_) {
            if (registration.probe(image, byte_offset)) {
                return fat_name;
            }
        }
#ifdef UIFE_WITH_LIBTSK
        return fat_name + "（已识别；当前 libtsk 未注册该文件系统）";
#elif defined(_WIN32)
        if (image.format_name() != "RAW") {
            return fat_name + "（已识别；Sleuth Kit CLI 后备仅支持 RAW）";
        }
        return TskCliFsParser::available()
            ? fat_name : fat_name + "（已识别；未找到 Sleuth Kit 命令行工具）";
#else
        if (image.format_name() != "RAW") {
            return fat_name + "（已识别；当前构建仅支持 RAW 后端）";
        }
        return fat_name + "（已识别；未构建 libtsk 后端）";
#endif
    }
    if (is_hfs_plus(image, byte_offset)) {
        for (const auto& registration : registrations_) {
            if (registration.probe(image, byte_offset)) {
                return "HFS+";
            }
        }
#ifdef UIFE_WITH_LIBTSK
        return "HFS+（已识别；当前 libtsk 未注册该文件系统）";
#elif defined(_WIN32)
        if (image.format_name() != "RAW") {
            return "HFS+（已识别；Sleuth Kit CLI 后备仅支持 RAW）";
        }
        return TskCliFsParser::available()
            ? "HFS+" : "HFS+（已识别；未找到 Sleuth Kit 命令行工具）";
#else
        if (image.format_name() != "RAW") {
            return "HFS+（已识别；当前构建仅支持 RAW 后端）";
        }
        return "HFS+（已识别；未构建 libtsk 后端）";
#endif
    }
    const auto ext_name = ext_filesystem_name(image, byte_offset);
    if (!ext_name.empty()) {
        for (const auto& registration : registrations_) {
            if (registration.probe(image, byte_offset)) {
                return ext_name;
            }
        }
#ifdef _WIN32
        if (image.format_name() != "RAW") {
            return ext_name + "（已识别；Sleuth Kit CLI 后备仅支持 RAW）";
        }
        return TskCliFsParser::available()
            ? ext_name : ext_name + "（已识别；未找到 Sleuth Kit 命令行工具）";
#else
        if (image.format_name() != "RAW") {
            return ext_name + "（已识别；当前构建仅支持 RAW 后端）";
        }
        return ext_name + "（已识别；未构建 libtsk 后端）";
#endif
    }
    if (is_xfs(image, byte_offset)) {
        for (const auto& registration : registrations_) {
            if (registration.name == "XFS" &&
                registration.probe(image, byte_offset)) {
                return "XFS";
            }
        }
#ifdef UIFE_WITH_TSK_XFS
        return "XFS（已识别；libtsk XFS 解析器不可用）";
#elif defined(_WIN32)
        if (image.format_name() != "RAW") {
            return "XFS（已识别；当前 Sleuth Kit CLI 后备不支持 XFS 或非 RAW 镜像）";
        }
        return TskCliFsParser::available()
            ? "XFS（已识别；当前 Sleuth Kit CLI 未接入 XFS 解析）"
            : "XFS（已识别；未构建可用的 libtsk XFS 后端）";
#else
        if (image.format_name() != "RAW") {
            return "XFS（已识别；当前构建未提供非 RAW 的 libtsk XFS 后端）";
        }
        return "XFS（已识别；未构建可用的 libtsk XFS 后端）";
#endif
    }
    return "未知";
}

std::unique_ptr<IFSParser> FsParserFactory::create(
    const IImageDriver& image, std::uint64_t byte_offset,
    std::string_view password) const {
    if (byte_offset > image.get_size()) {
        throw std::out_of_range("文件系统偏移超出镜像范围");
    }
    if (is_btrfs_send_stream(image, byte_offset)) {
        for (const auto& registration : registrations_) {
            if (registration.name == "Btrfs send stream" &&
                registration.probe(image, byte_offset)) {
                return registration.creator(password);
            }
        }
    }
    if (image.sector_size() > image.get_size() - byte_offset) {
        throw std::out_of_range("文件系统偏移超出镜像范围");
    }
    if (is_wim(image, byte_offset) && byte_offset == 0) {
#ifdef UIFE_WITH_LIBWIM
        return std::make_unique<WimFsParser>();
#else
        throw std::runtime_error(
            "检测到 WIM/ESD，但当前构建未启用 wimlib");
#endif
    }
    for (const auto& registration : registrations_) {
        if (registration.probe(image, byte_offset)) {
            return registration.creator(password);
        }
    }
    if (!ext_filesystem_name(image, byte_offset).empty()) {
        throw std::runtime_error(
            "检测到 ext 文件系统，但当前构建尚未接入可用的 ext2/3/4 解析器");
    }
    if (is_xfs(image, byte_offset)) {
        throw std::runtime_error(
            "检测到 XFS，但当前构建未提供兼容的 libtsk XFS 解析器");
    }
    if (is_apfs_container(image, byte_offset)) {
        throw std::runtime_error(
            "检测到 APFS 容器，但当前构建没有可用的 APFS 解析器"
            "（需要 libfsapfs；libtsk 的 pool API 未提供实现）");
    }
    if (!fat_filesystem_name(image, byte_offset).empty()) {
        throw std::runtime_error(
            "检测到 FAT/exFAT，但当前构建尚未接入可用的文件系统解析器");
    }
    if (is_hfs_plus(image, byte_offset)) {
        throw std::runtime_error(
            "检测到 HFS+，但当前构建尚未接入可用的文件系统解析器");
    }
    if (is_ntfs(image, byte_offset)) {
        throw std::runtime_error(
            "检测到 NTFS，但当前未找到可用的 libtsk 开发库或 Sleuth Kit 命令行工具");
    }
    if (is_refs(image, byte_offset)) {
        throw std::runtime_error(
            "检测到 ReFS，但当前构建没有可用的 ReFS 解析器（需要 libfsrefs）");
    }
    if (is_iso9660(image, byte_offset)) {
        throw std::runtime_error(
            "检测到 ISO9660，但当前构建尚未接入 ISO9660 文件系统解析器");
    }
    throw std::runtime_error("该位置没有已注册的文件系统解析器");
}

std::vector<std::string> FsParserFactory::supported_names() const {
    std::vector<std::string> names;
    names.reserve(registrations_.size());
    for (const auto& registration : registrations_) {
        names.push_back(registration.name);
    }
    return names;
}

} // namespace uife
