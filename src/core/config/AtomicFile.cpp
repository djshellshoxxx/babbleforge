#include "core/config/AtomicFile.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace bf {
namespace fs = std::filesystem;

bool writeFileAtomic(const fs::path& path, std::string_view data, std::string* error) {
  auto fail = [&](const std::string& m) {
    if (error) *error = m;
    return false;
  };
  std::error_code ec;
  if (path.has_parent_path()) {
    fs::create_directories(path.parent_path(), ec);
    if (ec) return fail("cannot create directory: " + ec.message());
  }
  fs::path tmp = path;
  tmp += ".tmp";
  {
    std::FILE* f = std::fopen(tmp.string().c_str(), "wb");
    if (!f) return fail("cannot open " + tmp.string());
    bool ok = data.empty() || std::fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = ok && std::fflush(f) == 0;
#ifdef _WIN32
    ok = ok && _commit(_fileno(f)) == 0;
#else
    ok = ok && fsync(fileno(f)) == 0;
#endif
    ok = (std::fclose(f) == 0) && ok;
    if (!ok) {
      fs::remove(tmp, ec);
      return fail("write failed: " + tmp.string());
    }
  }
  if (fs::exists(path, ec)) {
    fs::path bak = path;
    bak += ".bak";
    fs::copy_file(path, bak, fs::copy_options::overwrite_existing, ec);
    if (ec) {
      fs::remove(tmp, ec);
      return fail("cannot create backup: " + ec.message());
    }
  }
  fs::rename(tmp, path, ec);
  if (ec) {
    std::error_code ec2;
    fs::remove(tmp, ec2);
    return fail("rename failed: " + ec.message());
  }
  return true;
}

bool readFile(const fs::path& path, std::string& out, std::string* error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (error) *error = "cannot open " + path.string();
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

}  // namespace bf
