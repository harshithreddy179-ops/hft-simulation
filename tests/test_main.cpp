#include <chrono>
#include <cstring>
#include <exception>
#include <iostream>

#include "testing.hpp"

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int ran = 0, failed_tests = 0;
  const auto t0 = std::chrono::steady_clock::now();
  for (const auto& t : qt::registry()) {
    if (filter && !std::strstr(t.name, filter)) continue;
    const int before = qt::failures();
    try {
      t.fn();
    } catch (const std::exception& e) {
      qt::report_failure(__FILE__, __LINE__, std::string("uncaught exception: ") + e.what());
    }
    ++ran;
    const bool ok = qt::failures() == before;
    if (!ok) ++failed_tests;
    std::cout << (ok ? "[ ok ] " : "[FAIL] ") << t.name << "\n";
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::cout << "\n" << ran << " tests, " << failed_tests << " failed (" << secs << "s)\n";
  return failed_tests == 0 ? 0 : 1;
}
