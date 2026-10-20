#include "doctest.h"

#include "fake_fs.h"

#include "fujinet/config/fuji_config.h"
#include "fujinet/disk/disk_service.h"
#include "fujinet/disk/image_registry.h"
#include "fujinet/fs/boot_mount.h"
#include "fujinet/fs/storage_manager.h"
#include "fujinet/io/devices/disk_device.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace {

std::unique_ptr<fujinet::tests::MemoryFileSystem> make_host_fs_with_boot()
{
    auto fs = std::make_unique<fujinet::tests::MemoryFileSystem>("host");
    REQUIRE(fs->createDirectory("/boot"));
    const std::vector<std::uint8_t> bytes{0, 1, 2, 3};
    REQUIRE(fs->create_file("/boot/autorun.atr", bytes));
    return fs;
}

} // namespace

TEST_CASE("apply_boot_mount skips normal mode")
{
    fujinet::fs::StorageManager storage;
    REQUIRE(storage.registerFileSystem(make_host_fs_with_boot()));
    fujinet::disk::DiskService disk(storage, fujinet::disk::ImageRegistry{});

    fujinet::config::BootConfig boot{};
    boot.mode = fujinet::config::BootMode::Normal;
    boot.configUri = "persist:/boot/autorun.atr";

    CHECK(fujinet::apply_boot_mount(disk, storage, boot, 0) == 0);
    CHECK(!disk.get_pending_mount(0).has_value());
}

TEST_CASE("apply_boot_mount applies config disk as pending read-only mount")
{
    fujinet::fs::StorageManager storage;
    REQUIRE(storage.registerFileSystem(make_host_fs_with_boot()));
    fujinet::disk::DiskService disk(storage, fujinet::disk::ImageRegistry{});

    fujinet::config::BootConfig boot{};
    boot.mode = fujinet::config::BootMode::Config;
    boot.configUri = "persist:/boot/autorun.atr";
    boot.readOnly = true;

    CHECK(fujinet::apply_boot_mount(disk, storage, boot, 0) == 1);
    auto pending = disk.get_pending_mount(0);
    REQUIRE(pending.has_value());
    CHECK(pending->uri == "persist:/boot/autorun.atr");
    CHECK(pending->mode == "r");
    CHECK(pending->enabled == true);
}

TEST_CASE("apply_boot_mount skips missing config disk")
{
    fujinet::fs::StorageManager storage;
    auto fs = std::make_unique<fujinet::tests::MemoryFileSystem>("host");
    REQUIRE(storage.registerFileSystem(std::move(fs)));
    fujinet::disk::DiskService disk(storage, fujinet::disk::ImageRegistry{});

    fujinet::config::BootConfig boot{};
    boot.mode = fujinet::config::BootMode::Config;
    boot.configUri = "persist:/boot/missing.atr";

    CHECK(fujinet::apply_boot_mount(disk, storage, boot, 0) == 0);
    CHECK(!disk.get_pending_mount(0).has_value());
}

TEST_CASE("apply_boot_mount config overlay removes a restored active projection on failure")
{
    fujinet::fs::StorageManager storage;
    REQUIRE(storage.registerFileSystem(make_host_fs_with_boot()));
    fujinet::disk::DiskService disk(storage, fujinet::disk::ImageRegistry{});
    disk.set_pending_mount(0, "host:/user/d1.atr", "rw", true);
    disk.set_pending_mount(1, "host:/user/d2.atr", "r", true);

    fujinet::config::BootConfig boot{};
    boot.mode = fujinet::config::BootMode::Config;
    boot.configUri = "persist:/boot/missing.atr";

    CHECK(fujinet::apply_boot_mount(disk, storage, boot, 0) == 0);
    CHECK_FALSE(disk.get_pending_mount(0).has_value());
    const auto d2 = disk.get_pending_mount(1);
    REQUIRE(d2.has_value());
    CHECK(d2->uri == "host:/user/d2.atr");
    CHECK(d2->mode == "r");
}

TEST_CASE("apply_boot_mount config overlay replaces active D1 but leaves other slots")
{
    fujinet::fs::StorageManager storage;
    REQUIRE(storage.registerFileSystem(make_host_fs_with_boot()));
    fujinet::disk::DiskService disk(storage, fujinet::disk::ImageRegistry{});
    disk.set_pending_mount(0, "host:/user/d1.atr", "rw", true);
    disk.set_pending_mount(1, "host:/user/d2.atr", "r", true);

    fujinet::config::BootConfig boot{};
    boot.mode = fujinet::config::BootMode::Config;
    boot.configUri = "persist:/boot/autorun.atr";
    boot.readOnly = true;

    CHECK(fujinet::apply_boot_mount(disk, storage, boot, 0) == 1);
    const auto d1 = disk.get_pending_mount(0);
    REQUIRE(d1.has_value());
    CHECK(d1->uri == "persist:/boot/autorun.atr");
    CHECK(d1->mode == "r");
    const auto d2 = disk.get_pending_mount(1);
    REQUIRE(d2.has_value());
    CHECK(d2->uri == "host:/user/d2.atr");
    CHECK(d2->mode == "r");
}

TEST_CASE("config overlay does not persist over a restored user D1")
{
    fujinet::fs::StorageManager storage;
    auto fs = make_host_fs_with_boot();
    REQUIRE(fs->create_file("/fujinet-runtime-mounts.tsv",
                            std::vector<std::uint8_t>{
                                'v', '1', '\n',
                                '0', '\t', '0', '\t', 'r', 'w', '\t',
                                'h', 'o', 's', 't', ':', '/', 'u', 's', 'e', 'r', '/', 'd', '1', '.', 'a', 't', 'r', '\n',
                                '1', '\t', '0', '\t', 'r', '\t',
                                'h', 'o', 's', 't', ':', '/', 'u', 's', 'e', 'r', '/', 'd', '2', '.', 'a', 't', 'r', '\n'}));
    REQUIRE(storage.registerFileSystem(std::move(fs)));

    fujinet::io::DiskDevice configBoot(storage);
    REQUIRE(configBoot.restore_runtime_mounts().size() == 2);
    fujinet::config::BootConfig boot{};
    boot.mode = fujinet::config::BootMode::Config;
    boot.configUri = "host:/boot/autorun.atr";
    boot.readOnly = true;
    REQUIRE(fujinet::apply_boot_mount(configBoot.disk_service(), storage, boot, 0) == 1);
    REQUIRE(configBoot.disk_service().get_pending_mount(0)->uri == "host:/boot/autorun.atr");

    fujinet::io::DiskDevice normalBoot(storage);
    REQUIRE(normalBoot.restore_runtime_mounts().size() == 2);
    const auto d1 = normalBoot.disk_service().get_pending_mount(0);
    const auto d2 = normalBoot.disk_service().get_pending_mount(1);
    REQUIRE(d1.has_value());
    REQUIRE(d2.has_value());
    CHECK(d1->uri == "host:/user/d1.atr");
    CHECK(d1->mode == "rw");
    CHECK(d2->uri == "host:/user/d2.atr");
    CHECK(d2->mode == "r");
}

TEST_CASE("apply_boot_mount stages a network config disk without probing it")
{
    // Nothing exists on the fake network filesystem: at boot the link is not
    // up, so the image can't be probed and must be left to the lazy mount.
    fujinet::fs::StorageManager storage;
    REQUIRE(storage.registerFileSystem(std::make_unique<fujinet::tests::MemoryFileSystem>(
        "tnfs", fujinet::fs::FileSystemKind::NetworkTnfs)));
    fujinet::disk::DiskService disk(storage, fujinet::disk::ImageRegistry{});

    fujinet::config::BootConfig boot{};
    boot.mode = fujinet::config::BootMode::Config;
    boot.configUri = "tnfs://server.example/boot/autorun.atr";

    CHECK(fujinet::apply_boot_mount(disk, storage, boot, 0) == 1);
    auto pending = disk.get_pending_mount(0);
    REQUIRE(pending.has_value());
    CHECK(pending->uri == "tnfs://server.example/boot/autorun.atr");
}

TEST_CASE("apply_boot_mount recognises a network filesystem regardless of URI spelling")
{
    // The decision follows the filesystem the URI resolves to, so scheme case
    // (resolved case-insensitively) cannot change it.
    fujinet::fs::StorageManager storage;
    REQUIRE(storage.registerFileSystem(std::make_unique<fujinet::tests::MemoryFileSystem>(
        "tnfs", fujinet::fs::FileSystemKind::NetworkTnfs)));
    fujinet::disk::DiskService disk(storage, fujinet::disk::ImageRegistry{});

    fujinet::config::BootConfig boot{};
    boot.mode = fujinet::config::BootMode::Config;
    boot.configUri = "TNFS://server.example/boot/autorun.atr";

    CHECK(fujinet::apply_boot_mount(disk, storage, boot, 0) == 1);
    CHECK(disk.get_pending_mount(0).has_value());
}

TEST_CASE("apply_boot_mount stages an HTTP config disk without probing it")
{
    fujinet::fs::StorageManager storage;
    REQUIRE(storage.registerFileSystem(std::make_unique<fujinet::tests::MemoryFileSystem>(
        "http", fujinet::fs::FileSystemKind::NetworkHttp)));
    fujinet::disk::DiskService disk(storage, fujinet::disk::ImageRegistry{});

    fujinet::config::BootConfig boot{};
    boot.mode = fujinet::config::BootMode::Config;
    boot.configUri = "http://server.example/boot/autorun.atr";

    CHECK(fujinet::apply_boot_mount(disk, storage, boot, 0) == 1);
    CHECK(disk.get_pending_mount(0).has_value());
}

TEST_CASE("apply_boot_mount still probes a local filesystem whatever its name")
{
    // A local filesystem registered under a network-looking name is probed as
    // before: the filesystem's kind decides, not its name or URI scheme.
    fujinet::fs::StorageManager storage;
    REQUIRE(storage.registerFileSystem(std::make_unique<fujinet::tests::MemoryFileSystem>(
        "tnfs", fujinet::fs::FileSystemKind::HostPosix)));
    fujinet::disk::DiskService disk(storage, fujinet::disk::ImageRegistry{});

    fujinet::config::BootConfig boot{};
    boot.mode = fujinet::config::BootMode::Config;
    boot.configUri = "tnfs://server.example/boot/missing.atr";

    CHECK(fujinet::apply_boot_mount(disk, storage, boot, 0) == 0);
    CHECK(!disk.get_pending_mount(0).has_value());
}

TEST_CASE("is_network_kind classifies every filesystem kind")
{
    using fujinet::fs::FileSystemKind;
    using fujinet::fs::is_network_kind;
    CHECK(is_network_kind(FileSystemKind::NetworkTnfs));
    CHECK(is_network_kind(FileSystemKind::NetworkSmb));
    CHECK(is_network_kind(FileSystemKind::NetworkFtp));
    CHECK(is_network_kind(FileSystemKind::NetworkHttp));
    CHECK_FALSE(is_network_kind(FileSystemKind::LocalFlash));
    CHECK_FALSE(is_network_kind(FileSystemKind::LocalSD));
    CHECK_FALSE(is_network_kind(FileSystemKind::HostPosix));
    CHECK_FALSE(is_network_kind(FileSystemKind::Unknown));
}

TEST_CASE("apply_boot_mount uses the bootstrap-selected active disk unit")
{
    fujinet::fs::StorageManager storage;
    REQUIRE(storage.registerFileSystem(make_host_fs_with_boot()));
    fujinet::disk::DiskService disk(storage, fujinet::disk::ImageRegistry{});

    fujinet::config::BootConfig boot{};
    boot.mode = fujinet::config::BootMode::Config;
    boot.configUri = "persist:/boot/autorun.atr";
    boot.readOnly = true;

    CHECK(fujinet::apply_boot_mount(disk, storage, boot, 1) == 1);
    CHECK(!disk.get_pending_mount(0).has_value());
    auto pending = disk.get_pending_mount(1);
    REQUIRE(pending.has_value());
    CHECK(pending->uri == "persist:/boot/autorun.atr");
}
