#pragma once
// Minimal, dependency-free test harness for Model Cache. No timeouts are used:
// tests run to natural completion. A CHECK that fails records the failure and
// continues, so one run exercises the maximum surface.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

#include "model_cache/error.hpp"

namespace testfw {

extern std::atomic<int> g_checks;
extern std::atomic<int> g_failures;
extern std::atomic<int> g_tests;

struct Test {
  std::string name;
  std::function<void()> fn;
};
std::vector<Test>& registry();

struct Registrar {
  Registrar(std::string name, std::function<void()> fn) { registry().push_back({std::move(name), std::move(fn)}); }
};

inline int run_all() {
  std::printf("# run: %zu tests\n", registry().size());
  int passed = 0;
  for (auto& t : registry()) {
    ++g_tests;
    std::printf("[TEST] %s\n", t.name.c_str());
    std::fflush(stdout);
    const int before = g_failures.load();
    try {
      t.fn();
    } catch (const std::exception& e) {
      ++g_failures;
      std::printf("EXCEPTION in %s: %s\n", t.name.c_str(), e.what());
    } catch (...) {
      ++g_failures;
      std::printf("UNKNOWN EXCEPTION in %s\n", t.name.c_str());
    }
    if (g_failures.load() == before) ++passed;
  }
  return passed;
}

template <typename T>
std::string show(const T& v) {
  if constexpr (std::is_pointer_v<T>) {
    if (v == nullptr) return "nullptr";
    if constexpr (std::is_same_v<std::remove_cv_t<std::remove_pointer_t<T>>, char>) {
      return std::string(reinterpret_cast<const char*>(v));
    } else {
      return std::to_string(reinterpret_cast<std::uintptr_t>(v));
    }
  } else if constexpr (std::is_same_v<std::decay_t<T>, std::string>) {
    return v;
  } else if constexpr (std::is_same_v<std::decay_t<T>, bool>) {
    return v ? "true" : "false";
  } else if constexpr (std::is_enum_v<std::decay_t<T>>) {
    return std::to_string(static_cast<int>(v));
  } else if constexpr (std::is_arithmetic_v<std::decay_t<T>>) {
    return std::to_string(v);
  } else {
    return "<value>";
  }
}

}  // namespace testfw

#define TEST(name)                                                        \
  static void name();                                                     \
  static ::testfw::Registrar reg_##name(#name, []() { name(); });         \
  static void name()

#define CHECK(cond)                                                       \
  do {                                                                    \
    ++::testfw::g_checks;                                                 \
    if (!(cond)) {                                                        \
      ++::testfw::g_failures;                                             \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
    }                                                                     \
  } while (0)

#define CHECK_EQ(a, b)                                                    \
  do {                                                                    \
    ++::testfw::g_checks;                                                 \
    const auto va = (a);                                                  \
    const auto vb = (b);                                                  \
    if (!(va == vb)) {                                                    \
      ++::testfw::g_failures;                                             \
      std::printf("FAIL %s:%d  %s == %s\n", __FILE__, __LINE__, #a, #b); \
      std::printf("    left: %s\n    right: %s\n",                       \
                  ::testfw::show(va).c_str(), ::testfw::show(vb).c_str());\
    }                                                                     \
  } while (0)

namespace testfw {
template <typename Fn>
bool throw_code(Fn&& fn, int expected) {
  bool threw = false;
  try {
    fn();
  } catch (const model_cache::Error& e) {
    threw = (static_cast<int>(e.code()) == expected);
  } catch (...) {
    threw = false;
  }
  return threw;
}
}  // namespace testfw

#define CHECK_THROWS(expr, code)                                          \
  do {                                                                    \
    ++::testfw::g_checks;                                                 \
    const bool ok = ::testfw::throw_code([&]() { (void)(expr); },         \
                                         static_cast<int>(code));         \
    if (!ok) {                                                            \
      ++::testfw::g_failures;                                             \
      std::printf("FAIL %s:%d  expected throw: %s\n", __FILE__, __LINE__, #expr); \
    }                                                                     \
  } while (0)

#define CHECK_GE(a, b)                                                    \
  do {                                                                    \
    ++::testfw::g_checks;                                                 \
    if (!((a) >= (b))) {                                                  \
      ++::testfw::g_failures;                                             \
      std::printf("FAIL %s:%d  %s >= %s\n", __FILE__, __LINE__, #a, #b); \
    }                                                                     \
  } while (0)

#define CHECK_LE(a, b)                                                    \
  do {                                                                    \
    ++::testfw::g_checks;                                                 \
    if (!((a) <= (b))) {                                                  \
      ++::testfw::g_failures;                                             \
      std::printf("FAIL %s:%d  %s <= %s\n", __FILE__, __LINE__, #a, #b); \
    }                                                                     \
  } while (0)

#define CHECK_NO_THROW(expr)                                              \
  do {                                                                    \
    ++::testfw::g_checks;                                                 \
    try { (void)(expr); }                                                 \
    catch (...) {                                                         \
      ++::testfw::g_failures;                                             \
      std::printf("FAIL %s:%d  unexpected throw\n", __FILE__, __LINE__); \
    }                                                                     \
  } while (0)