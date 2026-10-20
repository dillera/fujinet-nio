#pragma once

#include <chrono>
#include <vector>

#include "fujinet/io/core/io_message.h"
#include "fujinet/io/core/request_handler.h"
#include "fujinet/io/core/work_source.h"
#include "fujinet/io/transport/transport.h"

namespace fujinet::io {

// Owns a set of transports and pumps IORequests through an IRequestHandler.
class IOService {
public:
    explicit IOService(IRequestHandler& handler)
        : _handler(handler)
    {}

    // We don't own the transports; lifetime is managed externally.
    void addTransport(ITransport* transport) {
        if (transport) {
            _transports.push_back(transport);
        }
    }

    void addWorkSource(IWorkSource* source) {
        if (source) _workSources.push_back(source);
    }

    // One "tick" of the service loop.
    // - Let transports poll
    // - Pull all available requests
    // - Route them through the handler
    // - Send responses back via the same transport
    void serviceOnce();

    // Wait until any transport can make progress, up to timeout.
    // Returns false when no registered transport has a waitable source or when
    // the wait timed out. Callers own any fallback sleep policy.
    bool hasWaitableWorkSource() const;
    bool waitForWork(std::chrono::milliseconds timeout);

private:
    IRequestHandler&              _handler;
    std::vector<ITransport*>      _transports;
    std::vector<IWorkSource*>      _workSources;
};

} // namespace fujinet::io
