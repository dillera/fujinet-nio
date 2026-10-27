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
    std::uint32_t frames_bad{0};       // command frames that could not be read: bad checksum, UART error or overflow
    std::uint32_t aborts_edge{0};      // CMD edges that did not fit a frame (extra, early or missing)
    std::uint32_t aborts_timeout{0};   // frame not complete in 50 ms, or write data not in 200 ms
    std::uint32_t aborts_overflow{0};  // more than 5 command bytes while CMD was low
    std::uint32_t aborts_rx_error{0};  // UART framing/parity/overflow error during a frame or data
    // Timing, in microseconds (the clock the ingress was given; since boot on the device).
    std::uint32_t first_frame_us{0};   // when the first complete command frame was processed (0 = none)
    std::uint32_t lag_us_max{0};       // CMD release edge -> frame processing started: how fast the loop reacts
};

// When each reply started, measured where the ingress sends it. A command reply is judged against the
// CMD release edge (the Atari expects the ACK within 16 ms); every reply is judged against when its
// processing started, which is the time the handler took.
struct AtariSioReplyStats {
    static constexpr std::uint32_t ack_deadline_us = 16000;

    std::uint32_t ready_us{0};         // when the ingress was ready to serve (set once, kept across resets)
    std::uint32_t replies{0};
    std::uint32_t reply_us_last{0};    // command replies: CMD release edge -> first reply byte sent
    std::uint32_t reply_us_max{0};
    std::uint32_t reply_over_deadline{0};
    std::uint32_t handle_us_last{0};   // any reply: processing started -> reply sent
    std::uint32_t handle_us_max{0};

    // Unsigned subtraction, so the clock's 32-bit wrap does not matter for short intervals.
    void record(std::uint32_t send_us, std::uint32_t processing_start_us, bool command_reply,
                std::uint32_t cmd_release_edge_us) noexcept
    {
        ++replies;
        handle_us_last = send_us - processing_start_us;
        if (handle_us_last > handle_us_max) handle_us_max = handle_us_last;
        if (command_reply) {
            reply_us_last = send_us - cmd_release_edge_us;
            if (reply_us_last > reply_us_max) reply_us_max = reply_us_last;
            if (reply_us_last > ack_deadline_us) ++reply_over_deadline;
        }
    }
    void reset() noexcept
    {
        const std::uint32_t ready = ready_us;
        *this = {};
        ready_us = ready;
    }
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
    std::uint32_t first_edge_us{0};    // when the first CMD edge was seen (0 = none)
    bool cmd_line_low{false};          // CMD level right now (low = asserted by the Atari)
    std::uint32_t uart_rx_errors{0};   // the SIO UART's own error count
    AtariSioCaptureStats capture;
    AtariSioRouterStats router;
    AtariSioReplyStats reply;
    // Longest time between two polls of the ingress during which the Atari signalled CMD (a stalled
    // SIO task that could miss a command). Idle gaps with no CMD edge are not counted.
    std::uint32_t poll_gap_us_max{0};
    std::uint32_t poll_gap_at_us{0};   // when that longest gap began (same clock as first_edge_us)
    std::uint32_t uart_baud_set{0};    // the baud the SIO UART was last told to use (0 = unknown)
    std::uint32_t uart_baud_hw{0};     // what the UART hardware reports back after its divider rounding
    int hsio_index{-1};                // high-speed SIO index answered to 3F (-1 = off)
    bool hsio_high{false};             // the UART is at the high speed right now
    std::uint32_t hsio_switches{0};    // times the UART was switched to high speed
    std::uint32_t hsio_fallbacks{0};   // times it was switched back after unreadable command frames
    std::uint32_t stale_replies_dropped{0};  // native replies the Atari never read, discarded after 2 s
};

} // namespace fujinet::io
