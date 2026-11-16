#!/bin/sh
# CLI 기능 테스트: 실제 minidb 바이너리를 -e / --file / stdin 으로 실행한다.
# 임시 디렉터리는 build/test_tmp 아래에만 만들고 data/ 는 건드리지 않는다.
BUILD="${1:-build}"
BIN="$BUILD/minidb"
mkdir -p build/test_tmp
TMP=$(mktemp -d build/test_tmp/cli_XXXXXX) || exit 1
trap 'rm -rf "$TMP"' EXIT
fail=0
check() { # name, condition-exit-code
  if [ "$2" -eq 0 ]; then echo "[  OK  ] cli: $1"; else echo "[ FAIL ] cli: $1"; fail=1; fi
}

mkdir -p "$TMP/db/users"
cp data/users/schema.json "$TMP/db/users/schema.json"
D="$TMP/db"

# 1) -e INSERT 후 SELECT
out=$("$BIN" --data-dir "$D" -e "INSERT INTO users (name, age, email) VALUES ('kim', 30, 'k@x.com'), ('lee', 25, 'l@x.com');")
echo "$out" | grep -q "2 row(s) inserted (id 1..2)"; check "insert via -e" $?
out=$("$BIN" --data-dir "$D" -e "SELECT name, age FROM users WHERE age > 26;")
echo "$out" | grep -q "kim" && ! echo "$out" | grep -q "lee"; check "select with where" $?

# 2) 텍스트 파일(--file): 여러 문장 + 주석
cat > "$TMP/q.sql" <<'SQL'
-- 파일로 전달된 SQL
INSERT INTO users (name, age, email) VALUES ('park', 41, 'p@x.com');
SELECT * FROM users;
SQL
out=$("$BIN" --data-dir "$D" --file "$TMP/q.sql")
echo "$out" | grep -q "park" && echo "$out" | grep -q "(3 rows"; check "--file with multiple statements" $?

# 3) stdin
out=$(echo "SELECT COUNT_PLACEHOLDER FROM users;" | "$BIN" --data-dir "$D" 2>&1); rc=$?
[ $rc -ne 0 ]; check "stdin syntax-ish error gives non-zero exit" $?
out=$(echo "SELECT id FROM users LIMIT 1;" | "$BIN" --data-dir "$D")
echo "$out" | grep -q "(1 row"; check "stdin input" $?

# 4) 에러는 non-zero exit + stderr
"$BIN" --data-dir "$D" -e "SELECT * FROM missing;" 2>"$TMP/err" >/dev/null; rc=$?
[ $rc -eq 1 ] && grep -q "does not exist" "$TMP/err"; check "missing table error" $?
"$BIN" --data-dir "$D" -e "INSERT INTO users (id, name, age, email) VALUES (1,'a',1,'b');" 2>"$TMP/err" >/dev/null; rc=$?
[ $rc -eq 1 ] && grep -q "auto-assigned" "$TMP/err"; check "explicit id rejected" $?
"$BIN" --data-dir "$D/nope" -e "SELECT 1" >/dev/null 2>&1; [ $? -eq 1 ]; check "missing data dir" $?
"$BIN" --data-dir "$D" --file "$TMP/none.sql" >/dev/null 2>&1; [ $? -eq 2 ]; check "missing sql file" $?
"$BIN" --bogus >/dev/null 2>&1; [ $? -eq 2 ]; check "unknown flag" $?

# 5) 재시작 후 데이터와 id 유지
out=$("$BIN" --data-dir "$D" -e "INSERT INTO users (name, age, email) VALUES ('new', 1, 'n@x');")
echo "$out" | grep -q "id 4..4"; check "ids continue after restart" $?

exit $fail
