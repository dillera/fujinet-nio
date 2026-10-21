#include "fujinet/io/atari_sio_nio_frontend.h"

#include "fujinet/io/protocol/fujibus_packet_codec.h"

namespace fujinet::io {
namespace {

constexpr std::uint8_t command_query = 'Q';
constexpr std::uint8_t command_write = 'W';
constexpr std::uint8_t command_poll = 'P';
constexpr std::uint8_t command_read = 'R';

constexpr std::uint8_t wire_revision = 1;
constexpr std::uint8_t feature_flags = 0;

} // namespace

std::uint8_t AtariSioNioFrontend::checksum(const std::uint8_t* data, std::size_t len)
{
    std::uint8_t result = 0;
    for (std::size_t i = 0; data && i < len; ++i) {
        const std::uint16_t sum = static_cast<std::uint16_t>(result) + data[i];
        result = static_cast<std::uint8_t>((sum & 0xffU) + (sum >> 8U));
    }
    return result;
}

AtariSioNioFrontend::Result AtariSioNioFrontend::reject() const
{
    return Result{{nak}, 0, false};
}

AtariSioNioFrontend::Result AtariSioNioFrontend::data_reply(const std::vector<std::uint8_t>& data) const
{
    std::vector<std::uint8_t> bytes{ack, complete};
    bytes.insert(bytes.end(), data.begin(), data.end());
    bytes.push_back(checksum(data.data(), data.size()));
    return Result{std::move(bytes), 0, true};
}

std::uint16_t AtariSioNioFrontend::aux(const std::array<std::uint8_t, command_bytes>& command) const
{
    return static_cast<std::uint16_t>(command[2]) |
           static_cast<std::uint16_t>(command[3] << 8U);
}

std::vector<std::uint8_t> AtariSioNioFrontend::bounded_error_response(const IORequest& request) const
{
    IOResponse error = make_base_response(request, StatusCode::InternalError);
    return protocol::encode_fujibus_response(error);
}

AtariSioNioFrontend::Result AtariSioNioFrontend::begin(
    const std::array<std::uint8_t, command_bytes>& command)
{
    if (command[0] != device_id || checksum(command.data(), command.size() - 1) != command.back()) {
        return reject();
    }

    const auto length = aux(command);
    switch (command[1]) {
    case command_query:
        if (_state != State::Idle || length != 0) return reject();
        return data_reply({wire_revision,
                           static_cast<std::uint8_t>(capacity & 0xffU),
                           static_cast<std::uint8_t>(capacity >> 8U),
                           feature_flags});

    case command_write:
        if (_state != State::Idle || length == 0 || length > capacity) return reject();
        _state = State::Receiving;
        _write_bytes = length;
        return Result{{ack}, _write_bytes, true};

    case command_poll: {
        if (length != 0) return reject();
        std::vector<std::uint8_t> state(3, 0);
        if (_state == State::Receiving) {
            state[0] = 1;
        } else if (_state == State::Ready) {
            state[0] = 2;
            state[1] = static_cast<std::uint8_t>(_response.size() & 0xffU);
            state[2] = static_cast<std::uint8_t>(_response.size() >> 8U);
        }
        return data_reply(state);
    }

    case command_read: {
        if (_state != State::Ready || length != _response.size()) return reject();
        // The reply owns its copy before the transaction slot is consumed.  An
        // ingress may transmit Result::bytes after this function returns.
        std::vector<std::uint8_t> reply = _response;
        _response.clear();
        _state = State::Idle;
        return data_reply(reply);
    }

    default:
        return reject();
    }
}

AtariSioNioFrontend::Result AtariSioNioFrontend::complete_write(const std::uint8_t* data,
                                                                  std::size_t data_bytes)
{
    if (_state != State::Receiving || !data || data_bytes != _write_bytes + 1 ||
        checksum(data, _write_bytes) != data[_write_bytes]) {
        reset();
        return reject();
    }

    protocol::ByteBuffer raw(data, data + _write_bytes);
    IORequest request;
    if (!protocol::decode_fujibus_request(raw, _next_request_id, request)) {
        reset();
        return reject();
    }
    ++_next_request_id;

    std::vector<std::uint8_t> response =
        protocol::encode_fujibus_response(_handler.handleRequest(request));
    if (response.empty() || response.size() > capacity) {
        response = bounded_error_response(request);
    }
    if (response.empty() || response.size() > capacity) {
        reset();
        return reject();
    }

    _write_bytes = 0;
    _response = std::move(response);
    _state = State::Ready;
    return Result{{ack, complete}, 0, true};
}

void AtariSioNioFrontend::abort_write()
{
    if (_state == State::Receiving) reset();
}

void AtariSioNioFrontend::reset()
{
    _state = State::Idle;
    _write_bytes = 0;
    _response.clear();
}

} // namespace fujinet::io
