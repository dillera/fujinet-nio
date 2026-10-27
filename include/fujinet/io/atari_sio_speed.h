#pragma once

#include <cstdint>

namespace fujinet::io {

// High-speed SIO state (docs/atari/hsio.md). Off by default. When an index is set, a `3F` query to a
// disk answers it (the POKEY divisor the Atari should use) and the ingress switches its UART to
// that speed; two command frames in a row that cannot be read switch it back.
class AtariSioSpeed {
public:
    static constexpr std::uint32_t standard_baud = 19200;
    // Index 10 is not offered: a driver reads a speed byte of $0A as a Happy 1050 and probes it.
    static constexpr int max_index = 9;
    static constexpr std::uint32_t fallback_after_bad_frames = 2;

    // The legacy firmware's rate for an index: the PAL POKEY clock over 2 x (index + 7), with 3 CPU
    // cycles per byte added to compensate for late POKEY sampling. Index 0 is 124,017 baud.
    static constexpr std::uint32_t baud_for_index(unsigned index)
    {
        return static_cast<std::uint32_t>(static_cast<std::uint64_t>(pal_clock_hz) * 10 /
                                          (10 * (2 * (index + 7)) + 3));
    }

    // -1 turns it off. False (and no change) for an index above max_index.
    bool set_index(int index)
    {
        if (index < -1 || index > max_index) return false;
        _index = index;
        if (_index < 0) _high = false;
        return true;
    }

    int index() const { return _index; }
    bool enabled() const { return _index >= 0; }
    bool high() const { return _high; }
    void enter_high() { if (enabled()) _high = true; }
    void leave_high() { _high = false; }
    std::uint32_t baud() const { return _high ? baud_for_index(static_cast<unsigned>(_index)) : standard_baud; }

private:
    static constexpr std::uint32_t pal_clock_hz = 1773447;

    int _index{-1};
    bool _high{false};
};

} // namespace fujinet::io
