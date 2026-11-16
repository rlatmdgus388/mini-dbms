#!/bin/sh
# API 서버 기능 테스트: 실제 minidb_server 프로세스를 띄우고 curl 로 호출한다.
# 임시 디렉터리는 build/test_tmp 아래에만 만들고 data/ 는 건드리지 않는다.
BUILD="${1:-build}"
BIN="$BUILD/minidb_server"
command -v curl >/dev/null 2>&1 || { echo "[ SKIP ] server functional: curl not found"; exit 0; }
mkdir -p build/test_tmp
TMP=$(mktemp -d build/test_tmp/srv_XXXXXX) || exit 1
PID=""
cleanup() { [ -n "$PID" ] && kill "$PID" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT
fail=0
check() { if [ "$2" -eq 0 ]; then echo "[  OK  ] server: $1"; else echo "[ FAIL ] server: $1"; fail=1; fi; }

mkdir -p "$TMP/db/users"
cp data/users/schema.json "$TMP/db/users/schema.json"

"$BIN" --data-dir "$TMP/db" --port 0 --threads 4 > "$TMP/log" 2>&1 &
PID=$!
PORT=""
for i in $(seq 1 50); do
  PORT=$(sed -n 's/^listening on [0-9.]*:\([0-9]*\).*/\1/p' "$TMP/log" | head -1)
  [ -n "$PORT" ] && break
  sleep 0.1
done
[ -n "$PORT" ]; check "server starts and reports port" $? || exit 1
URL="http://127.0.0.1:$PORT"

curl -s "$URL/health" | grep -q '"status":"up"'; check "GET /health" $?
out=$(curl -s -X POST "$URL/query" -d "INSERT INTO users (name, age, email) VALUES ('kim', 30, 'k@x.com'), ('lee', 25, 'l@x.com')")
echo "$out" | grep -q '"first_id":1' && echo "$out" | grep -q '"last_id":2'; check "POST /query insert (text/plain)" $?
out=$(curl -s -X POST "$URL/query" -H 'Content-Type: application/json' -d '{"sql":"SELECT name FROM users WHERE id = 2"}')
echo "$out" | grep -q '"plan":"INDEX"' && echo "$out" | grep -q '"lee"'; check "select by id uses INDEX (json body)" $?
curl -s -X POST "$URL/query" -d "SELECT * FROM users WHERE age = 30" | grep -q '"plan":"SCAN"'; check "select by age uses SCAN" $?
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$URL/query" -d "SELEC x")
[ "$code" = "400" ]; check "syntax error -> 400" $?
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$URL/query" -d "SELECT * FROM nope")
[ "$code" = "404" ]; check "unknown table -> 404" $?
code=$(curl -s -o /dev/null -w '%{http_code}' "$URL/query")
[ "$code" = "405" ]; check "GET /query -> 405" $?
code=$(curl -s -o /dev/null -w '%{http_code}' "$URL/other")
[ "$code" = "404" ]; check "unknown path -> 404" $?

# 동시 요청 40개 (병렬 curl): 모든 id 가 유일해야 한다
for i in $(seq 1 40); do
  curl -s -X POST "$URL/query" -d "INSERT INTO users VALUES ('p$i', $i, 'p$i@x')" > "$TMP/r$i" &
done
wait_pids=$(jobs -p | grep -v "^$PID$")
for p in $wait_pids; do wait "$p" 2>/dev/null; done
cat "$TMP"/r* | grep -o '"first_id":[0-9]*' | sort | uniq -d | wc -l | grep -q '^ *0$'; check "40 parallel inserts: no duplicate ids" $?
cnt=$(curl -s -X POST "$URL/query" -d "SELECT id FROM users" | grep -o '"count":[0-9]*')
[ "$cnt" = '"count":42' ]; check "row count after parallel inserts is 42" $?

# SIGINT -> graceful shutdown (exit code 0) 후 재시작해도 데이터 유지
kill -INT "$PID"; wait "$PID"; rc=$?; PID=""
[ $rc -eq 0 ]; check "SIGINT graceful shutdown exits 0" $?
"$BIN" --data-dir "$TMP/db" --port 0 > "$TMP/log2" 2>&1 &
PID=$!
for i in $(seq 1 50); do PORT=$(sed -n 's/^listening on [0-9.]*:\([0-9]*\).*/\1/p' "$TMP/log2" | head -1); [ -n "$PORT" ] && break; sleep 0.1; done
cnt=$(curl -s -X POST "http://127.0.0.1:$PORT/query" -d "SELECT id FROM users WHERE id >= 40" | grep -o '"count":[0-9]*')
[ "$cnt" = '"count":3' ]; check "data and index survive restart" $?
"$BIN" --port 99999 >/dev/null 2>&1; [ $? -eq 2 ]; check "invalid --port rejected" $?
exit $fail
