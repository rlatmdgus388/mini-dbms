#pragma once
#include <cstdint>
#include <string>

#include "common/result.h"
#include "storage/schema.h"

namespace minidb {

// 행 직렬화. 컬럼 순서대로: INT/BIGINT = int64(8B), DOUBLE = 8B, VARCHAR = u32 길이 + 바이트.
// (리틀엔디안 호스트를 가정하고 memcpy로 기록한다.)
// 호출자는 row가 스키마와 타입이 일치함을 보장해야 한다 (executor가 coerce 한 뒤 호출).
void encode_row(const Schema& schema, const Row& row, std::string* out);
Result<Row> decode_row(const Schema& schema, const char* data, size_t len);
// 레코드 첫 8바이트가 id (id는 항상 첫 BIGINT 컬럼). 인덱스 재구축 시 전체 디코딩 없이 사용.
bool peek_id(const char* data, size_t len, int64_t* id);

}  // namespace minidb
