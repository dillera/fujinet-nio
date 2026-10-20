#pragma once

#include "fujinet/config/fuji_config.h"
#include "fujinet/io/atari_sio_disk_frontend.h"
#include "fujinet/io/atari_sio_transaction_boundary.h"
#include "fujinet/io/core/work_source.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace fujinet::platform::esp32 {

class UartChannel;

// The single owner of Atari SIO UART/GPIO sequencing.  Disk transactions are
// dispatched directly to DiskService; 0x7f is intentionally only an attached
// future endpoint, never a disk alias or transport.
class AtariSioIngress final : public io::IWorkSource {
public:
    using NativeHandler = std::function<void(const std::vector<std::uint8_t>&)>;

    AtariSioIngress(const config::FujiConfig& config, disk::DiskService& disks);
    ~AtariSioIngress() override;

    void poll();
    // ISR-safe edge recorder; UART/event work remains the sole wake source.
    void note_cmd_edge() noexcept;
    void set_native_handler(NativeHandler handler) { _nativeHandler = std::move(handler); }
    bool supports_work_wait() const override;
    bool wait_for_work(std::chrono::milliseconds timeout) override;

private:
    void reset_transaction();
    void consume(std::uint8_t byte);
    void send(const std::vector<std::uint8_t>& bytes);

    std::unique_ptr<UartChannel> _uart;
    io::AtariSioDiskFrontend _disk;
    std::array<std::uint8_t, io::AtariSioDiskFrontend::command_bytes> _command{};
    std::vector<std::uint8_t> _write;
    std::size_t _commandUsed{0};
    std::size_t _writeExpected{0};
    io::AtariSioTransactionBoundary _boundary;
    NativeHandler _nativeHandler;
};

} // namespace fujinet::platform::esp32
