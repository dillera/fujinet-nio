#include "fujinet/io/atari_sio_disk_frontend.h"

#include <algorithm>

namespace fujinet::io {
namespace {
constexpr std::uint8_t ACK = 0x41, NAK = 0x4e, COMPLETE = 0x43, ERROR = 0x45;
constexpr std::uint8_t READ = 0x52, PUT = 0x50, STATUS = 0x53, WRITE = 0x57, HSIO_INDEX = 0x3f;
}

std::uint8_t AtariSioDiskFrontend::checksum(const std::uint8_t* bytes, std::size_t size)
{
    std::uint16_t sum = 0;
    for (std::size_t i = 0; i != size; ++i) {
        sum += bytes[i];
        sum = static_cast<std::uint16_t>((sum & 0xffu) + (sum >> 8));
    }
    return static_cast<std::uint8_t>(sum);
}

bool AtariSioDiskFrontend::supported_geometry(const disk::DiskSlotInfo& info)
{
    return info.inserted && (info.geometry.sectorSize == 128 || info.geometry.sectorSize == 256);
}

std::size_t AtariSioDiskFrontend::sector_bytes(std::size_t slot, std::uint16_t sector) const
{
    const auto info = _service.info(slot);
    if (info.geometry.sectorSize == 256 && sector <= 3) return 128;
    return info.geometry.sectorSize;
}

std::uint8_t AtariSioDiskFrontend::controller_status(disk::DiskError error)
{
    using disk::DiskError;
    switch (error) {
    case DiskError::None: return 0xdf;
    case DiskError::ReadOnly: return 0x9f;
    case DiskError::InvalidRequest:
    case DiskError::OutOfRange:
    case DiskError::InvalidSlot:
    case DiskError::InvalidGeometry: return 0xcf;
    case DiskError::IoError:
    case DiskError::InternalError: return 0xdb;
    default: return 0x5f;
    }
}

void AtariSioDiskFrontend::record(std::size_t slot, disk::DiskError error, std::uint8_t transient)
{
    _state[slot].error = error;
    _state[slot].transient = transient;
}

AtariSioDiskFrontend::Result AtariSioDiskFrontend::status(std::size_t slot)
{
    const auto info = _service.info(slot); // STATUS must never lazy-mount.
    const auto pending = _service.get_pending_mount(slot);
    std::uint8_t drive = _state[slot].transient;
    disk::DiskError error = _state[slot].error;
    if (!info.inserted) {
        error = pending ? disk::DiskError::NotMounted : (error == disk::DiskError::None ? disk::DiskError::NotMounted : error);
    } else if (!supported_geometry(info)) {
        error = disk::DiskError::InvalidGeometry;
    } else {
        if (info.readOnly) drive |= 0x08;
        if (info.geometry.sectorSize == 256) drive |= 0x20;
    }
    std::vector<std::uint8_t> out{ACK, COMPLETE, drive, controller_status(error), 0x0f, 0x00};
    out.push_back(checksum(out.data() + 2, 4));
    return Result{std::move(out), 0, true};
}

AtariSioDiskFrontend::Result AtariSioDiskFrontend::read(std::size_t slot, std::uint16_t sector)
{
    if (sector == 0) {
        record(slot, disk::DiskError::InvalidRequest, 0x04);
        return Result{{ACK, ERROR}, 0, true};
    }
    const auto before = _service.info(slot);
    if (before.inserted && !supported_geometry(before)) {
        record(slot, disk::DiskError::InvalidGeometry, 0x04);
        return Result{{ACK, ERROR}, 0, true};
    }
    std::array<std::uint8_t, max_sector_bytes> buffer{};
    const auto result = _service.read_sector(slot, static_cast<std::uint32_t>(sector - 1), buffer.data(), buffer.size());
    if (!result.ok()) {
        record(slot, result.error, 0x04);
        return Result{{ACK, ERROR}, 0, true};
    }
    record(slot, disk::DiskError::None, 0);
    const std::size_t bytes = result.bytes;
    std::vector<std::uint8_t> out{ACK, COMPLETE};
    out.insert(out.end(), buffer.begin(), buffer.begin() + bytes);
    out.push_back(checksum(buffer.data(), bytes));
    return Result{std::move(out), 0, true};
}

AtariSioDiskFrontend::Result AtariSioDiskFrontend::begin(const std::array<std::uint8_t, command_bytes>& command)
{
    _pending = PendingWrite::None;
    if (checksum(command.data(), command.size() - 1) != command.back() || command[0] < 0x31 || command[0] > 0x38) {
        return Result{{NAK}, 0, false};
    }
    const std::size_t slot = command[0] - 0x31;
    const std::uint16_t sector = static_cast<std::uint16_t>(command[2] | (static_cast<std::uint16_t>(command[3]) << 8));
    switch (command[1]) {
    case STATUS:
        // STATUS has no sector argument. MyDOS 4.53/4 retains AUX $0028
        // when polling the drive; ignore both AUX bytes for this command.
        return status(slot);
    case HSIO_INDEX: {
        // The Happy/Speedy "ultra speed" query a high-speed SIO driver sends once per drive, at standard
        // speed: reply the POKEY divisor to use. AUX is ignored (drivers send $20). Off: NAK, as for
        // any command we do not have.
        if (_hsio_index < 0) return Result{{NAK}, 0, false};
        const auto index = static_cast<std::uint8_t>(_hsio_index);
        Result result{{ACK, COMPLETE, index, checksum(&index, 1)}, 0, true};
        result.high_speed = true;
        return result;
    }
    case READ:
        return read(slot, sector);
    case PUT:
    case WRITE: {
        if (sector == 0) {
            record(slot, disk::DiskError::InvalidRequest, 0x04);
            return Result{{ACK, ERROR}, 0, true};
        }
        // A write needs the actual sector transfer size before its data phase.
        // Activating a pending slot here is the operation's lazy access; STATUS
        // above remains inspection-only.
        auto info = _service.info(slot);
        if (!info.inserted && _service.get_pending_mount(slot)) {
            const auto mounted = _service.ensure_mounted(slot);
            if (!mounted.ok()) {
                record(slot, mounted.error, 0x04);
                return Result{{ACK, ERROR}, 0, true};
            }
            info = _service.info(slot);
        }
        if (info.inserted && !supported_geometry(info)) {
            record(slot, disk::DiskError::InvalidGeometry, 0x04);
            return Result{{ACK, ERROR}, 0, true};
        }
        _pending = command[1] == PUT ? PendingWrite::Put : PendingWrite::Verify;
        _pending_slot = slot;
        _pending_sector = sector;
        _pending_bytes = sector_bytes(slot, sector);
        if (_pending_bytes == 0 || _pending_bytes > max_sector_bytes) {
            _pending = PendingWrite::None;
            record(slot, disk::DiskError::InvalidGeometry, 0x04);
            return Result{{ACK, ERROR}, 0, true};
        }
        return Result{{ACK}, _pending_bytes, true};
    }
    default:
        return Result{{NAK}, 0, false};
    }
}

AtariSioDiskFrontend::Result AtariSioDiskFrontend::complete_write(const std::uint8_t* data, std::size_t data_bytes)
{
    if (_pending == PendingWrite::None || !data || data_bytes != _pending_bytes + 1) return Result{{NAK}, 0, false};
    const auto kind = _pending;
    _pending = PendingWrite::None;
    if (checksum(data, _pending_bytes) != data[_pending_bytes]) {
        record(_pending_slot, disk::DiskError::InvalidRequest, 0x02);
        return Result{{NAK}, 0, false};
    }
    const auto write = _service.write_sector(_pending_slot, _pending_sector - 1, data, _pending_bytes);
    if (!write.ok() || !_service.flush(_pending_slot).ok()) {
        record(_pending_slot, write.ok() ? disk::DiskError::IoError : write.error, 0x04);
        return Result{{ACK, ERROR}, 0, true};
    }
    if (kind == PendingWrite::Verify) {
        std::array<std::uint8_t, max_sector_bytes> readback{};
        const auto reread = _service.read_sector(_pending_slot, _pending_sector - 1, readback.data(), readback.size());
        if (!reread.ok() || reread.bytes != _pending_bytes || !std::equal(data, data + _pending_bytes, readback.begin())) {
            record(_pending_slot, reread.ok() ? disk::DiskError::IoError : reread.error, 0x04);
            return Result{{ACK, ERROR}, 0, true};
        }
    }
    record(_pending_slot, disk::DiskError::None, 0);
    return Result{{ACK, COMPLETE}, 0, true};
}

} // namespace fujinet::io
