#ifndef SLINKY_BUILDER_TEST_UTIL_H
#define SLINKY_BUILDER_TEST_UTIL_H

#include <fstream>
#include <string>

#include <gtest/gtest.h>

namespace slinky {

template <typename T>
std::string str_join(const std::string&, const T& t) {
  using std::to_string;
  return to_string(t);
}

template <typename T, typename... Ts>
std::string str_join(const std::string& separator, const T& t, const Ts&... ts) {
  using std::to_string;
  return to_string(t) + separator + str_join(separator, ts...);
}

template <typename T, std::size_t... Is>
std::string test_params_to_string_impl(const T& t, std::index_sequence<Is...>) {
  return str_join("_", std::get<Is>(t)...);
}

template <typename T>
std::string test_params_to_string(const testing::TestParamInfo<T>& info) {
  constexpr std::size_t n = std::tuple_size<T>();
  return test_params_to_string_impl(info.param, std::make_index_sequence<n>());
}

inline std::string remove_windows_newlines(std::string s) {
  s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
  return s;
}

inline std::string read_entire_file(const std::string& pathname) {
  std::ifstream f(pathname);
  std::stringstream buffer;
  buffer << f.rdbuf();
  return remove_windows_newlines(buffer.str());
}

}  // namespace slinky

#endif  // SLINKY_BUILDER_TEST_UTIL_H