#include "resilientpos/util/Sha256.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using resilientpos::util::sha256Hex;

TEST_CASE("SHA-256 matches the NIST test vectors", "[util][sha256]")
{
    REQUIRE(sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    REQUIRE(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    REQUIRE(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
            == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    REQUIRE(sha256Hex(std::string(1'000'000, 'a'))
            == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("SHA-256 pads correctly around the block boundary", "[util][sha256]")
{
    // 55 bytes fit the padding in one block, 56 need a second, 64 is one exact block.
    REQUIRE(sha256Hex(std::string(55, 'a')) == "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    REQUIRE(sha256Hex(std::string(56, 'a')) == "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    REQUIRE(sha256Hex(std::string(64, 'a')) == "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
}
