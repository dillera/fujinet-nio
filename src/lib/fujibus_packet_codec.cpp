#include "fujinet/io/protocol/fujibus_packet_codec.h"

#include "fujinet/io/protocol/wire_device_ids.h"

namespace fujinet::io::protocol {

bool decode_fujibus_request(const ByteBuffer& raw, RequestID id, IORequest& out)
{
    auto packet = FujiBusPacket::fromRaw(raw);
    if (!packet) return false;

    out.id = id;
    out.deviceId = static_cast<DeviceID>(packet->device());
    out.type = RequestType::Command;
    out.command = static_cast<std::uint16_t>(packet->command());
    out.params.clear();
    out.params.reserve(packet->paramCount());
    for (unsigned int i = 0; i < packet->paramCount(); ++i) {
        out.params.push_back(packet->param(i));
    }
    out.payload.clear();
    if (const auto& data = packet->data()) {
        out.payload.insert(out.payload.end(), data->begin(), data->end());
    }
    return true;
}

ByteBuffer encode_fujibus_response(const IOResponse& response)
{
    ByteBuffer data(response.payload.begin(), response.payload.end());
    FujiBusPacket packet(static_cast<WireDeviceId>(response.deviceId),
                         static_cast<std::uint8_t>(response.command & 0xFF));
    packet.addParamU8(static_cast<std::uint8_t>(response.status)).setData(std::move(data));
    return packet.serializeRaw();
}

} // namespace fujinet::io::protocol
