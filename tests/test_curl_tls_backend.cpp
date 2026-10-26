#include "doctest.h"

#if defined(FN_PLATFORM_POSIX)

#include "fujinet/platform/posix/http_network_protocol_curl.h"

using fujinet::platform::posix::curl_tls_is_openssl;

TEST_CASE("curl TLS backend: the OpenSSL we link")
{
    CHECK(curl_tls_is_openssl("OpenSSL/3.0.13", "OpenSSL 3.0.13 30 Jan 2024"));
    CHECK(curl_tls_is_openssl("OpenSSL/1.1.1w", "OpenSSL 1.1.1w  11 Sep 2023"));
}

TEST_CASE("curl TLS backend: another OpenSSL release")
{
    CHECK_FALSE(curl_tls_is_openssl("OpenSSL/3.5.9", "OpenSSL 4.0.3 1 Oct 2026"));
    CHECK_FALSE(curl_tls_is_openssl("OpenSSL/3.0.1", "OpenSSL 3.0.13 30 Jan 2024"));
}

TEST_CASE("curl TLS backend: another library")
{
    // macOS's system curl: LibreSSL in use, Secure Transport built in
    CHECK_FALSE(curl_tls_is_openssl("(SecureTransport) LibreSSL/3.3.6", "OpenSSL 4.0.3 1 Oct 2026"));
    CHECK_FALSE(curl_tls_is_openssl("GnuTLS/3.8.3", "OpenSSL 3.0.13 30 Jan 2024"));
    CHECK_FALSE(curl_tls_is_openssl("BoringSSL", "OpenSSL 3.0.13 30 Jan 2024"));
    CHECK_FALSE(curl_tls_is_openssl("quictls/3.1.4", "OpenSSL 3.1.4 24 Oct 2023"));
}

TEST_CASE("curl TLS backend: only the backend in use counts")
{
    CHECK(curl_tls_is_openssl("(SecureTransport) OpenSSL/3.0.13", "OpenSSL 3.0.13 30 Jan 2024"));
    CHECK_FALSE(curl_tls_is_openssl("SecureTransport (OpenSSL/3.0.13)", "OpenSSL 3.0.13 30 Jan 2024"));
}

TEST_CASE("curl TLS backend: no TLS or no version")
{
    CHECK_FALSE(curl_tls_is_openssl("", "OpenSSL 3.0.13 30 Jan 2024"));
    CHECK_FALSE(curl_tls_is_openssl("OpenSSL/3.0.13", ""));
    CHECK_FALSE(curl_tls_is_openssl("OpenSSL/", "OpenSSL "));
}

#endif
