// minidb CLI: SQL 텍스트(-e / --file / stdin)를 엔진에 전달하고 결과를 표로 출력한다.
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

#include "engine/engine.h"

using namespace minidb;

namespace {

void print_usage() {
  std::fprintf(stderr,
               "usage: minidb [--data-dir DIR] [--timing] (-e \"SQL\" | --file query.sql | < query.sql)\n"
               "  --data-dir DIR   directory containing <table>/schema.json (default: data)\n"
               "  -e SQL           execute the given SQL text\n"
               "  --file PATH      execute SQL from a text file\n"
               "  --timing         print elapsed time per statement batch\n");
}

size_t display_width(const std::string& s) {
  size_t w = 0;
  for (unsigned char c : s)
    if ((c & 0xC0) != 0x80) ++w;  // UTF-8 연속 바이트는 건너뛴다
  return w;
}

void print_select(const QueryResult& r) {
  std::vector<std::vector<std::string>> cells;
  std::vector<size_t> width;
  for (const auto& c : r.columns) width.push_back(display_width(c));
  for (const Row& row : r.rows) {
    std::vector<std::string> line;
    for (size_t i = 0; i < row.size(); ++i) {
      line.push_back(value_to_string(row[i]));
      width[i] = std::max(width[i], display_width(line.back()));
    }
    cells.push_back(std::move(line));
  }
  auto print_line = [&](const std::vector<std::string>& line) {
    for (size_t i = 0; i < line.size(); ++i) {
      std::printf("%s%s%s", i ? " | " : "", line[i].c_str(), std::string(width[i] - display_width(line[i]), ' ').c_str());
    }
    std::printf("\n");
  };
  print_line(r.columns);
  for (size_t i = 0; i < width.size(); ++i) std::printf("%s%s", i ? "-+-" : "", std::string(width[i], '-').c_str());
  std::printf("\n");
  for (const auto& line : cells) print_line(line);
  std::printf("(%zu row%s, plan=%s)\n", r.rows.size(), r.rows.size() == 1 ? "" : "s", r.plan.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  std::string data_dir = "data";
  std::string sql;
  bool have_sql = false;
  bool timing = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "error: %s requires a value\n", name);
        print_usage();
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--data-dir") {
      data_dir = next("--data-dir");
    } else if (a == "-e") {
      sql = next("-e");
      have_sql = true;
    } else if (a == "--file") {
      std::ifstream in(next("--file"), std::ios::binary);
      if (!in) {
        std::fprintf(stderr, "error: cannot read file '%s'\n", argv[i]);
        return 2;
      }
      std::ostringstream ss;
      ss << in.rdbuf();
      sql = ss.str();
      have_sql = true;
    } else if (a == "--timing") {
      timing = true;
    } else if (a == "-h" || a == "--help") {
      print_usage();
      return 0;
    } else {
      std::fprintf(stderr, "error: unknown argument '%s'\n", a.c_str());
      print_usage();
      return 2;
    }
  }
  if (!have_sql) {
    if (isatty(STDIN_FILENO)) {
      print_usage();
      return 2;
    }
    std::ostringstream ss;
    ss << std::cin.rdbuf();
    sql = ss.str();
  }

  Result<std::unique_ptr<Engine>> engine = Engine::open(data_dir);
  if (!engine.ok()) {
    std::fprintf(stderr, "ERROR: %s\n", engine.status().message().c_str());
    return 1;
  }
  auto start = std::chrono::steady_clock::now();
  ExecOutcome outcome = engine.value()->execute(sql);
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

  for (const QueryResult& r : outcome.results) {
    if (r.kind == "INSERT") {
      std::printf("OK, %llu row(s) inserted (id %lld..%lld)\n", static_cast<unsigned long long>(r.affected),
                  static_cast<long long>(r.first_id), static_cast<long long>(r.last_id));
    } else {
      print_select(r);
    }
  }
  if (timing) std::printf("elapsed: %.3f ms\n", ms);
  if (!outcome.status.ok()) {
    std::fprintf(stderr, "ERROR: %s\n", outcome.status.message().c_str());
    return 1;
  }
  return 0;
}
