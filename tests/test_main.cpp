#include "framework.hpp"
#include <cstdio>

// Single definition of the harness globals/registry across all test TUs.
std::atomic<int> testfw::g_checks{0};
std::atomic<int> testfw::g_failures{0};
std::atomic<int> testfw::g_tests{0};
std::vector<testfw::Test>& testfw::registry() {
  static std::vector<testfw::Test> r;
  return r;
}

int main() {
  const int passed = testfw::run_all();
  std::printf("=== Model Cache test suite ===\n");
  std::printf("tests: %d  passed: %d  failures: %d  checks: %d\n",
              testfw::g_tests.load(), passed, testfw::g_failures.load(), testfw::g_checks.load());
  return testfw::g_failures.load() == 0 ? 0 : 1;
}
