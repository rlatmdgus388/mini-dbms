#include <cstring>

#include "test_util.h"

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int ran = 0, failed_tests = 0;
  for (const auto& t : testing::registry()) {
    if (filter && !std::strstr(t.name, filter)) continue;
    int before = testing::failure_count();
    std::printf("[ RUN  ] %s\n", t.name);
    std::fflush(stdout);
    t.fn();
    ++ran;
    if (testing::failure_count() > before) {
      ++failed_tests;
      std::printf("[ FAIL ] %s\n", t.name);
    } else {
      std::printf("[  OK  ] %s\n", t.name);
    }
  }
  std::printf("\n%d tests run, %d passed, %d failed\n", ran, ran - failed_tests, failed_tests);
  return failed_tests == 0 ? 0 : 1;
}
