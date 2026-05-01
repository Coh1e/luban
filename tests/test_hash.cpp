// Tests for src/hash.cpp — HashSpec parse/format and verify_file.
//
// Hash code is the trust boundary for every download. Misparsing a hash
// spec ("md5:abc...") as sha256 silently breaks SHA verification: the
// download would be accepted with the wrong algorithm. These tests pin
// the parse + format contract.

#include "doctest.h"

#include <filesystem>
#include <fstream>

#include "hash.hpp"

namespace fs = std::filesystem;

TEST_CASE("hash::parse_algo recognizes the four supported algorithms") {
    using A = luban::hash::Algorithm;
    CHECK(*luban::hash::parse_algo("sha256") == A::Sha256);
    CHECK(*luban::hash::parse_algo("sha512") == A::Sha512);
    CHECK(*luban::hash::parse_algo("sha1")   == A::Sha1);
    CHECK(*luban::hash::parse_algo("md5")    == A::Md5);
}

TEST_CASE("hash::parse_algo rejects unknown / typos") {
    CHECK_FALSE(luban::hash::parse_algo("sha3").has_value());
    CHECK_FALSE(luban::hash::parse_algo("blake2b").has_value());
    CHECK_FALSE(luban::hash::parse_algo("").has_value());
}

TEST_CASE("hash::parse_algo is case-insensitive (forgiving for SHA256SUMS files)") {
    // vcpkg / GitHub release SHA256SUMS files often UPPERCASE the algo
    // prefix. parse_algo accepts that — the HashSpec.hex is normalized
    // to lowercase later regardless.
    using A = luban::hash::Algorithm;
    CHECK(*luban::hash::parse_algo("SHA256") == A::Sha256);
    CHECK(*luban::hash::parse_algo("Sha256") == A::Sha256);
}

TEST_CASE("hash::parse with explicit algorithm prefix") {
    auto h = luban::hash::parse("sha256:abc123def456");
    REQUIRE(h.has_value());
    CHECK(h->algo == luban::hash::Algorithm::Sha256);
    CHECK(h->hex == "abc123def456");
}

TEST_CASE("hash::parse defaults to sha256 when no prefix") {
    // Bare hex with no `:` defaults to sha256 — common in Scoop manifests
    // that pre-date the multi-algo era.
    auto h = luban::hash::parse("abc123def456");
    REQUIRE(h.has_value());
    CHECK(h->algo == luban::hash::Algorithm::Sha256);
    CHECK(h->hex == "abc123def456");
}

TEST_CASE("hash::parse normalizes hex case to lowercase") {
    // The HashSpec invariant says hex is lowercase. Inputs from manifests
    // sometimes have UPPERCASE hex (vcpkg's SHA256SUMS files etc.).
    auto h = luban::hash::parse("sha256:ABC123DEF456");
    REQUIRE(h.has_value());
    CHECK(h->hex == "abc123def456");
}

TEST_CASE("hash::to_string round-trips through parse") {
    luban::hash::HashSpec orig{luban::hash::Algorithm::Sha512,
                               "deadbeef0123456789abcdef"};
    auto s = luban::hash::to_string(orig);
    auto reparsed = luban::hash::parse(s);
    REQUIRE(reparsed.has_value());
    CHECK(reparsed->algo == orig.algo);
    CHECK(reparsed->hex == orig.hex);
}

TEST_CASE("hash::hash_file matches verify_file on a small synthetic file") {
    // Write a known-content file under temp_dir, hash it via hash_file,
    // verify_file should accept the same hash and reject a tampered one.
    fs::path tmp = fs::temp_directory_path() / "luban-test-hash.bin";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << "hello luban tests";
    }
    auto h = luban::hash::hash_file(tmp);
    REQUIRE(h.has_value());
    CHECK(h->algo == luban::hash::Algorithm::Sha256);
    CHECK(h->hex.size() == 64);  // 32 bytes hex-encoded

    // verify_file accepts the same hash.
    CHECK(luban::hash::verify_file(tmp, *h));

    // Tamper one hex char — verify_file must reject.
    auto bad = *h;
    bad.hex[0] = (bad.hex[0] == 'a' ? 'b' : 'a');
    CHECK_FALSE(luban::hash::verify_file(tmp, bad));

    fs::remove(tmp);
}

TEST_CASE("hash::hash_file returns nullopt on missing path") {
    auto h = luban::hash::hash_file(
        fs::temp_directory_path() / "definitely-does-not-exist-xyzzy.bin");
    CHECK_FALSE(h.has_value());
}
