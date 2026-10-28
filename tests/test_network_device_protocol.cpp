// tests/test_network_device_protocol.cpp

#include "doctest.h"
#include "image_fixtures.h"
#include "net_device_test_helpers.h"
#include "fujinet/io/core/io_device_manager.h"

#include <algorithm>
#include <cstring>

using namespace fujinet::tests::netdev;

TEST_CASE("NetworkDevice reset reclaims only sessions owned by the resetting ingress")
{
    NetworkDevice dev(make_stub_registry_http_only());
    const auto deviceId = to_device_id(WireDeviceId::NetworkService);
    int clientA = 0, clientB = 0;
    auto open = [&](const void* owner, std::uint8_t method, std::uint8_t flags) {
        IORequest req{};
        req.deviceId = deviceId;
        req.clientOwner = owner;
        req.command = 1;
        req.payload = to_vec(open_payload("http://example.com/reset", method, flags, 0, {}));
        auto resp = dev.handle(req);
        REQUIRE(resp.status == StatusCode::Ok);
        REQUIRE(resp.payload.size() >= 6);
        return static_cast<std::uint16_t>(resp.payload[4] | (resp.payload[5] << 8));
    };
    const auto readA = open(&clientA, 1, 0);
    const auto outputA = open(&clientA, 3, 4);
    const auto readB = open(&clientB, 1, 0);
    const auto unowned = open(nullptr, 1, 0);
    dev.resetClient(nullptr);
    CHECK(info_req(dev, deviceId, readA).status == StatusCode::Ok);
    dev.resetClient(&clientA);
    CHECK(info_req(dev, deviceId, readA).status == StatusCode::InvalidRequest);
    CHECK(info_req(dev, deviceId, outputA).status == StatusCode::InvalidRequest);
    CHECK(info_req(dev, deviceId, readB).status == StatusCode::Ok);
    CHECK(info_req(dev, deviceId, unowned).status == StatusCode::Ok);
    dev.resetClient(&clientA);
    CHECK(info_req(dev, deviceId, readB).status == StatusCode::Ok);
    const auto replacement = open(&clientB, 1, 0);
    dev.resetClient(&clientA);
    CHECK(info_req(dev, deviceId, replacement).status == StatusCode::Ok);
    for (unsigned i = 0; i < 8; ++i) {
        const auto next = open(&clientA, 1, 0);
        dev.resetClient(&clientA);
        CHECK(info_req(dev, deviceId, next).status == StatusCode::InvalidRequest);
        CHECK(info_req(dev, deviceId, readB).status == StatusCode::Ok);
    }
}

TEST_CASE("NetworkDevice v1: Open -> Info -> Read -> Close (stub backend)")
{
    auto reg = make_stub_registry_http_only();
    NetworkDevice dev(std::move(reg));

    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    // ---- Open ----
    // Request only the "Server" response header; otherwise headers are not stored.
    std::uint16_t handle = open_handle_stub(
        dev,
        deviceId,
        "http://example.com/hello",
        /*method=*/1,
        /*flags=*/0,
        /*bodyLenHint=*/0,
        { "Server" }
    );


    // ---- Info ----
    {
        IOResponse iresp = info_req(dev, deviceId, handle);
        CHECK(iresp.status == StatusCode::Ok);

        netproto::Reader ir(iresp.payload.data(), iresp.payload.size());

        std::uint8_t iver = 0, iflags = 0;
        std::uint16_t ires = 0, ihandle = 0, httpStatus = 0;
        std::uint64_t contentLength = 0;
        std::uint32_t hdrLen = 0;

        REQUIRE(ir.read_u8(iver));
        REQUIRE(ir.read_u8(iflags));
        REQUIRE(ir.read_u16le(ires));
        REQUIRE(ir.read_u16le(ihandle));
        REQUIRE(ir.read_u16le(httpStatus));
        REQUIRE(ir.read_u64le(contentLength));
        REQUIRE(ir.read_u32le(hdrLen));

        CHECK(iver == V);
        CHECK(ihandle == handle);
        CHECK(httpStatus == 200);
        CHECK((iflags & 0x02) != 0); // hasContentLength
        CHECK((iflags & 0x04) != 0); // hasHttpStatus
        CHECK(hdrLen > 0);

        CHECK(ir.remaining() == 0);

        IOResponse hresp = info_read_req(dev, deviceId, handle, 0, 1024);
        CHECK(hresp.status == StatusCode::Ok);
        netproto::Reader hr(hresp.payload.data(), hresp.payload.size());
        std::uint8_t hver = 0, hflags = 0;
        std::uint16_t hres = 0, hhandle = 0, hlen = 0;
        std::uint32_t hoff = 0;
        REQUIRE(hr.read_u8(hver));
        REQUIRE(hr.read_u8(hflags));
        REQUIRE(hr.read_u16le(hres));
        REQUIRE(hr.read_u16le(hhandle));
        REQUIRE(hr.read_u32le(hoff));
        REQUIRE(hr.read_u16le(hlen));
        CHECK(hver == V);
        CHECK(hhandle == handle);
        CHECK(hoff == 0);
        CHECK((hflags & 0x01) != 0);
        const std::uint8_t* hdrPtr = nullptr;
        REQUIRE(hr.read_bytes(hdrPtr, hlen));
        std::string hdr(reinterpret_cast<const char*>(hdrPtr), hlen);
        CHECK(hdr.find("Server:") != std::string::npos);
    }

    // ---- Read ----
    {
        IOResponse rresp = read_req(dev, deviceId, handle, 0, 1024);
        CHECK(rresp.status == StatusCode::Ok);

        netproto::Reader rr(rresp.payload.data(), rresp.payload.size());

        std::uint8_t rver = 0, rflags = 0;
        std::uint16_t rres = 0, rhandle = 0;
        std::uint32_t offEcho = 0;
        std::uint16_t dataLen = 0;

        REQUIRE(rr.read_u8(rver));
        REQUIRE(rr.read_u8(rflags));
        REQUIRE(rr.read_u16le(rres));
        REQUIRE(rr.read_u16le(rhandle));
        REQUIRE(rr.read_u32le(offEcho));
        REQUIRE(rr.read_u16le(dataLen));

        CHECK(rver == V);
        CHECK(rhandle == handle);
        CHECK(offEcho == 0);
        CHECK(dataLen > 0);

        const std::uint8_t* dataPtr = nullptr;
        REQUIRE(rr.read_bytes(dataPtr, dataLen));

        std::string body(reinterpret_cast<const char*>(dataPtr), dataLen);
        CHECK(body.find("stub response for: http://example.com/hello") != std::string::npos);

        // eof should be set (stub body is small)
        CHECK((rflags & 0x01) != 0);
    }

    // ---- Close ----
    {
        IOResponse cresp = close_req(dev, deviceId, handle);
        CHECK(cresp.status == StatusCode::Ok);
    }

    // ---- Info after close should be InvalidRequest ----
    {
        IOResponse iresp = info_req(dev, deviceId, handle);
        CHECK(iresp.status == StatusCode::InvalidRequest);
    }
}

TEST_CASE("NetworkDevice v1: Read sets more_available when additional bytes remain immediately readable")
{
    auto reg = make_stub_registry_http_only();
    NetworkDevice dev(std::move(reg));

    const auto deviceId = to_device_id(WireDeviceId::NetworkService);
    std::uint16_t handle = open_handle_stub(
        dev,
        deviceId,
        "http://example.com/chunk",
        /*method=*/1,
        /*flags=*/0,
        /*bodyLenHint=*/0
    );

    IOResponse rresp = read_req(dev, deviceId, handle, 0, 4);
    CHECK(rresp.status == StatusCode::Ok);

    netproto::Reader rr(rresp.payload.data(), rresp.payload.size());
    std::uint8_t rver = 0, rflags = 0;
    std::uint16_t rres = 0, rhandle = 0, dataLen = 0;
    std::uint32_t offEcho = 0;

    REQUIRE(rr.read_u8(rver));
    REQUIRE(rr.read_u8(rflags));
    REQUIRE(rr.read_u16le(rres));
    REQUIRE(rr.read_u16le(rhandle));
    REQUIRE(rr.read_u32le(offEcho));
    REQUIRE(rr.read_u16le(dataLen));

    CHECK(rver == V);
    CHECK(rhandle == handle);
    CHECK(offEcho == 0);
    CHECK(dataLen == 4);
    CHECK((rflags & 0x01) == 0);
    CHECK((rflags & 0x02) != 0);
    CHECK((rflags & 0x04) != 0);
}

TEST_CASE("NetworkDevice v1: response headers are only returned when requested (allowlist)") {
    using namespace fujinet::tests::netdev;

    auto reg = make_stub_registry_http_only();
    NetworkDevice dev(reg);
    const std::uint16_t deviceId = to_device_id(WireDeviceId::NetworkService);

    // 1) No allowlist: headers must be absent.
    {
        const std::uint16_t h = open_handle_stub(
            dev, deviceId, "http://example.com/hello",
            /*method=*/1, /*flags=*/0, /*bodyLenHint=*/0,
            {} // no response headers requested
        );

        IOResponse iresp = info_req(dev, deviceId, h);
        REQUIRE(iresp.status == StatusCode::Ok);

        netproto::Reader r(iresp.payload.data(), iresp.payload.size());

        std::uint8_t ver = 0, flags = 0;
        std::uint16_t reserved = 0, handle = 0;
        std::uint16_t httpStatus = 0;
        std::uint64_t contentLen = 0;
        std::uint32_t hdrLen = 0;

        REQUIRE(r.read_u8(ver));
        REQUIRE(r.read_u8(flags));
        REQUIRE(r.read_u16le(reserved));
        REQUIRE(r.read_u16le(handle));
        REQUIRE(r.read_u16le(httpStatus));
        REQUIRE(r.read_u64le(contentLen));
        REQUIRE(r.read_u32le(hdrLen));

        CHECK(ver == V);
        CHECK(handle == h);
        CHECK(hdrLen == 0);
        CHECK((flags & 0x01) == 0); // headersIncluded must be false

        close_req(dev, deviceId, h);
    }

    // 2) Allowlist: only requested headers should be returned.
    {
        const std::uint16_t h = open_handle_stub(
            dev, deviceId, "http://example.com/hello",
            /*method=*/1, /*flags=*/0, /*bodyLenHint=*/0,
            { "Server" } // request only Server
        );

        IOResponse iresp = info_req(dev, deviceId, h);
        REQUIRE(iresp.status == StatusCode::Ok);

        netproto::Reader r(iresp.payload.data(), iresp.payload.size());

        std::uint8_t ver = 0, flags = 0;
        std::uint16_t reserved = 0, handle = 0;
        std::uint16_t httpStatus = 0;
        std::uint64_t contentLen = 0;
        std::uint32_t hdrLen = 0;

        REQUIRE(r.read_u8(ver));
        REQUIRE(r.read_u8(flags));
        REQUIRE(r.read_u16le(reserved));
        REQUIRE(r.read_u16le(handle));
        REQUIRE(r.read_u16le(httpStatus));
        REQUIRE(r.read_u64le(contentLen));
        REQUIRE(r.read_u32le(hdrLen));

        REQUIRE(ver == V);
        REQUIRE(handle == h);
        REQUIRE((flags & 0x01) != 0); // headersIncluded must be true
        REQUIRE(hdrLen > 0);

        CHECK(r.remaining() == 0);

        IOResponse hresp = info_read_req(dev, deviceId, h, 0, 1024);
        REQUIRE(hresp.status == StatusCode::Ok);
        netproto::Reader hr(hresp.payload.data(), hresp.payload.size());
        std::uint8_t hver = 0, hflags = 0;
        std::uint16_t hres = 0, hhandle = 0, hlen = 0;
        std::uint32_t hoff = 0;
        REQUIRE(hr.read_u8(hver));
        REQUIRE(hr.read_u8(hflags));
        REQUIRE(hr.read_u16le(hres));
        REQUIRE(hr.read_u16le(hhandle));
        REQUIRE(hr.read_u32le(hoff));
        REQUIRE(hr.read_u16le(hlen));
        const std::uint8_t* hdrPtr = nullptr;
        REQUIRE(hr.read_bytes(hdrPtr, hlen));
        std::string hdrs(reinterpret_cast<const char*>(hdrPtr), hlen);

        CHECK(hdrs.find("Server:") != std::string::npos);
        CHECK(hdrs.find("Content-Type:") == std::string::npos); // must not be included

        close_req(dev, deviceId, h);
    }

    // 3) Case-insensitive match (client asks "server" lower-case)
    {
        const std::uint16_t h = open_handle_stub(
            dev, deviceId, "http://example.com/hello",
            /*method=*/1, /*flags=*/0, /*bodyLenHint=*/0,
            { "server" }
        );

        IOResponse iresp = info_req(dev, deviceId, h);
        REQUIRE(iresp.status == StatusCode::Ok);

        netproto::Reader r(iresp.payload.data(), iresp.payload.size());

        std::uint8_t ver = 0, flags = 0;
        std::uint16_t reserved = 0, handle = 0;
        std::uint16_t httpStatus = 0;
        std::uint64_t contentLen = 0;
        std::uint32_t hdrLen = 0;

        REQUIRE(r.read_u8(ver));
        REQUIRE(r.read_u8(flags));
        REQUIRE(r.read_u16le(reserved));
        REQUIRE(r.read_u16le(handle));
        REQUIRE(r.read_u16le(httpStatus));
        REQUIRE(r.read_u64le(contentLen));
        REQUIRE(r.read_u32le(hdrLen));

        REQUIRE((flags & 0x01) != 0);
        REQUIRE(hdrLen > 0);

        IOResponse hresp = info_read_req(dev, deviceId, h, 0, 1024);
        REQUIRE(hresp.status == StatusCode::Ok);
        netproto::Reader hr(hresp.payload.data(), hresp.payload.size());
        std::uint8_t hver = 0, hflags = 0;
        std::uint16_t hres = 0, hhandle = 0, hlen = 0;
        std::uint32_t hoff = 0;
        REQUIRE(hr.read_u8(hver));
        REQUIRE(hr.read_u8(hflags));
        REQUIRE(hr.read_u16le(hres));
        REQUIRE(hr.read_u16le(hhandle));
        REQUIRE(hr.read_u32le(hoff));
        REQUIRE(hr.read_u16le(hlen));
        const std::uint8_t* hdrPtr = nullptr;
        REQUIRE(hr.read_bytes(hdrPtr, hlen));
        std::string hdrs(reinterpret_cast<const char*>(hdrPtr), hlen);
        
        CHECK(hdrs.find("Server:") != std::string::npos);

        close_req(dev, deviceId, h);
    }
}

TEST_CASE("NetworkDevice v1: Write (POST) returns writtenLen via stub backend")
{
    auto reg = make_stub_registry_http_only();
    NetworkDevice dev(std::move(reg));

    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    // ---- Open POST ----
    std::uint16_t handle = 0;
    {
        std::string p;
        netproto::write_u8(p, V); // version
        netproto::write_u8(p, 2); // method=POST
        netproto::write_u8(p, 0); // flags
        netproto::write_lp_u16_string(p, "http://example.com/post");
        netproto::write_u16le(p, 0); // headerCount
        netproto::write_u32le(p, 4); // bodyLenHint
        netproto::write_u16le(p, 0); // respHeaderCount (store no response headers)

        IORequest req{};
        req.id = 10;
        req.deviceId = deviceId;
        req.command = 0x01; // Open
        req.payload = to_vec(p);

        IOResponse resp = dev.handle(req);
        REQUIRE(resp.status == StatusCode::Ok);

        netproto::Reader r(resp.payload.data(), resp.payload.size());

        std::uint8_t ver = 0, flags = 0;
        std::uint16_t reserved = 0;

        REQUIRE(r.read_u8(ver));
        REQUIRE(r.read_u8(flags));
        REQUIRE(r.read_u16le(reserved));
        REQUIRE(r.read_u16le(handle));

        CHECK(ver == V);
        CHECK((flags & 0x01) != 0); // accepted
        CHECK((flags & 0x02) != 0); // needs_body_write
        CHECK(handle != 0);
    }

    // ---- Write ----
    {
        IOResponse wresp = write_req(dev, deviceId, handle, 0, "ABCD");
        REQUIRE(wresp.status == StatusCode::Ok);

        netproto::Reader wr(wresp.payload.data(), wresp.payload.size());

        std::uint8_t ver = 0, flags = 0;
        std::uint16_t reserved = 0, h = 0, written = 0;
        std::uint32_t offEcho = 0;

        REQUIRE(wr.read_u8(ver));
        REQUIRE(wr.read_u8(flags));
        REQUIRE(wr.read_u16le(reserved));
        REQUIRE(wr.read_u16le(h));
        REQUIRE(wr.read_u32le(offEcho));
        REQUIRE(wr.read_u16le(written));

        CHECK(ver == V);
        CHECK(h == handle);
        CHECK(offEcho == 0);
        CHECK(written == 4);
    }

    // ---- Close ----
    {
        IOResponse cresp = close_req(dev, deviceId, handle);
        CHECK(cresp.status == StatusCode::Ok);
    }
}

TEST_CASE("NetworkDevice v1: POST unknown-length body commits on zero-length Write()")
{
    auto reg = make_stub_registry_http_only();
    NetworkDevice dev(std::move(reg));

    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    // Open POST with bodyLenHint==0 and the "unknown-length body" flag set (bit2).
    const std::uint16_t handle = open_handle_stub(
        dev,
        deviceId,
        "http://example.com/post",
        /*method=*/2,
        /*flags=*/0x04,
        /*bodyLenHint=*/0,
        {}
    );

    // Before commit, Info/Read are gated.
    CHECK(info_req(dev, deviceId, handle).status == StatusCode::NotReady);
    CHECK(read_req(dev, deviceId, handle, 0, 256).status == StatusCode::NotReady);

    // Write some body bytes.
    {
        IOResponse w = write_req(dev, deviceId, handle, 0, "abc");
        CHECK(w.status == StatusCode::Ok);
    }

    CHECK(info_req(dev, deviceId, handle).status == StatusCode::NotReady);

    // Commit body by sending a zero-length Write at the next offset.
    {
        IOResponse w = write_req(dev, deviceId, handle, 3, std::string_view{});
        CHECK(w.status == StatusCode::Ok);
    }

    // After commit, Info/Read should be available (subject to backend readiness).
    CHECK(info_req(dev, deviceId, handle).status == StatusCode::Ok);
    CHECK(read_req(dev, deviceId, handle, 0, 256).status == StatusCode::Ok);

    CHECK(close_req(dev, deviceId, handle).status == StatusCode::Ok);
}

// -----------------------------------------------------------------------------
// Conformance tests (session semantics + StatusCode contract basics)
// -----------------------------------------------------------------------------

TEST_CASE("Conformance: unknown handle => InvalidRequest (Info/Read/Write/Close)")
{
    auto reg = make_stub_registry_http_only();
    NetworkDevice dev(std::move(reg));

    const auto deviceId = to_device_id(WireDeviceId::NetworkService);
    const std::uint16_t badHandle = 0x1234;

    CHECK(info_req(dev, deviceId, badHandle).status == StatusCode::InvalidRequest);
    CHECK(read_req(dev, deviceId, badHandle, 0, 16).status == StatusCode::InvalidRequest);
    CHECK(write_req(dev, deviceId, badHandle, 0, "AB").status == StatusCode::InvalidRequest);
    CHECK(close_req(dev, deviceId, badHandle).status == StatusCode::InvalidRequest);
}

TEST_CASE("Conformance: handle is invalid after Close, and handle generation changes on reuse")
{
    auto reg = make_stub_registry_http_only();
    NetworkDevice dev(std::move(reg));

    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    const std::uint16_t h1 = open_handle_stub(dev, deviceId, "http://example.com/a");
    REQUIRE(close_req(dev, deviceId, h1).status == StatusCode::Ok);

    // Old handle must now be rejected
    CHECK(info_req(dev, deviceId, h1).status == StatusCode::InvalidRequest);

    // Next open should not produce the same handle token (generation should change)
    const std::uint16_t h2 = open_handle_stub(dev, deviceId, "http://example.com/b");
    CHECK(h2 != h1);

    REQUIRE(close_req(dev, deviceId, h2).status == StatusCode::Ok);
}

TEST_CASE("Conformance: Open malformed URL => InvalidRequest")
{
    auto reg = make_stub_registry_http_only();
    NetworkDevice dev(std::move(reg));

    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    std::string p;
    netproto::write_u8(p, V);
    netproto::write_u8(p, 1); // GET
    netproto::write_u8(p, 0); // flags
    netproto::write_lp_u16_string(p, "example.com/no-scheme"); // malformed: missing scheme
    netproto::write_u16le(p, 0);
    netproto::write_u32le(p, 0);
    netproto::write_u16le(p, 0); // respHeaderCount (store no response headers)

    IORequest req{};
    req.id = 600;
    req.deviceId = deviceId;
    req.command = 0x01; // Open
    req.payload = to_vec(p);

    IOResponse resp = dev.handle(req);
    CHECK(resp.status == StatusCode::InvalidRequest);
}

TEST_CASE("Conformance: Open unsupported scheme => Unsupported")
{
    auto reg = make_stub_registry_http_only();
    NetworkDevice dev(std::move(reg));

    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    std::string p;
    netproto::write_u8(p, V);
    netproto::write_u8(p, 1); // GET
    netproto::write_u8(p, 0); // flags
    netproto::write_lp_u16_string(p, "tcp://example.com:80/");
    netproto::write_u16le(p, 0);
    netproto::write_u32le(p, 0);
    netproto::write_u16le(p, 0); // respHeaderCount (store no response headers)

    IORequest req{};
    req.id = 700;
    req.deviceId = deviceId;
    req.command = 0x01; // Open
    req.payload = to_vec(p);

    IOResponse resp = dev.handle(req);
    CHECK(resp.status == StatusCode::Unsupported);
}

static constexpr std::uint8_t OPEN_ALLOW_EVICT = 0x08;

TEST_CASE("Conformance: capacity strict (allow_evict=0) => 5th Open returns DeviceBusy")
{
    auto reg = make_stub_registry_http_only();
    NetworkDevice dev(std::move(reg));
    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    // Fill the session pool. (Design constraint: 4 sessions.)
    (void)open_handle_stub(dev, deviceId, "http://example.com/1", 1, /*flags=*/0, 0);
    (void)open_handle_stub(dev, deviceId, "http://example.com/2", 1, /*flags=*/0, 0);
    (void)open_handle_stub(dev, deviceId, "http://example.com/3", 1, /*flags=*/0, 0);
    (void)open_handle_stub(dev, deviceId, "http://example.com/4", 1, /*flags=*/0, 0);

    // 5th open in strict mode must fail with DeviceBusy.
    std::string p;
    netproto::write_u8(p, V);
    netproto::write_u8(p, 1); // GET
    netproto::write_u8(p, 0); // allow_evict=0
    netproto::write_lp_u16_string(p, "http://example.com/5");
    netproto::write_u16le(p, 0);
    netproto::write_u32le(p, 0);
    netproto::write_u16le(p, 0); // respHeaderCount (store no response headers)

    IORequest req{};
    req.id = 999;
    req.deviceId = deviceId;
    req.command = 0x01; // Open
    req.payload = to_vec(p);

    IOResponse resp = dev.handle(req);
    CHECK(resp.status == StatusCode::DeviceBusy);
}

TEST_CASE("Conformance: capacity eviction (allow_evict=1) => oldest handle becomes invalid")
{
    auto reg = make_stub_registry_http_only();
    NetworkDevice dev(std::move(reg));
    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    const std::uint16_t h0 = open_handle_stub(dev, deviceId, "http://example.com/h0", 1, OPEN_ALLOW_EVICT, 0);
    REQUIRE(h0 != 0);

    // Apply pressure: opens should keep succeeding (evicting as needed)
    for (int i = 0; i < 32; ++i) {
        (void)open_handle_stub(
            dev,
            deviceId,
            "http://example.com/p/" + std::to_string(i),
            1,
            OPEN_ALLOW_EVICT,
            0
        );
    }

    // The oldest handle should have been evicted at some point.
    CHECK(info_req(dev, deviceId, h0).status == StatusCode::InvalidRequest);
    CHECK(close_req(dev, deviceId, h0).status == StatusCode::InvalidRequest);
}

TEST_CASE("HTTP body lifecycle: Info/Read are NotReady until POST body fully written")
{
    auto reg = make_stub_registry_http_only();
    NetworkDevice dev(std::move(reg));
    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    // POST with bodyLenHint > 0 => needs body
    const std::uint16_t h = open_handle_stub(dev, deviceId, "http://example.com/post", /*method=*/2, /*flags=*/0, /*bodyLenHint=*/4);

    // Before body complete, response must not be ready
    CHECK(info_req(dev, deviceId, h).status == StatusCode::NotReady);
    CHECK(read_req(dev, deviceId, h, 0, 128).status == StatusCode::NotReady);

    // Write partial body
    CHECK(write_req(dev, deviceId, h, 0, "AB").status == StatusCode::Ok);

    CHECK(info_req(dev, deviceId, h).status == StatusCode::NotReady);
    CHECK(read_req(dev, deviceId, h, 0, 128).status == StatusCode::NotReady);

    // Finish body
    CHECK(write_req(dev, deviceId, h, 2, "CD").status == StatusCode::Ok);

    // Now response is available (stub should allow Info/Read)
    CHECK(info_req(dev, deviceId, h).status == StatusCode::Ok);
    CHECK(read_req(dev, deviceId, h, 0, 128).status == StatusCode::Ok);

    CHECK(close_req(dev, deviceId, h).status == StatusCode::Ok);
}

TEST_CASE("HTTP body lifecycle: non-sequential Write offset => InvalidRequest")
{
    auto reg = make_stub_registry_http_only();
    NetworkDevice dev(std::move(reg));
    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    const std::uint16_t h = open_handle_stub(dev, deviceId, "http://example.com/post", /*method=*/2, /*flags=*/0, /*bodyLenHint=*/4);

    // First write at offset 0 OK
    CHECK(write_req(dev, deviceId, h, 0, "AB").status == StatusCode::Ok);

    // Next write must be offset 2; using offset 3 must fail
    CHECK(write_req(dev, deviceId, h, 3, "C").status == StatusCode::InvalidRequest);

    CHECK(close_req(dev, deviceId, h).status == StatusCode::Ok);
}

TEST_CASE("HTTP body lifecycle: Write overflow beyond bodyLenHint => InvalidRequest")
{
    auto reg = make_stub_registry_http_only();
    NetworkDevice dev(std::move(reg));
    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    const std::uint16_t h = open_handle_stub(dev, deviceId, "http://example.com/post", /*method=*/2, /*flags=*/0, /*bodyLenHint=*/4);

    // Writing 5 bytes when hint is 4 is invalid
    CHECK(write_req(dev, deviceId, h, 0, "ABCDE").status == StatusCode::InvalidRequest);

    CHECK(close_req(dev, deviceId, h).status == StatusCode::Ok);
}

TEST_CASE("HTTP body lifecycle: bodyLenHint>0 on non-POST/PUT => InvalidRequest")
{
    auto reg = make_stub_registry_http_only();
    NetworkDevice dev(std::move(reg));
    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    // GET with bodyLenHint should be rejected (keeps v1 simple + deterministic)
    std::string p;
    netproto::write_u8(p, V);
    netproto::write_u8(p, 1); // GET
    netproto::write_u8(p, 0); // flags
    netproto::write_lp_u16_string(p, "http://example.com/get");
    netproto::write_u16le(p, 0);
    netproto::write_u32le(p, 4); // bodyLenHint on GET => InvalidRequest
    netproto::write_u16le(p, 0); // respHeaderCount (store no response headers)

    IORequest req{};
    req.id = 1234;
    req.deviceId = deviceId;
    req.command = 0x01; // Open
    req.payload = to_vec(p);

    IOResponse resp = dev.handle(req);
    CHECK(resp.status == StatusCode::InvalidRequest);
}

TEST_CASE("NetworkDevice v1: Open-time JSON translation returns translated view")
{
    auto reg = make_stub_registry_http_only();
    NetworkDevice dev(std::move(reg));
    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    const std::uint16_t handle = open_handle_stub(
        dev,
        deviceId,
        "http://example.com/json",
        /*method=*/1,
        /*flags=*/0,
        /*bodyLenHint=*/0,
        {},
        fujinet::io::ContentTranslationType::Json,
        "/url");

    IOResponse iresp = info_req(dev, deviceId, handle);
    REQUIRE(iresp.status == StatusCode::Ok);
    netproto::Reader ir(iresp.payload.data(), iresp.payload.size());

    std::uint8_t iver = 0, iflags = 0;
    std::uint16_t ires = 0, ihandle = 0, httpStatus = 0;
    std::uint64_t contentLength = 0;
    std::uint32_t hdrLen = 0;

    REQUIRE(ir.read_u8(iver));
    REQUIRE(ir.read_u8(iflags));
    REQUIRE(ir.read_u16le(ires));
    REQUIRE(ir.read_u16le(ihandle));
    REQUIRE(ir.read_u16le(httpStatus));
    REQUIRE(ir.read_u64le(contentLength));
    REQUIRE(ir.read_u32le(hdrLen));

    CHECK(ihandle == handle);
    CHECK(httpStatus == 200);
    CHECK(contentLength == std::string("http://example.com/json").size());

    IOResponse rresp = read_req(dev, deviceId, handle, 0, 128);
    REQUIRE(rresp.status == StatusCode::Ok);
    netproto::Reader rr(rresp.payload.data(), rresp.payload.size());

    std::uint8_t rver = 0, rflags = 0;
    std::uint16_t rres = 0, rhandle = 0;
    std::uint32_t offEcho = 0;
    std::uint16_t dataLen = 0;

    REQUIRE(rr.read_u8(rver));
    REQUIRE(rr.read_u8(rflags));
    REQUIRE(rr.read_u16le(rres));
    REQUIRE(rr.read_u16le(rhandle));
    REQUIRE(rr.read_u32le(offEcho));
    REQUIRE(rr.read_u16le(dataLen));

    const std::uint8_t* dataPtr = nullptr;
    REQUIRE(rr.read_bytes(dataPtr, dataLen));
    std::string body(reinterpret_cast<const char*>(dataPtr), dataLen);

    CHECK(rhandle == handle);
    CHECK(body == "http://example.com/json");
    CHECK((rflags & 0x01) != 0);
    CHECK(close_req(dev, deviceId, handle).status == StatusCode::Ok);
}

TEST_CASE("NetworkDevice v1: JSON compatibility command reuses cached body")
{
    auto reg = make_stub_registry_http_only();
    NetworkDevice dev(std::move(reg));
    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    const std::uint16_t handle = open_handle_stub(dev, deviceId, "http://example.com/json");

    std::string qp;
    netproto::write_u8(qp, V);
    netproto::write_u16le(qp, handle);
    netproto::write_u8(qp, 1); // translationType=Json
    netproto::write_u8(qp, 0); // translationFlags
    netproto::write_lp_u16_string(qp, "/url");

    IORequest qreq{};
    qreq.id = 900;
    qreq.deviceId = deviceId;
    qreq.command = 0x07;
    qreq.payload = to_vec(qp);

    IOResponse qresp = dev.handle(qreq);
    REQUIRE(qresp.status == StatusCode::Ok);

    netproto::Reader qr(qresp.payload.data(), qresp.payload.size());
    std::uint8_t qver = 0, qflags = 0;
    std::uint16_t qres = 0, qhandle = 0;
    std::uint32_t qsize = 0;
    REQUIRE(qr.read_u8(qver));
    REQUIRE(qr.read_u8(qflags));
    REQUIRE(qr.read_u16le(qres));
    REQUIRE(qr.read_u16le(qhandle));
    REQUIRE(qr.read_u32le(qsize));
    CHECK((qflags & 0x01) != 0);
    CHECK(qhandle == handle);
    CHECK(qsize == std::string("http://example.com/json").size());

    IOResponse firstRead = read_req(dev, deviceId, handle, 0, 128);
    REQUIRE(firstRead.status == StatusCode::Ok);
    netproto::Reader firstR(firstRead.payload.data(), firstRead.payload.size());
    std::uint8_t frVer = 0, frFlags = 0;
    std::uint16_t frRes = 0, frHandle = 0, frLen = 0;
    std::uint32_t frOff = 0;
    REQUIRE(firstR.read_u8(frVer));
    REQUIRE(firstR.read_u8(frFlags));
    REQUIRE(firstR.read_u16le(frRes));
    REQUIRE(firstR.read_u16le(frHandle));
    REQUIRE(firstR.read_u32le(frOff));
    REQUIRE(firstR.read_u16le(frLen));
    const std::uint8_t* firstPtr = nullptr;
    REQUIRE(firstR.read_bytes(firstPtr, frLen));
    CHECK(std::string(reinterpret_cast<const char*>(firstPtr), frLen) == "http://example.com/json");

    std::string qp2;
    netproto::write_u8(qp2, V);
    netproto::write_u16le(qp2, handle);
    netproto::write_u8(qp2, 1); // translationType=Json
    netproto::write_u8(qp2, 0); // translationFlags
    netproto::write_lp_u16_string(qp2, "/headers/Host");

    qreq.id = 901;
    qreq.payload = to_vec(qp2);
    qresp = dev.handle(qreq);
    REQUIRE(qresp.status == StatusCode::Ok);

    IOResponse rresp = read_req(dev, deviceId, handle, 0, 128);
    REQUIRE(rresp.status == StatusCode::Ok);
    netproto::Reader secondR(rresp.payload.data(), rresp.payload.size());
    std::uint8_t srVer = 0, srFlags = 0;
    std::uint16_t srRes = 0, srHandle = 0, srLen = 0;
    std::uint32_t srOff = 0;
    REQUIRE(secondR.read_u8(srVer));
    REQUIRE(secondR.read_u8(srFlags));
    REQUIRE(secondR.read_u16le(srRes));
    REQUIRE(secondR.read_u16le(srHandle));
    REQUIRE(secondR.read_u32le(srOff));
    REQUIRE(secondR.read_u16le(srLen));
    const std::uint8_t* secondPtr = nullptr;
    REQUIRE(secondR.read_bytes(secondPtr, srLen));
    CHECK(std::string(reinterpret_cast<const char*>(secondPtr), srLen) == "example.com");

    CHECK(close_req(dev, deviceId, handle).status == StatusCode::Ok);
}

TEST_CASE("NetworkDevice v1: JSON array elements can be fetched by indexed path")
{
    auto reg = make_stub_registry_http_only();
    NetworkDevice dev(std::move(reg));
    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    const std::uint16_t handle = open_handle_stub(dev, deviceId, "http://example.com/json-array");

    auto configure_and_read = [&](std::string_view selector) {
        std::string qp;
        netproto::write_u8(qp, V);
        netproto::write_u16le(qp, handle);
        netproto::write_u8(qp, 1); // translationType=Json
        netproto::write_u8(qp, 0); // translationFlags
        netproto::write_lp_u16_string(qp, selector);

        IORequest qreq{};
        qreq.id = 950;
        qreq.deviceId = deviceId;
        qreq.command = 0x07;
        qreq.payload = to_vec(qp);

        IOResponse qresp = dev.handle(qreq);
        REQUIRE(qresp.status == StatusCode::Ok);

        IOResponse rresp = read_req(dev, deviceId, handle, 0, 128);
        REQUIRE(rresp.status == StatusCode::Ok);

        netproto::Reader rr(rresp.payload.data(), rresp.payload.size());
        std::uint8_t rver = 0, rflags = 0;
        std::uint16_t rres = 0, rhandle = 0, dataLen = 0;
        std::uint32_t offEcho = 0;
        REQUIRE(rr.read_u8(rver));
        REQUIRE(rr.read_u8(rflags));
        REQUIRE(rr.read_u16le(rres));
        REQUIRE(rr.read_u16le(rhandle));
        REQUIRE(rr.read_u32le(offEcho));
        REQUIRE(rr.read_u16le(dataLen));

        const std::uint8_t* dataPtr = nullptr;
        REQUIRE(rr.read_bytes(dataPtr, dataLen));
        return std::string(reinterpret_cast<const char*>(dataPtr), dataLen);
    };

    CHECK(configure_and_read("/items/0") == "alpha");
    CHECK(configure_and_read("/items/1") == "beta");
    CHECK(configure_and_read("/items/2") == "gamma");

    CHECK(close_req(dev, deviceId, handle).status == StatusCode::Ok);
}

TEST_CASE("NetworkDevice v1: Open content profile injects request Content-Type")
{
    fujinet::io::StubNetworkProtocol* lastStub = nullptr;
    fujinet::io::ProtocolRegistry reg;
    reg.register_scheme("http", [&] {
        auto p = std::make_unique<fujinet::io::StubNetworkProtocol>();
        lastStub = p.get();
        return p;
    });
    NetworkDevice dev(std::move(reg));
    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    const std::uint16_t handle = open_handle_stub(
        dev,
        deviceId,
        "http://example.com/post",
        /*method=*/2,
        /*flags=*/0,
        /*bodyLenHint=*/4,
        {},
        fujinet::io::ContentTranslationType::None,
        {},
        0,
        fujinet::io::RequestContentProfile::JsonBody);
    REQUIRE(lastStub != nullptr);

    bool foundJson = false;
    for (const auto& kv : lastStub->openRequest().headers) {
        if (kv.first == "Content-Type" && kv.second == "application/json") {
            foundJson = true;
        }
    }
    CHECK(foundJson);

    CHECK(close_req(dev, deviceId, handle).status == StatusCode::Ok);
}

TEST_CASE("NetworkDevice v1: Open content profile does not override explicit Content-Type")
{
    fujinet::io::StubNetworkProtocol* lastStub = nullptr;
    fujinet::io::ProtocolRegistry reg;
    reg.register_scheme("http", [&] {
        auto p = std::make_unique<fujinet::io::StubNetworkProtocol>();
        lastStub = p.get();
        return p;
    });
    NetworkDevice dev(std::move(reg));
    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    std::string p;
    netproto::write_u8(p, V);
    netproto::write_u8(p, 2); // POST
    netproto::write_u8(p, 0); // flags
    netproto::write_lp_u16_string(p, "http://example.com/post");
    netproto::write_u16le(p, 1); // one explicit header
    netproto::write_lp_u16_string(p, "Content-Type");
    netproto::write_lp_u16_string(p, "text/custom");
    netproto::write_u32le(p, 0);
    netproto::write_u16le(p, 0);
    netproto::write_u32le(p, fujinet::io::NETWORK_OPEN_EXT_CONTENT_PROFILE);
    netproto::write_u8(p, static_cast<std::uint8_t>(fujinet::io::RequestContentProfile::JsonBody));

    IORequest req{};
    req.id = 960;
    req.deviceId = deviceId;
    req.command = 0x01;
    req.payload = to_vec(p);

    IOResponse resp = dev.handle(req);
    REQUIRE(resp.status == StatusCode::Ok);
    REQUIRE(lastStub != nullptr);

    REQUIRE(lastStub->openRequest().headers.size() == 1);
    CHECK(lastStub->openRequest().headers[0].first == "Content-Type");
    CHECK(lastStub->openRequest().headers[0].second == "text/custom");

    netproto::Reader r(resp.payload.data(), resp.payload.size());
    std::uint8_t ver = 0, oflags = 0;
    std::uint16_t reserved = 0, handle = 0;
    REQUIRE(r.read_u8(ver));
    REQUIRE(r.read_u8(oflags));
    REQUIRE(r.read_u16le(reserved));
    REQUIRE(r.read_u16le(handle));
    CHECK(close_req(dev, deviceId, handle).status == StatusCode::Ok);
}

// ---------------------------------------------------------------------------
// Image translation (type 4)
// ---------------------------------------------------------------------------

namespace {

// Serves a fixed body (an image) for any URL, like an HTTP GET.
class FixedBodyProtocol final : public fujinet::io::INetworkProtocol {
public:
    FixedBodyProtocol(const std::uint8_t* body, std::size_t len)
        : _body(body, body + len)
    {}

    StatusCode open(const fujinet::io::NetworkOpenRequest&) override { return StatusCode::Ok; }

    StatusCode write_body(std::uint32_t, const std::uint8_t*, std::size_t, std::uint16_t& written) override
    {
        written = 0;
        return StatusCode::Unsupported;
    }

    StatusCode read_body(std::uint32_t offset,
                         std::uint8_t* out,
                         std::size_t outLen,
                         std::uint16_t& read,
                         bool& eof,
                         bool& more_available) override
    {
        const std::size_t off = std::min<std::size_t>(offset, _body.size());
        const std::size_t n = std::min<std::size_t>({outLen, _body.size() - off, 0xFFFFu});
        if (n > 0) {
            std::memcpy(out, _body.data() + off, n);
        }
        read = static_cast<std::uint16_t>(n);
        eof = off + n >= _body.size();
        more_available = !eof;
        return StatusCode::Ok;
    }

    StatusCode info(fujinet::io::NetworkInfo& out) override
    {
        out = fujinet::io::NetworkInfo{};
        out.hasHttpStatus = true;
        out.httpStatus = 200;
        out.hasContentLength = true;
        out.contentLength = _body.size();
        return StatusCode::Ok;
    }

    void poll() override {}
    void close() override {}

private:
    std::vector<std::uint8_t> _body;
};

template <std::size_t N>
fujinet::io::ProtocolRegistry make_image_registry(const std::uint8_t (&body)[N])
{
    fujinet::io::ProtocolRegistry reg;
    reg.register_scheme("http", [&body] { return std::make_unique<FixedBodyProtocol>(body, N); });
    return reg;
}

// Settings with an Image pixel cap large enough for every fixture. A device
// built without settings refuses Image translation.
fujinet::io::NetworkDeviceSettings image_settings()
{
    fujinet::io::NetworkDeviceSettings settings;
    settings.imageMaxPixels = 4096u * 4096u;
    return settings;
}

struct ReadResult {
    StatusCode status{StatusCode::InternalError};
    std::string data;
    bool eof{false};
};

ReadResult read_chunk(NetworkDevice& dev, std::uint16_t deviceId, std::uint16_t handle,
                      std::uint32_t offset, std::uint16_t maxBytes)
{
    ReadResult out;
    IOResponse resp = read_req(dev, deviceId, handle, offset, maxBytes);
    out.status = resp.status;
    if (resp.status != StatusCode::Ok) {
        return out;
    }
    netproto::Reader r(resp.payload.data(), resp.payload.size());
    std::uint8_t ver = 0;
    std::uint8_t flags = 0;
    std::uint16_t reserved = 0;
    std::uint16_t h = 0;
    std::uint32_t offEcho = 0;
    std::uint16_t len = 0;
    REQUIRE(r.read_u8(ver));
    REQUIRE(r.read_u8(flags));
    REQUIRE(r.read_u16le(reserved));
    REQUIRE(r.read_u16le(h));
    REQUIRE(r.read_u32le(offEcho));
    REQUIRE(r.read_u16le(len));
    const std::uint8_t* ptr = nullptr;
    REQUIRE(r.read_bytes(ptr, len));
    out.data.assign(reinterpret_cast<const char*>(ptr), len);
    out.eof = (flags & 0x01) != 0;
    return out;
}

// Read the whole translated view.
std::string read_all(NetworkDevice& dev, std::uint16_t deviceId, std::uint16_t handle)
{
    std::string all;
    for (int guard = 0; guard < 1000; ++guard) {
        const ReadResult chunk = read_chunk(dev, deviceId, handle, static_cast<std::uint32_t>(all.size()), 512);
        REQUIRE(chunk.status == StatusCode::Ok);
        all += chunk.data;
        if (chunk.eof || chunk.data.empty()) {
            break;
        }
    }
    return all;
}

std::uint64_t info_content_length(NetworkDevice& dev, std::uint16_t deviceId, std::uint16_t handle)
{
    IOResponse resp = info_req(dev, deviceId, handle);
    REQUIRE(resp.status == StatusCode::Ok);
    netproto::Reader r(resp.payload.data(), resp.payload.size());
    std::uint8_t ver = 0;
    std::uint8_t flags = 0;
    std::uint16_t reserved = 0;
    std::uint16_t h = 0;
    std::uint16_t httpStatus = 0;
    std::uint64_t contentLength = 0;
    REQUIRE(r.read_u8(ver));
    REQUIRE(r.read_u8(flags));
    REQUIRE(r.read_u16le(reserved));
    REQUIRE(r.read_u16le(h));
    REQUIRE(r.read_u16le(httpStatus));
    REQUIRE(r.read_u64le(contentLength));
    return contentLength;
}

} // namespace

TEST_CASE("NetworkDevice v1: Open-time Image translation reads back FORM ILBM")
{
    NetworkDevice dev(make_image_registry(fujinet::tests::image::kPngColour16x12), image_settings());
    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    const std::uint16_t handle = open_handle_stub(
        dev, deviceId, "http://example.com/a.png", 1, 0, 0, {},
        fujinet::io::ContentTranslationType::Image, "");

    const std::string ilbm = read_all(dev, deviceId, handle);
    REQUIRE(ilbm.size() >= 12);
    CHECK(ilbm.substr(0, 4) == "FORM");
    CHECK(ilbm.substr(8, 4) == "ILBM");
    CHECK(info_content_length(dev, deviceId, handle) == ilbm.size());

    const std::vector<std::uint8_t> bytes(ilbm.begin(), ilbm.end());
    CHECK(fujinet::tests::image::fnv1a(bytes) == 0xBDA7F5DFu);   // as ImageGolden

    CHECK(close_req(dev, deviceId, handle).status == StatusCode::Ok);
}

TEST_CASE("NetworkDevice v1: Image selector errors fail at Open with InvalidRequest")
{
    NetworkDevice dev(make_image_registry(fujinet::tests::image::kPngColour16x12), image_settings());
    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    for (const char* selector : {"colors=99", "bogus=1", "w=16,w=16", "bits=9", "fmt=png", "fmt="}) {
        CAPTURE(selector);
        IOResponse resp = open_req(dev, deviceId, "http://example.com/a.png",
                                   fujinet::io::ContentTranslationType::Image, selector);
        CHECK(resp.status == StatusCode::InvalidRequest);
    }

    // The failed opens left no session behind: all four handles are free.
    for (int i = 0; i < 4; ++i) {
        open_handle_stub(dev, deviceId, "http://example.com/a.png");
    }
}

TEST_CASE("NetworkDevice v1: Image accepts fmt=ilbm and bits at Open")
{
    NetworkDevice dev(make_image_registry(fujinet::tests::image::kPngColour16x12), image_settings());
    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    const std::uint16_t handle = open_handle_stub(
        dev, deviceId, "http://example.com/a.png", 1, 0, 0, {},
        fujinet::io::ContentTranslationType::Image, "fmt=ilbm,bits=8,colors=8");
    const std::string ilbm = read_all(dev, deviceId, handle);
    CHECK(ilbm.substr(0, 4) == "FORM");
    CHECK(ilbm.substr(8, 4) == "ILBM");
}

TEST_CASE("NetworkDevice v1: undecodable Image body fails Read with InvalidRequest")
{
    NetworkDevice dev(make_stub_registry_http_only(), image_settings());
    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    const std::uint16_t handle = open_handle_stub(
        dev, deviceId, "http://example.com/not-an-image", 1, 0, 0, {},
        fujinet::io::ContentTranslationType::Image, "");
    CHECK(read_req(dev, deviceId, handle, 0, 64).status == StatusCode::InvalidRequest);
}

TEST_CASE("NetworkDevice v1: TranslateConfigure re-runs Image translation on the cached body")
{
    NetworkDevice dev(make_image_registry(fujinet::tests::image::kPngColour16x12), image_settings());
    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    const std::uint16_t handle = open_handle_stub(
        dev, deviceId, "http://example.com/a.png", 1, 0, 0, {},
        fujinet::io::ContentTranslationType::Image, "");
    const std::string first = read_all(dev, deviceId, handle);
    REQUIRE(first.substr(0, 4) == "FORM");

    auto translate = [&](const char* selector) {
        std::string qp;
        netproto::write_u8(qp, V);
        netproto::write_u16le(qp, handle);
        netproto::write_u8(qp, 4); // translationType=Image
        netproto::write_u8(qp, 0); // translationFlags
        netproto::write_lp_u16_string(qp, selector);
        IORequest qreq{};
        qreq.id = 950;
        qreq.deviceId = deviceId;
        qreq.command = 0x07;
        qreq.payload = to_vec(qp);
        return dev.handle(qreq);
    };

    REQUIRE(translate("up=1,colors=5").status == StatusCode::Ok);
    const std::string second = read_all(dev, deviceId, handle);
    const std::vector<std::uint8_t> bytes(second.begin(), second.end());
    CHECK(fujinet::tests::image::fnv1a(bytes) == 0x95302CF1u);   // as ImageGolden

    // Same selector again on the same cached body: same bytes.
    REQUIRE(translate("").status == StatusCode::Ok);
    CHECK(read_all(dev, deviceId, handle) == first);

    CHECK(translate("fmt=gif").status == StatusCode::InvalidRequest);
}

TEST_CASE("NetworkDevice v1: without settings, Image translation is Unsupported at Open")
{
    NetworkDevice dev(make_image_registry(fujinet::tests::image::kPngColour16x12));
    const auto deviceId = to_device_id(WireDeviceId::NetworkService);

    const IOResponse resp = open_req(dev, deviceId, "http://example.com/a.png",
                                     fujinet::io::ContentTranslationType::Image, "");
    CHECK(resp.status == StatusCode::Unsupported);
}

TEST_CASE("NetworkDevice v1: the configured image pixel cap reaches the Image translator")
{
    const auto deviceId = to_device_id(WireDeviceId::NetworkService);
    const auto& png = fujinet::tests::image::kPngColour16x12;    // 16x12 = 192 pixels

    auto open_and_read = [&](std::uint32_t cap) {
        fujinet::io::NetworkDeviceSettings settings;
        settings.imageMaxPixels = cap;
        NetworkDevice dev(make_image_registry(png), settings);
        const std::uint16_t handle = open_handle_stub(
            dev, deviceId, "http://example.com/a.png", 1, 0, 0, {},
            fujinet::io::ContentTranslationType::Image, "");
        return read_req(dev, deviceId, handle, 0, 64).status;
    };

    CHECK(open_and_read(192) == StatusCode::Ok);
    CHECK(open_and_read(191) == StatusCode::Unsupported);
    CHECK(open_and_read(16u * 1024u * 1024u) == StatusCode::Ok);
}

// ---------------------------------------------------------------------------
// Readiness notification (IClientNotifier): the client that owns a session hears when a read that
// would have been NotReady can now be answered, once per transition.
// ---------------------------------------------------------------------------

namespace {

// A stream whose readiness the test controls.
class ReadinessProtocol final : public fujinet::io::INetworkProtocol {
public:
    static bool ready;
    static bool more_after_read;   // a read leaves data behind
    StatusCode open(const fujinet::io::NetworkOpenRequest&) override { return StatusCode::Ok; }
    StatusCode write_body(std::uint32_t, const std::uint8_t*, std::size_t, std::uint16_t& w) override
    {
        w = 0;
        return StatusCode::Unsupported;
    }
    StatusCode read_body(std::uint32_t, std::uint8_t* out, std::size_t, std::uint16_t& read, bool& eof,
                         bool& more) override
    {
        read = 0;
        eof = false;
        more = false;
        if (!ready) return StatusCode::NotReady;
        out[0] = 'x';
        read = 1;
        more = more_after_read;
        return StatusCode::Ok;
    }
    StatusCode info(fujinet::io::NetworkInfo&) override { return StatusCode::Ok; }
    void poll() override {}
    void close() override {}
    bool read_ready() const override { return ready; }
};
bool ReadinessProtocol::ready = false;
bool ReadinessProtocol::more_after_read = false;

struct NotifierRecorder final : fujinet::io::IClientNotifier {
    std::vector<const void*> heard;
    void clientReadable(const void* owner) override { heard.push_back(owner); }
};

} // namespace

TEST_CASE("NetworkDevice tells the owning client once when a session becomes readable")
{
    fujinet::io::ProtocolRegistry reg;
    reg.register_scheme("tcp", [] { return std::make_unique<ReadinessProtocol>(); });
    NetworkDevice dev(std::move(reg));
    NotifierRecorder notes;
    dev.set_client_notifier(&notes);
    const auto deviceId = to_device_id(WireDeviceId::NetworkService);
    int clientA = 0, clientB = 0;
    auto open = [&](const void* owner) {
        IORequest req{};
        req.deviceId = deviceId;
        req.clientOwner = owner;
        req.command = 1;
        req.payload = to_vec(open_payload("tcp://example.com:1", 1, 0, 0, {}));
        auto resp = dev.handle(req);
        REQUIRE(resp.status == StatusCode::Ok);
        return static_cast<std::uint16_t>(resp.payload[4] | (resp.payload[5] << 8));
    };
    ReadinessProtocol::ready = false;
    const auto a = open(&clientA);
    open(&clientB);
    open(nullptr);   // no owner: nobody to tell

    dev.poll();
    CHECK(notes.heard.empty());                       // nothing readable yet
    CHECK(read_req(dev, deviceId, a, 0, 16).status == StatusCode::NotReady);

    ReadinessProtocol::ready = true;
    dev.poll();
    REQUIRE(notes.heard.size() == 2);                  // A and B, not the owner-less session
    CHECK(notes.heard[0] != notes.heard[1]);
    dev.poll();
    dev.poll();
    CHECK(notes.heard.size() == 2);                    // once per transition, not every tick

    // A read that takes everything (or reaches end of stream) adds no repeat: nothing more is coming, or the
    // session is still readable only because it is at the end.
    CHECK(read_req(dev, deviceId, a, 0, 16).status == StatusCode::Ok);
    dev.poll();
    CHECK(notes.heard.size() == 2);

    // A read that leaves data behind is announced again at the next poll (so a client that reads part of
    // it and then waits is not stranded), to that client only.
    ReadinessProtocol::more_after_read = true;
    CHECK(read_req(dev, deviceId, a, 0, 16).status == StatusCode::Ok);
    dev.poll();
    CHECK(notes.heard.size() == 3);
    CHECK(notes.heard.back() == &clientA);
    ReadinessProtocol::more_after_read = false;

    // Not readable again, then readable: a new notice.
    ReadinessProtocol::ready = false;
    dev.poll();
    ReadinessProtocol::ready = true;
    dev.poll();
    CHECK(notes.heard.size() == 5);
}

TEST_CASE("IODeviceManager forwards a device's notice to the listener watching that client")
{
    fujinet::io::IODeviceManager manager;
    fujinet::io::ProtocolRegistry reg;
    reg.register_scheme("tcp", [] { return std::make_unique<ReadinessProtocol>(); });
    const auto deviceId = to_device_id(WireDeviceId::NetworkService);
    REQUIRE(manager.registerDevice(deviceId, std::make_unique<NetworkDevice>(std::move(reg))));
    int watched = 0, other = 0;
    int heard = 0;
    manager.watchClient(&watched, [&] { ++heard; });

    auto open = [&](const void* owner) {
        IORequest req{};
        req.deviceId = deviceId;
        req.clientOwner = owner;
        req.command = 1;
        req.payload = to_vec(open_payload("tcp://example.com:1", 1, 0, 0, {}));
        REQUIRE(manager.handleRequest(req).status == StatusCode::Ok);
    };
    ReadinessProtocol::ready = false;
    open(&watched);
    open(&other);
    manager.pollDevices();
    CHECK(heard == 0);
    ReadinessProtocol::ready = true;
    manager.pollDevices();
    CHECK(heard == 1);                                 // only the watched client's listener ran

    manager.watchClient(&watched, {});                 // an empty listener removes it
    ReadinessProtocol::ready = false;
    manager.pollDevices();
    ReadinessProtocol::ready = true;
    manager.pollDevices();
    CHECK(heard == 1);
}

// An HTTP-like session: the status becomes known first (info_ready), then body bytes arrive (read_ready).
// The client is told for each, once, and for both at once only once.
namespace {
class StagedProtocol final : public fujinet::io::INetworkProtocol {
public:
    static bool info_known;
    static bool body_ready;
    StatusCode open(const fujinet::io::NetworkOpenRequest&) override { return StatusCode::Ok; }
    StatusCode write_body(std::uint32_t, const std::uint8_t*, std::size_t, std::uint16_t& w) override
    {
        w = 0;
        return StatusCode::Unsupported;
    }
    StatusCode read_body(std::uint32_t, std::uint8_t*, std::size_t, std::uint16_t& read, bool& eof, bool& more) override
    {
        read = 0;
        eof = false;
        more = false;
        return StatusCode::NotReady;
    }
    StatusCode info(fujinet::io::NetworkInfo&) override { return StatusCode::Ok; }
    void poll() override {}
    void close() override {}
    bool read_ready() const override { return body_ready; }
    bool info_ready() const override { return info_known; }
};
bool StagedProtocol::info_known = false;
bool StagedProtocol::body_ready = false;
}

TEST_CASE("NetworkDevice tells the client when the status is known and again when body bytes arrive")
{
    fujinet::io::ProtocolRegistry reg;
    reg.register_scheme("http", [] { return std::make_unique<StagedProtocol>(); });
    NetworkDevice dev(std::move(reg));
    NotifierRecorder notes;
    dev.set_client_notifier(&notes);
    const auto deviceId = to_device_id(WireDeviceId::NetworkService);
    int client = 0;
    IORequest req{};
    req.deviceId = deviceId;
    req.clientOwner = &client;
    req.command = 1;
    req.payload = to_vec(open_payload("http://example.com/slow", 1, 0, 0, {}));
    REQUIRE(dev.handle(req).status == StatusCode::Ok);

    StagedProtocol::info_known = false;
    StagedProtocol::body_ready = false;
    dev.poll();
    CHECK(notes.heard.empty());

    StagedProtocol::info_known = true;                 // the status line arrived
    dev.poll();
    CHECK(notes.heard.size() == 1);
    dev.poll();
    CHECK(notes.heard.size() == 1);                    // not repeated

    StagedProtocol::body_ready = true;                 // then the first body bytes
    dev.poll();
    CHECK(notes.heard.size() == 2);
    dev.poll();
    CHECK(notes.heard.size() == 2);
}

TEST_CASE("NetworkDevice says nothing for a session still waiting for its request body")
{
    fujinet::io::ProtocolRegistry reg;
    reg.register_scheme("http", [] { return std::make_unique<StagedProtocol>(); });
    NetworkDevice dev(std::move(reg));
    NotifierRecorder notes;
    dev.set_client_notifier(&notes);
    const auto deviceId = to_device_id(WireDeviceId::NetworkService);
    int client = 0;
    IORequest req{};
    req.deviceId = deviceId;
    req.clientOwner = &client;
    req.command = 1;
    // POST with a body length hint: Info and Read answer NotReady until the body is written.
    req.payload = to_vec(open_payload("http://example.com/post", 2, 0, 10, {}));
    REQUIRE(dev.handle(req).status == StatusCode::Ok);
    StagedProtocol::info_known = true;
    StagedProtocol::body_ready = true;
    dev.poll();
    CHECK(notes.heard.empty());
    StagedProtocol::info_known = false;
    StagedProtocol::body_ready = false;
}
