#pragma once

#include "fujinet/io/core/io_message.h"
#include "fujinet/io/protocol/fuji_bus_packet.h"

namespace fujinet::io::protocol {

// Convert a complete, decoded FujiBus packet into the core request contract.
// No byte-stream framing belongs here.
bool decode_fujibus_request(const ByteBuffer& raw, RequestID id, IORequest& out);

// Convert a core response into one complete, decoded FujiBus packet.  The
// status convention is shared by every FujiBus transport: U8 param[0].
ByteBuffer encode_fujibus_response(const IOResponse& response);

} // namespace fujinet::io::protocol
