#pragma once
// Internal helpers for typed JSON reading with field paths. Only included from .cpp files.
#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>

#include <nlohmann/json.hpp>

namespace bf::detail {

using json = nlohmann::json;

struct ParseError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

inline std::string join(const std::string& path, const std::string& key) {
  return path.empty() ? key : path + "." + key;
}

template <class T>
T conv(const json& v, const std::string& path) {
  if constexpr (std::is_same_v<T, bool>) {
    if (!v.is_boolean()) throw ParseError(path + ": expected boolean");
    return v.get<bool>();
  } else if constexpr (std::is_same_v<T, std::string>) {
    if (!v.is_string()) throw ParseError(path + ": expected string");
    return v.get<std::string>();
  } else if constexpr (std::is_integral_v<T>) {
    if (!v.is_number()) throw ParseError(path + ": expected integer");
    if (v.is_number_float()) {
      double d = v.get<double>();
      if (d != std::floor(d)) throw ParseError(path + ": expected integer");
      return static_cast<T>(d);
    }
    return v.get<T>();
  } else {
    if (!v.is_number()) throw ParseError(path + ": expected number");
    return v.get<T>();
  }
}

// Returns the member or nullptr when absent or null.
inline const json* member(const json& o, const char* key) {
  if (!o.is_object()) return nullptr;
  auto it = o.find(key);
  if (it == o.end() || it->is_null()) return nullptr;
  return &*it;
}

template <class T>
std::optional<T> getOpt(const json& o, const char* key, const std::string& path) {
  const json* v = member(o, key);
  if (!v) return std::nullopt;
  return conv<T>(*v, join(path, key));
}

template <class T>
T getReq(const json& o, const char* key, const std::string& path) {
  const json* v = member(o, key);
  if (!v) throw ParseError(join(path, key) + ": missing required field");
  return conv<T>(*v, join(path, key));
}

template <class T>
T getDef(const json& o, const char* key, const std::string& path, T def) {
  auto v = getOpt<T>(o, key, path);
  return v ? *v : def;
}

// Returns the object member, nullptr when absent/null, throws if present but not an object.
inline const json* getObj(const json& o, const char* key, const std::string& path) {
  const json* v = member(o, key);
  if (!v) return nullptr;
  if (!v->is_object()) throw ParseError(join(path, key) + ": expected object");
  return v;
}

inline const json* getArr(const json& o, const char* key, const std::string& path) {
  const json* v = member(o, key);
  if (!v) return nullptr;
  if (!v->is_array()) throw ParseError(join(path, key) + ": expected array");
  return v;
}

template <class T>
std::vector<T> getVec(const json& o, const char* key, const std::string& path) {
  std::vector<T> out;
  if (const json* a = getArr(o, key, path)) {
    std::size_t i = 0;
    for (const auto& e : *a) out.push_back(conv<T>(e, join(path, key) + "[" + std::to_string(i++) + "]"));
  }
  return out;
}

}  // namespace bf::detail
