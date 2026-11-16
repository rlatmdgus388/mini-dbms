#pragma once
// 자체 경량 테스트 하네스 (외부 라이브러리 금지 - CLAUDE.md 절대 규칙 1).
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "engine/engine.h"

namespace testing {

struct TestCase {
  const char* name;
  void (*fn)();
};
inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> r;
  return r;
}
inline std::atomic<int>& failure_count() {
  static std::atomic<int> n{0};
  return n;
}
struct Registrar {
  Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

// 테스트 전용 임시 디렉터리. build/test_tmp/ 아래에만 만들고, 소멸 시 자기 디렉터리만 삭제한다
// (CLAUDE.md 절대 규칙 5: data/ 는 절대 건드리지 않는다).
class TempDir {
 public:
  TempDir() {
    std::filesystem::create_directories("build/test_tmp");
    std::string tmpl = "build/test_tmp/t_XXXXXX";
    char* p = mkdtemp(tmpl.data());
    path_ = p ? p : "";
  }
  ~TempDir() {
    if (path_.rfind("build/test_tmp/t_", 0) == 0) {
      std::error_code ec;
      std::filesystem::remove_all(path_, ec);
    }
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

inline const char* kUsersSchema =
    R"J({"table":"users","columns":[{"name":"id","type":"BIGINT"},{"name":"name","type":"VARCHAR(10)"},)J"
    R"J({"name":"age","type":"INT"},{"name":"score","type":"DOUBLE"}]})J";

inline void write_table_schema(const std::string& data_dir, const std::string& table, const std::string& json) {
  std::filesystem::create_directories(data_dir + "/" + table);
  std::ofstream(data_dir + "/" + table + "/schema.json") << json;
}

}  // namespace testing

#define TEST(name)                                           \
  static void name();                                        \
  static ::testing::Registrar registrar_##name(#name, name); \
  static void name()

#define CHECK(cond)                                                                   \
  do {                                                                                \
    if (!(cond)) {                                                                    \
      std::fprintf(stderr, "  FAIL %s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond);   \
      ::testing::failure_count()++;                                                   \
    }                                                                                 \
  } while (0)

#define CHECK_EQ(a, b)                                                                              \
  do {                                                                                              \
    auto va_ = (a);                                                                                 \
    auto vb_ = (b);                                                                                 \
    if (!(va_ == vb_)) {                                                                            \
      std::fprintf(stderr, "  FAIL %s:%d: CHECK_EQ(%s, %s)\n", __FILE__, __LINE__, #a, #b);          \
      ::testing::failure_count()++;                                                                 \
    }                                                                                               \
  } while (0)

// 에러 상태여야 하며 메시지에 substr 이 포함되어야 한다.
#define CHECK_ERR(status_expr, substr)                                                                       \
  do {                                                                                                       \
    ::minidb::Status st_ = (status_expr);                                                                    \
    if (st_.ok() || st_.message().find(substr) == std::string::npos) {                                       \
      std::fprintf(stderr, "  FAIL %s:%d: expected error containing '%s' but got '%s'\n", __FILE__, __LINE__, \
                   substr, st_.ok() ? "<ok>" : st_.message().c_str());                                       \
      ::testing::failure_count()++;                                                                          \
    }                                                                                                        \
  } while (0)
