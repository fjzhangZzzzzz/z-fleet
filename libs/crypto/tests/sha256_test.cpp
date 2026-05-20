#include <zfleet/crypto/sha256.h>

#include "test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("sha256 hashes bytes as lowercase hex") {
  REQUIRE(zfleet::crypto::Sha256BytesHex("") ==
          "e3b0c44298fc1c149afbf4c8996fb924"
          "27ae41e4649b934ca495991b7852b855");
  REQUIRE(zfleet::crypto::Sha256BytesHex("abc") ==
          "ba7816bf8f01cfea414140de5dae2223"
          "b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("sha256 hashes files without loading full file") {
  const zfleet::test::ScopedTestDir test_dir("crypto");
  const auto file_path = test_dir / "payload.bin";
  zfleet::test::WriteTextFile(file_path, "abc");

  REQUIRE(zfleet::crypto::Sha256FileHex(file_path) ==
          zfleet::crypto::Sha256BytesHex("abc"));
}

TEST_CASE("sha256 lower hex validation rejects non canonical values") {
  REQUIRE(zfleet::crypto::IsLowerHexSha256(
      "e3b0c44298fc1c149afbf4c8996fb924"
      "27ae41e4649b934ca495991b7852b855"));
  REQUIRE_FALSE(zfleet::crypto::IsLowerHexSha256("abc"));
  REQUIRE_FALSE(zfleet::crypto::IsLowerHexSha256(
      "E3B0C44298FC1C149AFBF4C8996FB924"
      "27AE41E4649B934CA495991B7852B855"));
}
