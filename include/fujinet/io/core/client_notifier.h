#pragma once

namespace fujinet::io {

// How a device tells the client that issued its requests that something it asked about is now
// available: a request that returned NotReady (a read with no data yet) may succeed if repeated.
// `owner` is the IORequest::clientOwner of the client the session belongs to. It is a hint only: the
// client is always free to poll instead, and a notification can be early, repeated or for data another
// request already took.
class IClientNotifier {
public:
    virtual ~IClientNotifier() = default;
    virtual void clientReadable(const void* owner) = 0;
};

} // namespace fujinet::io
