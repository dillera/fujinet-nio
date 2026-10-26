#pragma once

#include "fujinet/io/core/request_handler.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fujinet::io {

// Portable native-v1 SIO endpoint.  It owns the 0x7f Q/W/P/R transaction
// state but deliberately has no UART, GPIO, Channel, or disk dependency.
class AtariSioNioFrontend {
public:
    static constexpr std::size_t command_bytes = 5;
    static constexpr std::size_t capacity = 512;

    struct Result {
        std::vector<std::uint8_t> bytes;
        std::size_t expected_write_bytes{0};
        bool accepted{false};
    };

    explicit AtariSioNioFrontend(IRequestHandler& handler) : _handler(handler) {}
    ~AtariSioNioFrontend();
    AtariSioNioFrontend(const AtariSioNioFrontend&) = delete;
    AtariSioNioFrontend& operator=(const AtariSioNioFrontend&) = delete;

    Result begin(const std::array<std::uint8_t, command_bytes>& command);
    Result complete_write(const std::uint8_t* data, std::size_t data_bytes);
    // An ingress CMD edge interrupted an accepted W before its data frame.
    // This must not discard a response already waiting for terminal R.
    void abort_write();
    void reset();
    // A host reset abandons the Atari client's network sessions. This is
    // distinct from clearing an interrupted W/P/R transaction.
    void reset_host();

    // A reply the Atari never read (its call timed out) would hold the endpoint busy and answer
    // every new Q/W with NAK. With a clock set, a Q or W arriving this long after the reply became
    // ready discards it. Without a clock (tests, POSIX) nothing expires. P and R never discard.
    using Clock = std::uint64_t (*)();
    static constexpr std::uint64_t stale_reply_us = 2'000'000;
    void set_clock(Clock clock) { _clock = clock; }
    std::uint32_t stale_replies_dropped() const { return _stale_dropped; }

    static std::uint8_t checksum(const std::uint8_t* data, std::size_t len);

private:
    enum class State : std::uint8_t { Idle, Receiving, Ready };

    static constexpr std::uint8_t device_id = 0x7f;
    static constexpr std::uint8_t ack = 'A';
    static constexpr std::uint8_t nak = 'N';
    static constexpr std::uint8_t complete = 'C';

    void drop_stale_reply();
    Result reject() const;
    Result data_reply(const std::vector<std::uint8_t>& data) const;
    std::uint16_t aux(const std::array<std::uint8_t, command_bytes>& command) const;
    std::vector<std::uint8_t> bounded_error_response(const IORequest& request) const;

    IRequestHandler& _handler;
    State _state{State::Idle};
    std::size_t _write_bytes{0};
    RequestID _next_request_id{1};
    Clock _clock{nullptr};
    std::uint64_t _ready_at_us{0};
    std::uint32_t _stale_dropped{0};
    std::vector<std::uint8_t> _response;
};

} // namespace fujinet::io
