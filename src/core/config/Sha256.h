#pragma once
// Small self-contained SHA-256 (FIPS 180-4).
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace bf {

class Sha256 {
 public:
  Sha256();
  void update(const void* data, std::size_t len);
  void update(std::string_view s) { update(s.data(), s.size()); }
  // Finalizes the hash; the object must not be updated afterwards.
  std::array<std::uint8_t, 32> finish();
  std::string finishHex();

 private:
  void block(const std::uint8_t* p);
  std::uint32_t h_[8];
  std::uint8_t buf_[64];
  std::size_t bufLen_ = 0;
  std::uint64_t total_ = 0;
};

std::string toHex(const std::array<std::uint8_t, 32>& d);
std::string sha256Hex(std::string_view data);

}  // namespace bf
