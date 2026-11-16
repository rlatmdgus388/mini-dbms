#include "storage/record.h"

#include <cstring>

namespace minidb {

void encode_row(const Schema& schema, const Row& row, std::string* out) {
  for (size_t i = 0; i < schema.columns.size(); ++i) {
    switch (schema.columns[i].type) {
      case ColumnType::kInt:
      case ColumnType::kBigInt: {
        int64_t v = std::get<int64_t>(row[i]);
        out->append(reinterpret_cast<const char*>(&v), sizeof(v));
        break;
      }
      case ColumnType::kDouble: {
        double v = std::get<double>(row[i]);
        out->append(reinterpret_cast<const char*>(&v), sizeof(v));
        break;
      }
      case ColumnType::kVarchar: {
        const std::string& s = std::get<std::string>(row[i]);
        uint32_t n = static_cast<uint32_t>(s.size());
        out->append(reinterpret_cast<const char*>(&n), sizeof(n));
        out->append(s);
        break;
      }
    }
  }
}

Result<Row> decode_row(const Schema& schema, const char* data, size_t len) {
  Row row;
  row.reserve(schema.columns.size());
  size_t pos = 0;
  auto corrupt = [] { return Status::fail(ErrorCode::kIo, "corrupt record"); };
  for (const Column& col : schema.columns) {
    switch (col.type) {
      case ColumnType::kInt:
      case ColumnType::kBigInt: {
        int64_t v;
        if (pos + sizeof(v) > len) return corrupt();
        std::memcpy(&v, data + pos, sizeof(v));
        pos += sizeof(v);
        row.emplace_back(v);
        break;
      }
      case ColumnType::kDouble: {
        double v;
        if (pos + sizeof(v) > len) return corrupt();
        std::memcpy(&v, data + pos, sizeof(v));
        pos += sizeof(v);
        row.emplace_back(v);
        break;
      }
      case ColumnType::kVarchar: {
        uint32_t n;
        if (pos + sizeof(n) > len) return corrupt();
        std::memcpy(&n, data + pos, sizeof(n));
        pos += sizeof(n);
        if (n > col.max_len || pos + n > len) return corrupt();
        row.emplace_back(std::string(data + pos, n));
        pos += n;
        break;
      }
    }
  }
  if (pos != len) return corrupt();
  return row;
}

bool peek_id(const char* data, size_t len, int64_t* id) {
  if (len < sizeof(int64_t)) return false;
  std::memcpy(id, data, sizeof(int64_t));
  return true;
}

}  // namespace minidb
