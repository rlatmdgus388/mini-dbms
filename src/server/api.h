#pragma once
#include <string>

#include "common/result.h"
#include "engine/engine.h"

namespace minidb {

// 실행 결과를 API 응답 JSON 으로 직렬화한다.
//   {"ok":true,"results":[{"kind":"SELECT","columns":[..],"rows":[[..]],"plan":"INDEX","count":N},
//                         {"kind":"INSERT","affected":N,"first_id":a,"last_id":b}],"elapsed_ms":1.23}
//   실패: {"ok":false,"error":"...","results":[앞서 성공한 문장들],"elapsed_ms":..}
std::string outcome_to_json(const ExecOutcome& outcome, double elapsed_ms);
std::string error_to_json(const std::string& message);
// 엔진 에러 코드 -> HTTP 상태 (사용자 오류 4xx, 서버 오류 5xx).
int http_status_for(ErrorCode code);

// POST /query 본문에서 SQL 을 꺼낸다: {"sql":"..."} 이거나 text/plain 원문.
Result<std::string> extract_sql(const std::string& content_type, const std::string& body);

}  // namespace minidb
