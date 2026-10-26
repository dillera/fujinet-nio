#pragma once

#include <array>
#include <cstdint>

namespace fujinet::io {

// Counters for bringing up the physical Atari SIO ingress. They are plain counts
// bumped on the polling path (no logging there: SIO timing is tight), read by the
// sio.status console command.

// What the capture state machine saw, from the UART and the CMD line.
struct AtariSioCaptureStats {
    std::uint32_t rx_bytes{0};         // bytes read from the SIO UART
    std::uint32_t frames{0};           // complete 5-byte command frames handed to the router
    std::uint32_t aborts_edge{0};      // CMD edges that did not fit a frame (extra, early or missing)
    std::uint32_t aborts_timeout{0};   // frame not complete in 50 ms, or write data not in 200 ms
    std::uint32_t aborts_overflow{0};  // more than 5 command bytes while CMD was low
    std::uint32_t aborts_rx_error{0};  // UART framing/parity/overflow error during a frame or data
};

// What the router did with each frame.
struct AtariSioRouterStats {
    std::uint32_t disk_commands{0};    // D1:-D8: (0x31-0x38)
    std::uint32_t nio_commands{0};     // the native endpoint (0x7F)
    std::uint32_t other_commands{0};   // frames for other bus devices, ignored on purpose
    bool has_last_frame{false};
    std::array<std::uint8_t, 5> last_frame{};  // device, command, aux1, aux2, checksum
};

// The whole picture the console shows; the ingress fills in the hardware parts.
struct AtariSioStats {
    std::uint32_t cmd_edges{0};        // CMD line edges seen by the interrupt since the last reset
    bool cmd_line_low{false};          // CMD level right now (low = asserted by the Atari)
    std::uint32_t uart_rx_errors{0};   // the SIO UART's own error count
    AtariSioCaptureStats capture;
    AtariSioRouterStats router;
};

} // namespace fujinet::io
