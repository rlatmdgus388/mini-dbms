#include "common/json.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace minidb {

const JsonValue* JsonValue::find(const std::string& key) const {
  for (size_t i = 0; i < keys.size(); ++i) {
    if (keys[i] == key) return &values[i];
  }
  return nullptr;
}

namespace {

constexpr int kMaxDepth = 64;

class Parser {
 public:
  explicit Parser(const std::string& t) : t_(t) {}

  Result<JsonValue> parse() {
    JsonValue v;
    Status st = parse_value(&v, 0);
    if (!st.ok()) return st;
    skip_ws();
    if (pos_ != t_.size()) return err("unexpected trailing characters");
    return v;
  }

 private:
  Status err(const std::string& m) const {
    return Status::fail(ErrorCode::kParse, "JSON error at offset " + std::to_string(pos_) + ": " + m);
  }
  void skip_ws() {
    while (pos_ < t_.size() && (t_[pos_] == ' ' || t_[pos_] == '\t' || t_[pos_] == '\n' || t_[pos_] == '\r')) ++pos_;
  }
  bool consume(const char* lit) {
    size_t n = std::char_traits<char>::length(lit);
    if (t_.compare(pos_, n, lit) == 0) {
      pos_ += n;
      return true;
    }
    return false;
  }

  Status parse_value(JsonValue* out, int depth) {
    if (depth > kMaxDepth) return err("nesting too deep");
    skip_ws();
    if (pos_ >= t_.size()) return err("unexpected end of input");
    char c = t_[pos_];
    if (c == '{') return parse_object(out, depth);
    if (c == '[') return parse_array(out, depth);
    if (c == '"') {
      out->type = JsonValue::Type::kString;
      return parse_string(&out->string);
    }
    if (consume("true")) { out->type = JsonValue::Type::kBool; out->boolean = true; return Status::success(); }
    if (consume("false")) { out->type = JsonValue::Type::kBool; out->boolean = false; return Status::success(); }
    if (consume("null")) { out->type = JsonValue::Type::kNull; return Status::success(); }
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number(out);
    return err("unexpected character");
  }

  Status parse_number(JsonValue* out) {
    size_t start = pos_;
    if (t_[pos_] == '-') ++pos_;
    while (pos_ < t_.size() && ((t_[pos_] >= '0' && t_[pos_] <= '9') || t_[pos_] == '.' || t_[pos_] == 'e' ||
                                t_[pos_] == 'E' || t_[pos_] == '+' || t_[pos_] == '-'))
      ++pos_;
    std::string tok = t_.substr(start, pos_ - start);
    char* end = nullptr;
    errno = 0;
    double d = std::strtod(tok.c_str(), &end);
    if (end == tok.c_str() || *end != '\0' || !std::isfinite(d)) return err("invalid number");
    out->type = JsonValue::Type::kNumber;
    out->number = d;
    return Status::success();
  }

  static void append_utf8(std::string* s, uint32_t cp) {
    if (cp < 0x80) {
      s->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      s->push_back(static_cast<char>(0xC0 | (cp >> 6)));
      s->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      s->push_back(static_cast<char>(0xE0 | (cp >> 12)));
      s->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      s->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      s->push_back(static_cast<char>(0xF0 | (cp >> 18)));
      s->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      s->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      s->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool parse_hex4(uint32_t* out) {
    if (pos_ + 4 > t_.size()) return false;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      char c = t_[pos_ + i];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
      else return false;
    }
    pos_ += 4;
    *out = v;
    return true;
  }

  Status parse_string(std::string* out) {
    ++pos_;  // opening quote
    out->clear();
    while (pos_ < t_.size()) {
      char c = t_[pos_++];
      if (c == '"') return Status::success();
      if (static_cast<unsigned char>(c) < 0x20) return err("control character in string");
      if (c != '\\') { out->push_back(c); continue; }
      if (pos_ >= t_.size()) break;
      char e = t_[pos_++];
      switch (e) {
        case '"': out->push_back('"'); break;
        case '\\': out->push_back('\\'); break;
        case '/': out->push_back('/'); break;
        case 'b': out->push_back('\b'); break;
        case 'f': out->push_back('\f'); break;
        case 'n': out->push_back('\n'); break;
        case 'r': out->push_back('\r'); break;
        case 't': out->push_back('\t'); break;
        case 'u': {
          uint32_t cp = 0;
          if (!parse_hex4(&cp)) return err("invalid \\u escape");
          if (cp >= 0xD800 && cp <= 0xDBFF) {  // surrogate pair
            uint32_t lo = 0;
            if (!consume("\\u") || !parse_hex4(&lo) || lo < 0xDC00 || lo > 0xDFFF) return err("invalid surrogate pair");
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return err("unpaired surrogate");
          }
          append_utf8(out, cp);
          break;
        }
        default: return err("invalid escape");
      }
    }
    return err("unterminated string");
  }

  Status parse_array(JsonValue* out, int depth) {
    out->type = JsonValue::Type::kArray;
    ++pos_;
    skip_ws();
    if (pos_ < t_.size() && t_[pos_] == ']') { ++pos_; return Status::success(); }
    while (true) {
      JsonValue item;
      Status st = parse_value(&item, depth + 1);
      if (!st.ok()) return st;
      out->array.push_back(std::move(item));
      skip_ws();
      if (pos_ >= t_.size()) return err("unterminated array");
      if (t_[pos_] == ',') { ++pos_; continue; }
      if (t_[pos_] == ']') { ++pos_; return Status::success(); }
      return err("expected ',' or ']'");
    }
  }

  Status parse_object(JsonValue* out, int depth) {
    out->type = JsonValue::Type::kObject;
    ++pos_;
    skip_ws();
    if (pos_ < t_.size() && t_[pos_] == '}') { ++pos_; return Status::success(); }
    while (true) {
      skip_ws();
      if (pos_ >= t_.size() || t_[pos_] != '"') return err("expected string key");
      std::string key;
      Status st = parse_string(&key);
      if (!st.ok()) return st;
      skip_ws();
      if (pos_ >= t_.size() || t_[pos_] != ':') return err("expected ':'");
      ++pos_;
      JsonValue v;
      st = parse_value(&v, depth + 1);
      if (!st.ok()) return st;
      out->keys.push_back(std::move(key));
      out->values.push_back(std::move(v));
      skip_ws();
      if (pos_ >= t_.size()) return err("unterminated object");
      if (t_[pos_] == ',') { ++pos_; continue; }
      if (t_[pos_] == '}') { ++pos_; return Status::success(); }
      return err("expected ',' or '}'");
    }
  }

  const std::string& t_;
  size_t pos_ = 0;
};

}  // namespace

Result<JsonValue> parse_json(const std::string& text) { return Parser(text).parse(); }

std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  return out;
}

}  // namespace minidb
