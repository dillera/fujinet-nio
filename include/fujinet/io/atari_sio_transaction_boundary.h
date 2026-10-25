#pragma once

#include <array>
#include <cstddef>
#include <atomic>
#include <cstdint>

namespace fujinet::io {

// A counter detects multiple observed edges between polls; unlike a boolean it
// cannot silently turn two observed edges into one. Hardware may still merge
// interrupts while masked, so this is not a physical edge timestamp.
class AtariSioTransactionBoundary {
public:
    void note_cmd_edge() noexcept { _edges.fetch_add(1, std::memory_order_relaxed); }
    std::uint32_t consume() noexcept { return _edges.exchange(0, std::memory_order_acq_rel); }
private:
    std::atomic<std::uint32_t> _edges{0};
};

// State machine used by the ESP32 ingress. The callables expose the existing
// GPIO, UART and clock operations so the identical ordering logic can run in
// host tests without ESP-IDF.
class AtariSioCapture {
public:
    bool needs_immediate_poll() const noexcept { return _collecting; }

    template<class Router, class Uart, class Asserted, class Now>
    void poll(AtariSioTransactionBoundary& boundary, Router& router, Uart& uart,
              Asserted asserted, Now now)
    {
        const auto abort = [this, &router, &uart] {
            router.reset_command_collection();
            _collecting = false;
            _released = false;
            _frame_used = 0;
            _write_started_us = 0;
            uart.discardInput();
            _rx_errors = uart.rx_errors();
            _discard_until_release = true;
        };
        if (_write_started_us && now() - _write_started_us > 200000) {
            abort();
            return;
        }
        const std::uint32_t edges = boundary.consume();
        const bool low = asserted();
        if (_discard_until_release) {
            uart.discardInput();
            if (!low && edges == 0) _discard_until_release = false;
            return;
        }
        if (edges > 1 || (edges == 1 && (low == _collecting || _released))) {
            abort();
            return;
        }
        if (edges == 1 && low) {
            router.reset_command_collection();
            _frame_used = 0;
            _write_started_us = 0;
            _collecting = true;
            _released = false;
            _started_us = now();
        }
        if (_collecting && now() - _started_us > 50000) {
            abort();
            return;
        }
        std::uint8_t bytes[256];
        while (uart.available()) {
            const auto count = uart.read(bytes, sizeof(bytes));
            if (uart.rx_errors() != _rx_errors || boundary.consume() != 0 || asserted() != low) {
                abort();
                return;
            }
            for (std::size_t i = 0; i < count; ++i) {
                if (_collecting) {
                    if (_frame_used == _frame.size()) { abort(); return; }
                    _frame[_frame_used++] = bytes[i];
                } else if (router.pending_write_bytes()) {
                    router.consume(bytes[i]);
                    _write_started_us = router.pending_write_bytes() ? now() : 0;
                }
                // Idle bytes, including traffic for another peripheral, are ignored.
            }
        }
        if ((_collecting || router.pending_write_bytes()) && uart.rx_errors() != _rx_errors) {
            abort();
            return;
        }
        if (_collecting && edges == 1 && !low) _released = true;
        if (_collecting && _released && _frame_used == _frame.size()) {
            if (boundary.consume() != 0 || asserted()) {
                abort();
                return;
            }
            _collecting = false;
            _released = false;
            for (const auto byte : _frame) router.consume(byte);
            _write_started_us = router.pending_write_bytes() ? now() : 0;
            _frame_used = 0;
        }
        if (!_collecting && !router.pending_write_bytes()) _rx_errors = uart.rx_errors();
    }
private:
    std::array<std::uint8_t, 5> _frame{};
    std::size_t _frame_used{0};
    std::int64_t _started_us{0};
    std::int64_t _write_started_us{0};
    std::uint32_t _rx_errors{0};
    bool _collecting{false};
    bool _released{false};
    bool _discard_until_release{false};
};

} // namespace fujinet::io
