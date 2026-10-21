#include "fujinet/io/atari_sio_ingress_router.h"

namespace fujinet::io {

AtariSioIngressRouter::AtariSioIngressRouter(AtariSioDiskFrontend& disk,
                                               AtariSioNioFrontend& nio,
                                               Send send)
    : _disk(disk)
    , _nio(nio)
    , _send(std::move(send))
{}

void AtariSioIngressRouter::start_write(PendingWrite kind, std::size_t bytes)
{
    _pending = kind;
    _write_expected = bytes;
    _write.clear();
    _write.reserve(bytes + 1);
}

void AtariSioIngressRouter::complete_write()
{
    std::vector<std::uint8_t> reply;
    if (_pending == PendingWrite::Disk) {
        reply = _disk.complete_write(_write.data(), _write.size()).bytes;
    } else {
        reply = _nio.complete_write(_write.data(), _write.size()).bytes;
    }
    _pending = PendingWrite::None;
    _write_expected = 0;
    _write.clear();
    _send(reply);
}

void AtariSioIngressRouter::begin_command()
{
    const auto command = _command;
    _command_used = 0;
    if (command[0] == 0x7f) {
        const auto result = _nio.begin(command);
        _send(result.bytes);
        if (result.accepted && result.expected_write_bytes != 0) {
            start_write(PendingWrite::Nio, result.expected_write_bytes);
        }
        return;
    }

    const auto result = _disk.begin(command);
    _send(result.bytes);
    if (result.accepted && result.expected_write_bytes != 0) {
        start_write(PendingWrite::Disk, result.expected_write_bytes);
    }
}

void AtariSioIngressRouter::consume(std::uint8_t byte)
{
    if (_pending != PendingWrite::None) {
        _write.push_back(byte);
        if (_write.size() == _write_expected + 1) complete_write();
        return;
    }

    _command[_command_used++] = byte;
    if (_command_used == _command.size()) begin_command();
}

void AtariSioIngressRouter::reset_command_collection()
{
    // A new command aborts only an incomplete W data phase.  Once a response
    // is ready, no data phase is pending and its terminal R remains available.
    if (_pending == PendingWrite::Nio) _nio.abort_write();
    _pending = PendingWrite::None;
    _write_expected = 0;
    _write.clear();
    _command_used = 0;
}

} // namespace fujinet::io
