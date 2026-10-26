#pragma once

#include "fujinet/config/fuji_config.h"
#include "fujinet/io/atari_sio_disk_frontend.h"
#include "fujinet/io/atari_sio_ingress_router.h"
#include "fujinet/io/atari_sio_nio_frontend.h"
#include "fujinet/io/atari_sio_transaction_boundary.h"
#include "fujinet/io/core/work_source.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace fujinet::platform::esp32 {

class UartChannel;

// The single owner of Atari SIO UART/GPIO sequencing. D1:-D8: remain direct
// DiskService traffic; 0x7f is the native packet endpoint on this same ingress.
class AtariSioIngress final : public io::IWorkSource {
public:
    AtariSioIngress(const config::FujiConfig& config,
                    disk::DiskService& disks,
                    io::IRequestHandler& routing);
    ~AtariSioIngress() override;

    void poll();
    // Counters for bring-up (sio.status). stats() reads the CMD line too.
    io::AtariSioStats stats() const;
    void reset_stats();
    // ISR-safe edge recorder; UART/event work remains the sole wake source.
    void note_cmd_edge() noexcept;
    bool supports_work_wait() const override;
    bool wait_for_work(std::chrono::milliseconds timeout) override;

private:
    void send(const std::vector<std::uint8_t>& bytes);

    std::unique_ptr<UartChannel> _uart;
    io::AtariSioDiskFrontend _disk;
    io::AtariSioNioFrontend _nio;
    io::AtariSioIngressRouter _router;
    io::AtariSioTransactionBoundary _boundary;
    io::AtariSioCapture _capture;
    io::AtariSioReplyStats _reply;
    std::uint32_t _last_poll_us{0};
    std::uint32_t _edges_at_last_poll{0};
    std::uint32_t _poll_gap_us_max{0};
    int _cmd_pin{-1};
    void* _owner_task{nullptr};
};

} // namespace fujinet::platform::esp32
