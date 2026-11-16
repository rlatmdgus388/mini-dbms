#include "sql/lexer.h"

#include <cctype>

namespace minidb {

namespace {
bool is_ident_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool is_ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }

Status lex_error(size_t pos, const std::string& msg) {
  return Status::fail(ErrorCode::kParse, "syntax error at position " + std::to_string(pos) + ": " + msg);
}
}  // namespace

Result<std::vector<Token>> tokenize(const std::string& sql) {
  std::vector<Token> tokens;
  size_t i = 0;
  const size_t n = sql.size();
  while (i < n) {
    char c = sql[i];
    if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
    if (c == '-' && i + 1 < n && sql[i + 1] == '-') {  // 한 줄 주석
      while (i < n && sql[i] != '\n') ++i;
      continue;
    }
    Token tok;
    tok.pos = i;
    if (is_ident_start(c)) {
      size_t j = i;
      while (j < n && is_ident_char(sql[j])) ++j;
      tok.type = TokenType::kIdent;
      tok.text = sql.substr(i, j - i);
      i = j;
    } else if (is_digit(c) || (c == '.' && i + 1 < n && is_digit(sql[i + 1]))) {
      size_t j = i;
      bool is_float = false;
      while (j < n && is_digit(sql[j])) ++j;
      if (j < n && sql[j] == '.') {
        is_float = true;
        ++j;
        while (j < n && is_digit(sql[j])) ++j;
      }
      if (j < n && (sql[j] == 'e' || sql[j] == 'E')) {
        size_t k = j + 1;
        if (k < n && (sql[k] == '+' || sql[k] == '-')) ++k;
        if (k < n && is_digit(sql[k])) {
          is_float = true;
          while (k < n && is_digit(sql[k])) ++k;
          j = k;
        }
      }
      if (j < n && is_ident_start(sql[j])) return lex_error(j, "invalid number literal");
      tok.type = is_float ? TokenType::kFloat : TokenType::kInteger;
      tok.text = sql.substr(i, j - i);
      i = j;
    } else if (c == '\'') {
      size_t j = i + 1;
      std::string value;
      bool closed = false;
      while (j < n) {
        if (sql[j] == '\'') {
          if (j + 1 < n && sql[j + 1] == '\'') { value.push_back('\''); j += 2; continue; }
          closed = true;
          ++j;
          break;
        }
        value.push_back(sql[j++]);
      }
      if (!closed) return lex_error(i, "unterminated string literal");
      tok.type = TokenType::kString;
      tok.text = std::move(value);
      i = j;
    } else {
      tok.type = TokenType::kSymbol;
      if ((c == '<' || c == '>' || c == '!') && i + 1 < n && sql[i + 1] == '=') {
        tok.text = sql.substr(i, 2);
        i += 2;
      } else if (c == '<' && i + 1 < n && sql[i + 1] == '>') {
        tok.text = "!=";  // <> 는 != 로 정규화
        i += 2;
      } else if (std::string("(),;*=<>-+").find(c) != std::string::npos) {
        tok.text = std::string(1, c);
        ++i;
      } else {
        return lex_error(i, std::string("unexpected character '") + c + "'");
      }
    }
    tokens.push_back(std::move(tok));
  }
  Token end;
  end.type = TokenType::kEnd;
  end.pos = n;
  tokens.push_back(end);
  return tokens;
}

}  // namespace minidb
