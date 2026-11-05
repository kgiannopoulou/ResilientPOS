#pragma once

#include <string>
#include <string_view>

namespace resilientpos::util {

// SHA-256 of `data` as 64 lowercase hex characters (FIPS 180-4).
//
// Used to fingerprint transaction payloads, not to protect secrets. Written
// out here rather than pulling in OpenSSL for one function; pinned by the NIST
// test vectors in Sha256Tests.
[[nodiscard]]
std::string sha256Hex(std::string_view data);

} // namespace resilientpos::util
