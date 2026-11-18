#!/usr/bin/env bash
# 서버를 실제로 띄우고 curl 로 확인하는 기능 테스트.   사용법: make e2e
set -u

PORT=18080
URL="http://127.0.0.1:$PORT"
FAIL=0

# 테스트 전용 데이터 폴더 (진짜 data/ 를 건드리지 않는다)
DATA=$(mktemp -d /tmp/dbms-e2e-XXXXXX)
printf 'id,INT\nname,TEXT\nage,INT\n' > "$DATA/users.schema"

# [역할] 실제 값과 기대 값을 비교해 PASS/FAIL 을 찍는다.
check() {
    if [[ "$2" == "$3" ]]; then echo "  PASS $1"; else echo "  FAIL $1: got [$2], expected [$3]"; FAIL=1; fi
}

# [역할] 상태 코드만 가져온다.
status() { curl -s -o /dev/null -w '%{http_code}' --max-time 10 "$@"; }

# [역할] SQL 을 POST /query 로 보내고 응답 본문을 출력한다.
query() { curl -s --max-time 10 -X POST --data-binary "$1" "$URL/query"; }

# [역할] curl 이 못 만드는 날것의 요청을 TCP 로 보내고 상태 코드만 뽑는다.
raw_status() {
    exec 3<>"/dev/tcp/127.0.0.1/$PORT"
    printf '%b' "$1" >&3
    timeout 10 cat <&3 | head -1 | awk '{print $2}'
    exec 3<&-
}

./build/dbms-server $PORT "$DATA" > /tmp/dbms-e2e.log 2>&1 &
PID=$!
sleep 0.3

echo "[1단계] 베이스 서버"
check "GET /health -> 200"        "$(status "$URL/health")" 200
check "body has status up"         "$(curl -s "$URL/health")" '{"ok":true,"status":"up"}'
check "unknown path -> 404"        "$(status "$URL/nope")" 404
check "POST /health -> 405"        "$(status -X POST "$URL/health")" 405
check "garbage -> 400"             "$(raw_status 'HELLO\r\n\r\n')" 400
check "huge Content-Length -> 413" "$(raw_status 'POST /health HTTP/1.1\r\nContent-Length: 99999999\r\n\r\n')" 413

# 응답을 안 읽고 끊는 클라이언트가 있어도 서버는 살아 있어야 한다 (SIGPIPE)
(exec 3<>"/dev/tcp/127.0.0.1/$PORT"; printf 'GET /health HTTP/1.1\r\n\r\n' >&3; exec 3<&-)
check "alive after abrupt client"  "$(status "$URL/health")" 200

echo "[2단계] SQL API"
check "INSERT"                     "$(query "INSERT INTO users VALUES ('Kim', 20);")" \
      '{"ok":true,"results":[{"type":"insert","id":1}]}'
query "INSERT INTO users VALUES ('Lee', 30);" > /dev/null
check "SELECT by id (index)"       "$(query "SELECT * FROM users WHERE id = 2;")" \
      '{"ok":true,"results":[{"type":"select","rows":[[2,"Lee",30]],"columns":["id","name","age"],"row_count":1}]}'
check "data written to file"       "$(cat "$DATA/users.csv" | wc -l)" 2
check "syntax error -> 400"        "$(status -X POST --data-binary 'SELEC 1' "$URL/query")" 400
check "unknown table -> 404"       "$(status -X POST --data-binary 'SELECT * FROM nope;' "$URL/query")" 404
check "empty body -> 400"          "$(status -X POST "$URL/query")" 400
check "GET /query -> 405"          "$(status "$URL/query")" 405

kill -INT $PID
wait $PID
check "Ctrl+C -> exit 0"           "$?" 0

rm -rf "$DATA"
[[ $FAIL -eq 0 ]] && echo "e2e: all passed"
exit $FAIL
