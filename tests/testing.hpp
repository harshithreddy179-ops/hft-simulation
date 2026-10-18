#pragma once
// Minimal self-registering test framework (no external dependency, so the repo builds anywhere).
//
//   TEST(order_book_basic) { CHECK(x == 1); CHECK_EQ(a, b); CHECK_NEAR(x, y, 1e-9); }
//
// Run all:           ./qc_tests
// Run by substring:  ./qc_tests risk

#include <cmath>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace qt {

struct TestCase {
  const char* name;
  void (*fn)();
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> r;
  return r;
}

inline int& failures() {
  static int f = 0;
  return f;
}

struct Registrar {
  Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline void report_failure(const char* file, int line, const std::string& msg) {
  ++failures();
  std::cerr << "    FAIL " << file << ":" << line << "  " << msg << "\n";
}

template <class A, class B>
inline void check_eq(const A& a, const B& b, const char* ea, const char* eb, const char* file, int line) {
  if (!(a == b)) {
    std::ostringstream os;
    os << ea << " == " << eb << "   (" << +a << " vs " << +b << ")";
    report_failure(file, line, os.str());
  }
}

}  // namespace qt

#define TEST(name)                                          \
  static void test_##name();                                \
  static qt::Registrar registrar_##name(#name, test_##name); \
  static void test_##name()

#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) qt::report_failure(__FILE__, __LINE__, #cond);    \
  } while (0)

#define CHECK_MSG(cond, msg)                                       \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::ostringstream os_;                                      \
      os_ << #cond << "  " << msg;                                 \
      qt::report_failure(__FILE__, __LINE__, os_.str());           \
    }                                                              \
  } while (0)

#define CHECK_EQ(a, b) qt::check_eq((a), (b), #a, #b, __FILE__, __LINE__)

#define CHECK_NEAR(a, b, tol)                                                          \
  do {                                                                                 \
    const double a_ = static_cast<double>(a), b_ = static_cast<double>(b);             \
    if (!(std::fabs(a_ - b_) <= (tol))) {                                              \
      std::ostringstream os_;                                                          \
      os_ << #a << " ~= " << #b << "   (" << a_ << " vs " << b_ << ")";                \
      qt::report_failure(__FILE__, __LINE__, os_.str());                               \
    }                                                                                  \
  } while (0)
