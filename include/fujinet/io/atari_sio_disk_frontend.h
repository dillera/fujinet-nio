#pragma once

#include "fujinet/disk/disk_service.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fujinet::io {

// Standard Atari D1:-D8: disk protocol adapter.  It deliberately has no
// Channel, transport, or ESP32 dependency: its sole backing service is the
// DiskDevice-owned DiskService supplied by the application.
class AtariSioDiskFrontend {
public:
    static constexpr std::size_t command_bytes = 5;
    static constexpr std::size_t max_sector_bytes = 256;

    struct Result {
        std::vector<std::uint8_t> bytes;
        std::size_t expected_write_bytes{0};
        bool accepted{false};
    };

    explicit AtariSioDiskFrontend(disk::DiskService& service) : _service(service) {}

    // Processes a complete, checksummed SIO command.  For PUT/WRITE the
    // returned ACK requests exactly expected_write_bytes plus its checksum;
    // complete_write() then performs the media operation.
    Result begin(const std::array<std::uint8_t, command_bytes>& command);
    Result complete_write(const std::uint8_t* data, std::size_t data_bytes);

    static std::uint8_t checksum(const std::uint8_t* bytes, std::size_t size);

private:
    enum class PendingWrite { None, Put, Verify };
    struct DriveState {
        std::uint8_t transient{0};
        disk::DiskError error{disk::DiskError::None};
    };

    Result status(std::size_t slot);
    Result read(std::size_t slot, std::uint16_t sector);
    void record(std::size_t slot, disk::DiskError error, std::uint8_t transient);
    static std::uint8_t controller_status(disk::DiskError error);
    static bool supported_geometry(const disk::DiskSlotInfo& info);
    std::size_t sector_bytes(std::size_t slot, std::uint16_t sector) const;

    disk::DiskService& _service;
    std::array<DriveState, disk::DiskService::MAX_SLOTS> _state{};
    PendingWrite _pending{PendingWrite::None};
    std::size_t _pending_slot{0};
    std::uint16_t _pending_sector{0};
    std::size_t _pending_bytes{0};
};

} // namespace fujinet::io
