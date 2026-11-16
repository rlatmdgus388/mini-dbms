#pragma once
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common/result.h"
#include "engine/table.h"
#include "sql/ast.h"

namespace minidb {

struct QueryResult {
  std::string kind;                  // "INSERT" | "SELECT"
  std::vector<std::string> columns;  // SELECT 전용
  std::vector<Row> rows;             // SELECT 전용
  std::string plan;                  // SELECT 전용: "INDEX" | "SCAN"
  uint64_t affected = 0;             // INSERT: 삽입된 행 수
  int64_t first_id = 0;              // INSERT: 부여된 id 범위
  int64_t last_id = 0;
};

// 스크립트 실행 결과. 중간에 실패하면 그 전까지의 결과는 results에 남고 status가 실패 상태가 된다.
struct ExecOutcome {
  std::vector<QueryResult> results;
  Status status;
};

// SQL 처리 파이프라인의 진입점: 파싱 -> 실행 -> 저장. CLI·벤치마크·서버 모두 execute()를 통한다.
// 스레드 안전: 테이블 맵은 mutex로, 테이블 데이터는 Table 내부 shared_mutex로 보호된다.
class Engine {
 public:
  // data_dir/<table>/{schema.json,data.bin}. data_dir 가 없으면 에러.
  static Result<std::unique_ptr<Engine>> open(const std::string& data_dir);

  ExecOutcome execute(const std::string& sql);
  Result<QueryResult> execute_statement(const Statement& stmt);

  // 테이블을 (필요하면 열어서) 반환한다. 반환된 포인터는 Engine 수명 동안 유효.
  Result<Table*> get_table(const std::string& name);
  // data_dir 아래의 모든 테이블을 미리 연다 (서버 시작 시 인덱스를 미리 구축).
  Status load_all_tables();

 private:
  explicit Engine(std::string data_dir) : data_dir_(std::move(data_dir)) {}
  Result<QueryResult> execute_insert(const InsertStmt& stmt);
  Result<QueryResult> execute_select(const SelectStmt& stmt);

  std::string data_dir_;
  std::mutex tables_mutex_;
  std::map<std::string, std::unique_ptr<Table>> tables_;
};

}  // namespace minidb
