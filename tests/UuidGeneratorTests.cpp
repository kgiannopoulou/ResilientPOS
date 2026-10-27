#include "resilientpos/util/UuidGenerator.hpp"

#include <catch2/catch_test_macros.hpp>

#include <regex>
#include <string>
#include <unordered_set>

using resilientpos::util::UuidGenerator;

TEST_CASE("generate() doesn't return empty string", "[uuid]")
{
    REQUIRE_FALSE(UuidGenerator::generate().empty());
}

TEST_CASE("two generated IDs are different", "[uuid]")
{
    REQUIRE(UuidGenerator::generate() != UuidGenerator::generate());
}

TEST_CASE("generated IDs look like version 4 UUIDs", "[uuid]")
{
    static const std::regex uuidV4{
        "^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$"};

    for (int i = 0; i < 100; ++i) {
        const std::string id = UuidGenerator::generate();
        INFO("id = " << id);
        REQUIRE(std::regex_match(id, uuidV4));
    }
}

TEST_CASE("many generated IDs are all unique", "[uuid]")
{
    constexpr int count = 10'000;
    std::unordered_set<std::string> seen;
    for (int i = 0; i < count; ++i) {
        seen.insert(UuidGenerator::generate());
    }
    REQUIRE(seen.size() == count);
}
