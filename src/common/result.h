#pragma once
#include <optional>
#include <string>
#include <utility>

namespace minidb {

// 에러 분류. 서버는 이 코드로 HTTP 상태를 결정한다 (kIo/kInternal -> 5xx, 나머지 -> 4xx).
enum class ErrorCode { kOk, kParse, kInvalid, kNotFound, kIo, kInternal };

class Status {
 public:
  Status() = default;
  static Status success() { return Status(); }
  static Status fail(ErrorCode code, std::string message) {
    Status s;
    s.code_ = code;
    s.message_ = std::move(message);
    return s;
  }
  bool ok() const { return code_ == ErrorCode::kOk; }
  ErrorCode code() const { return code_; }
  const std::string& message() const { return message_; }

 private:
  ErrorCode code_ = ErrorCode::kOk;
  std::string message_;
};

// 값 또는 에러. 예외 대신 사용한다 (CLAUDE.md 절대 규칙 7).
template <class T>
class Result {
 public:
  Result(T value) : value_(std::move(value)) {}      // NOLINT: 암시적 변환 의도
  Result(Status status) : status_(std::move(status)) {}  // NOLINT: 반드시 실패 상태여야 함

  bool ok() const { return value_.has_value(); }
  const Status& status() const { return status_; }
  T& value() { return *value_; }
  const T& value() const { return *value_; }
  T take() { return std::move(*value_); }

 private:
  std::optional<T> value_;
  Status status_;
};

}  // namespace minidb
