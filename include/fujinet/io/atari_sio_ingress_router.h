#pragma once

#include "fujinet/io/atari_sio_disk_frontend.h"
#include "fujinet/io/atari_sio_nio_frontend.h"
#include "fujinet/io/atari_sio_stats.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace fujinet::io {

// Byte-level half of the one physical Atari SIO ingress.  Hardware owns CMD
// edges and UART I/O; this router owns only command/data collection and the
// disk-versus-native pending-write discriminator so it can be tested on POSIX.
class AtariSioIngressRouter {
public:
    using Send = std::function<void(const std::vector<std::uint8_t>&)>;

    AtariSioIngressRouter(AtariSioDiskFrontend& disk, AtariSioNioFrontend& nio, Send send);

    void consume(std::uint8_t byte);
    // Exact accepted data bytes, excluding the SIO checksum.  Virtual SIO
    // carriers use this to request precisely the same write frame as UART.
    std::size_t pending_write_bytes() const noexcept;
    // Called by the physical CMD edge boundary.  It cancels only an incomplete
    // native write data phase; a ready NIO response deliberately survives.
    void reset_command_collection();

    // Called after a disk reply that asks for the high-speed switch (3F) has been sent.
    void set_high_speed_handler(std::function<void()> handler) { _high_speed = std::move(handler); }

    const AtariSioRouterStats& stats() const noexcept { return _stats; }
    void reset_stats() noexcept { _stats = {}; }

private:
    enum class PendingWrite : std::uint8_t { None, Disk, Nio };

    void begin_command();
    void note_refused(const std::array<std::uint8_t, AtariSioDiskFrontend::command_bytes>& command);
    void complete_write();
    void start_write(PendingWrite kind, std::size_t bytes);

    AtariSioDiskFrontend& _disk;
    AtariSioNioFrontend& _nio;
    Send _send;
    std::function<void()> _high_speed;
    std::array<std::uint8_t, AtariSioDiskFrontend::command_bytes> _command{};
    std::vector<std::uint8_t> _write;
    std::size_t _command_used{0};
    std::size_t _write_expected{0};
    PendingWrite _pending{PendingWrite::None};
    AtariSioRouterStats _stats;
};

} // namespace fujinet::io
