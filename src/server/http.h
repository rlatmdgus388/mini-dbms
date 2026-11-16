#pragma once
#include <map>
#include <string>

namespace minidb {

struct HttpRequest {
  std::string method;
  std::string path;  // 쿼리 스트링 제외
  std::map<std::string, std::string> headers;  // 키는 소문자
  std::string body;
};

constexpr size_t kMaxHeaderBytes = 16 * 1024;

// 요청 헤더 블록("\r\n\r\n" 앞까지)을 파싱한다. 성공하면 0, 실패하면 응답할 HTTP 상태 코드(400/501/505)를 반환하고
// *error 에 사유를 넣는다. *content_length 는 본문 길이 (헤더가 없으면 0).
int parse_request_head(const std::string& head, HttpRequest* req, size_t* content_length, std::string* error);

const char* http_status_text(int status);
// 완성된 HTTP/1.1 응답 (Connection: close).
std::string build_http_response(int status, const std::string& body, const std::string& content_type = "application/json; charset=utf-8");

}  // namespace minidb
