#include <catch2/catch_test_macros.hpp>

#include <string>

#include "core/config/AtomicFile.h"
#include "core/config/Sha256.h"

TEST_CASE("SHA-256 FIPS vectors", "[config][sha256]") {
  CHECK(bf::sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(bf::sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(bf::sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  bf::Sha256 h;  // one million 'a', fed in odd-sized chunks
  std::string chunk(997, 'a');
  std::size_t left = 1000000;
  while (left > 0) {
    std::size_t n = left < chunk.size() ? left : chunk.size();
    h.update(chunk.data(), n);
    left -= n;
  }
  CHECK(h.finishHex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("Atomic file write keeps a .bak", "[config][atomic]") {
  auto dir = std::filesystem::temp_directory_path() / "bf_atomic_test";
  std::filesystem::remove_all(dir);
  auto f = dir / "sub" / "settings.json";
  std::string err;
  REQUIRE(bf::writeFileAtomic(f, "one", &err));
  REQUIRE(bf::writeFileAtomic(f, "two", &err));
  std::string s;
  REQUIRE(bf::readFile(f, s));
  CHECK(s == "two");
  REQUIRE(bf::readFile(std::filesystem::path(f.string() + ".bak"), s));
  CHECK(s == "one");
  CHECK_FALSE(std::filesystem::exists(f.string() + ".tmp"));
  std::filesystem::remove_all(dir);
}
