#pragma once

#include <string>

namespace resilientpos::util {

// Generates random (version 4) UUIDs, e.g. "550e8400-e29b-41d4-a716-446655440000".
//
// Transactions are created offline on many tills at once, so IDs cannot come
// from a central counter. 122 random bits make collisions practically impossible.
class UuidGenerator {
public:
    [[nodiscard]]
    static std::string generate();
};

} // namespace resilientpos::util
