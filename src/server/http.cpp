#include "server/http.h"

#include <algorithm>
#include <cctype>

namespace minidb {

namespace {
std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}
std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
  return s.substr(a, b - a);
}
}  // namespace

int parse_request_head(const std::string& head, HttpRequest* req, size_t* content_length, std::string* error) {
  *content_length = 0;
  size_t line_end = head.find("\r\n");
  std::string request_line = head.substr(0, line_end);
  // "METHOD SP TARGET SP VERSION"
  size_t s1 = request_line.find(' ');
  size_t s2 = s1 == std::string::npos ? std::string::npos : request_line.find(' ', s1 + 1);
  if (s1 == std::string::npos || s2 == std::string::npos || s1 == 0 || s2 == s1 + 1 ||
      request_line.find(' ', s2 + 1) != std::string::npos) {
    *error = "malformed request line";
    return 400;
  }
  req->method = request_line.substr(0, s1);
  std::string target = request_line.substr(s1 + 1, s2 - s1 - 1);
  std::string version = request_line.substr(s2 + 1);
  if (version != "HTTP/1.1" && version != "HTTP/1.0") {
    *error = "unsupported HTTP version";
    return 505;
  }
  for (unsigned char c : req->method)
    if (!std::isupper(c)) {
      *error = "malformed method";
      return 400;
    }
  if (target.empty() || target[0] != '/') {
    *error = "request target must start with '/'";
    return 400;
  }
  req->path = target.substr(0, target.find('?'));

  size_t pos = line_end == std::string::npos ? head.size() : line_end + 2;
  bool have_length = false;
  while (pos < head.size()) {
    size_t e = head.find("\r\n", pos);
    std::string line = head.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
    pos = e == std::string::npos ? head.size() : e + 2;
    if (line.empty()) continue;
    if (line.find('\n') != std::string::npos || line.find('\r') != std::string::npos) {  // bare LF/CR: 스머글링 방지
      *error = "malformed header line";
      return 400;
    }
    size_t colon = line.find(':');
    if (colon == std::string::npos || colon == 0) {
      *error = "malformed header line";
      return 400;
    }
    std::string name = to_lower(line.substr(0, colon));
    std::string value = trim(line.substr(colon + 1));
    if (name == "transfer-encoding") {
      *error = "Transfer-Encoding is not supported; use Content-Length";
      return 501;
    }
    if (name == "content-length") {
      if (value.empty() || value.size() > 18 || !std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); })) {
        *error = "invalid Content-Length";
        return 400;
      }
      size_t n = static_cast<size_t>(std::stoull(value));
      if (have_length && n != *content_length) {  // 요청 스머글링 방지: 상충하는 중복 헤더 거부
        *error = "conflicting Content-Length headers";
        return 400;
      }
      *content_length = n;
      have_length = true;
    }
    req->headers[name] = value;
  }
  return 0;
}

const char* http_status_text(int status) {
  switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Payload Too Large";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    default: return "Unknown";
  }
}

std::string build_http_response(int status, const std::string& body, const std::string& content_type) {
  std::string r = "HTTP/1.1 " + std::to_string(status) + " " + http_status_text(status) + "\r\n";
  r += "Content-Type: " + content_type + "\r\n";
  r += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  r += "Connection: close\r\n\r\n";
  r += body;
  return r;
}

}  // namespace minidb
