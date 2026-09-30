#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace bf {

// Atomically replaces `path` with `data`: writes "<path>.tmp", flushes and fsyncs it,
// copies any existing file to "<path>.bak", then renames the temp file over `path`.
// Returns false and fills `error` (if non-null) on failure; the original is left intact.
bool writeFileAtomic(const std::filesystem::path& path, std::string_view data, std::string* error = nullptr);

// Reads a whole file. Returns false on failure.
bool readFile(const std::filesystem::path& path, std::string& out, std::string* error = nullptr);

}  // namespace bf
