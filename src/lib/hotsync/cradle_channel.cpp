#include "fujinet/hotsync/cradle_channel.h"

#include "fujinet/io/core/baud_control.h"

#include <algorithm>
#include <thread>
#include <utility>

namespace fujinet::hotsync {

namespace {

constexpr std::uint8_t kSlipEnd = 0xC0;
constexpr std::uint8_t kSlipEsc = 0xDB;
constexpr std::uint8_t kSlipEscEnd = 0xDC;
constexpr std::uint8_t kSlipEscEsc = 0xDD;
// The FujiBus packet header.
constexpr std::size_t kMinFrameBytes = 6;
// Longer than this without an END is not FujiBus.
constexpr std::size_t kMaxFrameBytes = 65536;
// Bounds the discard so a line that never goes quiet cannot hold us here.
constexpr std::size_t kMaxDiscardBytes = 16384;
constexpr auto kPoll = std::chrono::milliseconds(2);
constexpr auto kLentIdle = std::chrono::milliseconds(10);

std::uint64_t steady_now_ms()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

} // namespace

CradleChannel::CradleChannel(std::unique_ptr<io::Channel> line, io::BaudControl& baud, NowMs now,
                             CradleTiming timing)
    : _line(std::move(line)), _baud(baud), _now(now ? std::move(now) : NowMs(&steady_now_ms)),
      _timing(timing)
{}

bool CradleChannel::available()
{
    std::lock_guard<std::mutex> lock(_mutex);
    return !_lent && (!_pending.empty() || _line->available());
}

std::size_t CradleChannel::read(std::uint8_t* buffer, std::size_t maxLen)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_lent || buffer == nullptr || maxLen == 0) {
        return 0;
    }
    if (!_pending.empty()) {
        const std::size_t n = std::min(maxLen, _pending.size());
        std::copy(_pending.begin(), _pending.begin() + static_cast<std::ptrdiff_t>(n), buffer);
        _pending.erase(_pending.begin(), _pending.begin() + static_cast<std::ptrdiff_t>(n));
        return n;
    }
    const std::size_t n = _line->read(buffer, maxLen);
    if (n > 0) {
        scan(buffer, n, _now());
    }
    return n;
}

void CradleChannel::write(const std::uint8_t* buffer, std::size_t len)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_lent) {
        ++_stats.droppedWrites;
        return;
    }
    _line->write(buffer, len);
}

bool CradleChannel::supports_readable_wait() const
{
    return _line->supports_readable_wait();
}

bool CradleChannel::wait_for_readable(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(_mutex);
    if (_lent) {
        lock.unlock();
        std::this_thread::sleep_for(std::min(timeout, std::chrono::milliseconds(kLentIdle)));
        return false;
    }
    if (!_pending.empty()) {
        return true;
    }
    // Held across the wait: some lines (the ESP32 UART) read their driver
    // while waiting, so a claim must not start until the wait ends.
    return _line->wait_for_readable(timeout);
}

void CradleChannel::start_frame()
{
    _inFrame = true;
    _escape = false;
    _frameLen = 0;
    _lengthField = 0;
    _checksumField = 0;
    _sum = 0;
}

void CradleChannel::stray(std::size_t count, std::uint64_t now)
{
    _stats.strayBytes += static_cast<std::uint32_t>(count);
    if (!_sawFrame || now - _lastFrameMs >= _timing.strayGraceMs) {
        _holdUntilMs = now;
    }
}

// Follows SLIP framing and checks each frame as a FujiBus packet (header:
// device, command, u16 LE total length, checksum, descriptor) without keeping
// it: the length must match and the folded sum, taken with the checksum byte
// as zero, must equal the checksum.
void CradleChannel::scan(const std::uint8_t* data, std::size_t len, std::uint64_t now)
{
    for (std::size_t i = 0; i < len; ++i) {
        std::uint8_t b = data[i];
        if (b == kSlipEnd) {
            if (_inFrame && _frameLen > 0) {
                const bool packet = _frameLen >= kMinFrameBytes && _lengthField == _frameLen &&
                                    static_cast<std::uint8_t>(_sum) == _checksumField;
                if (packet) {
                    ++_stats.frames;
                    _sawFrame = true;
                    _lastFrameMs = now;
                    _holdUntilMs = now + _timing.hostHoldMs;
                    _inFrame = false;
                    continue;
                }
                stray(_frameLen, now);
            }
            // An END after anything but a packet may open the next one.
            start_frame();
            continue;
        }
        if (!_inFrame) {
            stray(1, now);
            continue;
        }
        if (_escape) {
            _escape = false;
            b = b == kSlipEscEnd ? kSlipEnd : b == kSlipEscEsc ? kSlipEsc : b;
        } else if (b == kSlipEsc) {
            _escape = true;
            continue;
        }
        const std::size_t pos = _frameLen++;
        if (pos == 2) {
            _lengthField = b;
        } else if (pos == 3) {
            _lengthField = static_cast<std::uint16_t>(_lengthField | (b << 8));
        }
        if (pos == 4) {
            _checksumField = b;
        } else {
            _sum = static_cast<std::uint16_t>(_sum + b);
            _sum = static_cast<std::uint16_t>((_sum >> 8) + (_sum & 0xFF));
        }
        if (_frameLen > kMaxFrameBytes) {
            stray(_frameLen, now);
            _inFrame = false;
        }
    }
}

void CradleChannel::discard_pending()
{
    std::uint8_t scratch[256];
    std::size_t dropped = 0;
    while (dropped < kMaxDiscardBytes && _line->available()) {
        const std::size_t n = _line->read(scratch, sizeof(scratch));
        if (n == 0) {
            break;
        }
        dropped += n;
    }
}

void CradleChannel::settle() const
{
    if (_timing.settleMs > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(_timing.settleMs));
    }
}

bool CradleChannel::claim(std::uint32_t baud)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_lent) {
            return true;
        }
        // Scan what FujiBus has not read yet: a request waiting there means
        // a host is active, and it must still reach FujiBus.
        if (_pending.empty()) {
            std::uint8_t scratch[256];
            while (_pending.size() < kMaxDiscardBytes && _line->available()) {
                const std::size_t n = _line->read(scratch, sizeof(scratch));
                if (n == 0) {
                    break;
                }
                scan(scratch, n, _now());
                _pending.insert(_pending.end(), scratch, scratch + n);
            }
        }
        if (_now() < _holdUntilMs) {
            ++_stats.refused;
            return false;
        }
        _busBaud = _baud.baud_rate();
        if (!_baud.set_baud_rate(baud)) {
            _baud.set_baud_rate(_busBaud);
            return false;
        }
        _pending.clear();
        discard_pending();
        _inFrame = false;
        _lent = true;
        ++_stats.lends;
    }
    settle();
    return true;
}

void CradleChannel::release()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_lent) {
        return;
    }
    _baud.set_baud_rate(_busBaud);
    discard_pending();
    _lent = false;
}

bool CradleChannel::lent() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _lent;
}

bool CradleChannel::host_active() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _now() < _holdUntilMs;
}

CradleStats CradleChannel::stats() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _stats;
}

int CradleChannel::LentLink::read(std::uint8_t* buf, std::size_t len, std::uint32_t timeoutMs)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (true) {
        {
            std::lock_guard<std::mutex> lock(_owner._mutex);
            if (!_owner._lent) {
                return -1;
            }
            if (_owner._line->available()) {
                const std::size_t n = _owner._line->read(buf, len);
                if (n > 0) {
                    return static_cast<int>(n);
                }
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return 0;
        }
        std::this_thread::sleep_for(kPoll);
    }
}

int CradleChannel::LentLink::write(const std::uint8_t* buf, std::size_t len)
{
    std::lock_guard<std::mutex> lock(_owner._mutex);
    if (!_owner._lent) {
        return -1;
    }
    _owner._line->write(buf, len);
    return static_cast<int>(len);
}

void CradleChannel::LentLink::set_baud_rate(std::uint32_t baud)
{
    {
        std::lock_guard<std::mutex> lock(_owner._mutex);
        if (!_owner._lent) {
            return;
        }
        _owner._baud.set_baud_rate(baud);
    }
    _owner.settle();
}

} // namespace fujinet::hotsync
