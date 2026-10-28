#include "doctest.h"

#if defined(FN_PLATFORM_POSIX) && defined(FN_WITH_CURL) && FN_WITH_CURL && !defined(_WIN32)

#include "fujinet/platform/posix/http_network_protocol_curl.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <thread>

namespace {

// A one-shot HTTP server on loopback: waits for the request, pauses, sends the headers, pauses, sends a
// short body and closes. The pauses are what make "not ready yet" observable.
struct SlowHttpServer {
    int listen_fd = -1;
    std::uint16_t port = 0;
    std::thread th;

    bool start(int pause_ms)
    {
        listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd < 0) return false;
        int opt = 1;
        (void)::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;
        socklen_t len = sizeof(addr);
        if (::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return false;
        port = ntohs(addr.sin_port);
        if (::listen(listen_fd, 1) != 0) return false;
        th = std::thread([this, pause_ms] {
            const int fd = ::accept(listen_fd, nullptr, nullptr);
            if (fd < 0) return;
            char buf[1024];
            (void)::recv(fd, buf, sizeof(buf), 0);   // the request
            std::this_thread::sleep_for(std::chrono::milliseconds(pause_ms));
            const std::string head = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\n";
            (void)::send(fd, head.data(), head.size(), 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(pause_ms));
            (void)::send(fd, "abcd", 4, 0);
            ::close(fd);
        });
        return true;
    }

    ~SlowHttpServer()
    {
        if (listen_fd >= 0) ::shutdown(listen_fd, SHUT_RDWR);
        if (th.joinable()) th.join();
        if (listen_fd >= 0) ::close(listen_fd);
    }
};

} // namespace

TEST_CASE("curl HTTP backend reports readiness only once the transfer has something to give")
{
    SlowHttpServer server;
    REQUIRE(server.start(300));

    fujinet::platform::posix::HttpNetworkProtocolCurl proto;
    fujinet::io::NetworkOpenRequest req;
    req.method = 1;
    req.url = "http://127.0.0.1:" + std::to_string(server.port) + "/slow";
    REQUIRE(proto.open(req) == fujinet::io::StatusCode::Ok);

    // Before the server answers: nothing to read, no status.
    proto.poll();
    CHECK_FALSE(proto.info_ready());
    CHECK_FALSE(proto.read_ready());

    // Poll as the device tick does until it turns ready (bounded).
    bool ready = false;
    for (int i = 0; i < 200 && !ready; ++i) {
        proto.poll();
        ready = proto.read_ready() || proto.info_ready();
        if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    REQUIRE(ready);

    // When ready, the matching call no longer says NotReady.
    for (int i = 0; i < 100 && !proto.info_ready(); ++i) {
        proto.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    fujinet::io::NetworkInfo info;
    CHECK(proto.info(info) == fujinet::io::StatusCode::Ok);
    CHECK(info.hasHttpStatus);
    CHECK(info.httpStatus == 200);

    std::uint8_t body[16] = {};
    std::uint16_t read = 0;
    bool eof = false;
    bool more = false;
    CHECK(proto.read_body(0, body, sizeof(body), read, eof, more) == fujinet::io::StatusCode::Ok);
    CHECK(read == 4);
    CHECK(std::string(reinterpret_cast<char*>(body), read) == "abcd");
    CHECK(eof);
    proto.poll();
    CHECK(proto.read_ready());   // at the end of the stream a read answers (end of stream), not NotReady
    proto.close();
}

#endif
