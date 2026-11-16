#pragma once
#include <string>
#include <vector>

#include "common/result.h"
#include "sql/ast.h"

namespace minidb {

// 세미콜론으로 구분된 여러 문장을 파싱한다. 빈 문장(;;)은 무시하고 마지막 세미콜론은 생략할 수 있다.
// 지원: INSERT INTO t [(c,...)] VALUES (...),(...) / SELECT cols|* FROM t [WHERE ... AND ...] [LIMIT n]
Result<std::vector<Statement>> parse_script(const std::string& sql);

}  // namespace minidb
