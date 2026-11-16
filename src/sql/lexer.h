#pragma once
#include <string>
#include <vector>

#include "common/result.h"

namespace minidb {

enum class TokenType { kIdent, kInteger, kFloat, kString, kSymbol, kEnd };

struct Token {
  TokenType type = TokenType::kEnd;
  std::string text;  // kIdent는 소문자화하지 않은 원문, kString은 이스케이프 해제된 내용
  size_t pos = 0;    // 입력 내 바이트 오프셋 (에러 메시지용)
};

// SQL 문자열을 토큰으로 분해한다. `--` 한 줄 주석과 'it''s' 문자열 이스케이프를 지원한다.
Result<std::vector<Token>> tokenize(const std::string& sql);

}  // namespace minidb
