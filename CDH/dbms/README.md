# mini-dbms API 서버 (CDH)

루트 `src/` 의 DB 엔진(Phase 1 SQL 처리기 + Phase 2 B+ 트리)을 **JSON-RPC 2.0 API** 로 감싸는 서버.
외부 클라이언트가 `curl` 같은 도구로 DB 를 쓸 수 있게 만드는 것이 목표다.

## 로드맵

| 단계         | 상태  | 끝나면 되는 것                                      |
| ---------- | --- | --------------------------------------------- |
| 1. 베이스 서버  | ✅   | `GET /health` 응답. 잘못된 요청에 400/404/405/408/413 |
| 2. SQL API | ✅   | `POST /rpc` 에 JSON-RPC 로 SQL 을 보내면 엔진이 실행하고 결과를 돌려준다 |
| 3. 스레드 풀   | ⏳   | worker N 개가 요청을 병렬 처리 + 락으로 데이터 보호            |
| 4. 마감      | ⏳   | 안전한 종료, 성능 비교                                 |

## API

| HTTP | 경로 | 용도 |
|---|---|---|
| GET | `/health` | 살아 있는지 확인 (모니터링 도구용 단순 HTTP) → `{"ok":true,"status":"up"}` |
| POST | `/rpc` | **JSON-RPC 2.0** 요청 하나 |

### JSON-RPC 메서드

| method | params | result |
|---|---|---|
| `query` | `{"sql":"..."}` 또는 `["..."]`. 여러 문장은 `;` 로 구분 | `{"results":[문장마다 결과 하나]}` |
| `ping` | 없음 | `"pong"` |

```bash
curl -X POST localhost:8080/rpc -d '{"jsonrpc":"2.0","method":"query","params":{"sql":"INSERT INTO users VALUES ('"'"'Kim'"'"', 20);"},"id":1}'
# {"jsonrpc":"2.0","result":{"results":[{"type":"insert","id":1}]},"id":1}

curl -X POST localhost:8080/rpc -d '{"jsonrpc":"2.0","method":"query","params":["SELECT * FROM users WHERE id = 1;"],"id":2}'
# {"jsonrpc":"2.0","result":{"results":[{"type":"select","rows":[[1,"Kim",20]],"columns":["id","name","age"],"row_count":1}]},"id":2}

curl -X POST localhost:8080/rpc -d '{"jsonrpc":"2.0","method":"query","params":["SELEC 1"],"id":3}'
# {"jsonrpc":"2.0","error":{"code":-32001,"message":"Syntax error at position 0: ...","data":{"executed":0}},"id":3}
```

- 응답의 `id` 는 요청의 `id` 를 그대로 돌려준다 (클라이언트가 요청·응답 짝을 맞추는 용도).
- `id` 를 빼면 **알림(notification)**: 실행은 하지만 응답이 없다 (HTTP 204).
- 배치 요청 `[ {...}, {...} ]` 은 지원하지 않는다 (-32600). 여러 SQL 은 한 `sql` 에 `;` 로 이어 쓴다.
- `data.executed` = 오류 전에 이미 실행된 문장 수. **그 문장들은 되돌리지 않는다** (엔진에 트랜잭션이 없다).

### 오류 코드

| code | 의미 | 예 |
|---|---|---|
| -32700 | Parse error: 본문이 JSON 이 아님 | `{oops` |
| -32600 | Invalid Request: 요청 모양이 아님 | `jsonrpc` 가 `"2.0"` 이 아님, `method` 없음, 배치 |
| -32601 | Method not found | `"method":"drop"` |
| -32602 | Invalid params | `params` 없음, `sql` 이 문자열이 아님, SQL 문장이 없음 |
| -32603 | Internal error | 메모리 부족 |
| -32001 | SQL 오류 (서버 정의) | 문법·컬럼·타입·값 개수 오류 |
| -32002 | 테이블 없음 (서버 정의) | `SELECT * FROM nope` |
| -32003 | 저장소 오류 (서버 정의) | 스키마·데이터 파일이 깨짐 |

-32768 ~ -32000 은 규격 예약 영역이고, 그중 -32000 ~ -32099 는 "서버가 정의해서 쓰라" 고 비워 둔 곳이라 SQL 오류를 여기에 배정했다.

---

## 1. 사용한 기술

| 기술 | 어디에 | 왜 |
|---|---|---|
| **C11** | 서버 전체 | 엔진(`src/`)이 C 라서 그대로 링크해 쓴다 |
| **BSD 소켓 API** (`socket`, `bind`, `listen`, `accept`, `recv`, `send`) | `main.c`, `http.c` | 라이브러리 없이 TCP 서버를 직접 만든다 |
| **HTTP/1.1** (직접 파싱) | `http.c` | 요청 줄 + 헤더 + `Content-Length` 본문만 지원하는 최소 구현. JSON-RPC 를 실어 나르는 통로 |
| **JSON-RPC 2.0** | `rpc.c` | "메서드 이름 + 파라미터" 로 부르는 표준 RPC 규격. 요청·응답·오류 모양이 정해져 있어 클라이언트를 만들기 쉽다 |
| **JSON 직접 읽기·쓰기** (재귀 하강, `\uXXXX`→UTF-8, 이스케이프) | `json.c` | 외부 라이브러리 없이 필요한 만큼만 |
| **루트 엔진 API** (`tokenize_next`, `parse`, `execute`, 행 콜백) | `query.c` | Phase 1·2 코드를 고치지 않고 그대로 부른다 |
| **가변 버퍼** (`realloc` 두 배 증가) | `json.c` | SELECT 결과 크기를 미리 모르므로 |
| **POSIX 시그널** (`sigaction`, `SIGPIPE`, `SIGINT`) | `main.c` | 끊긴 소켓에 써도 안 죽기, Ctrl+C 로 정상 종료 |
| **소켓 타임아웃** (`SO_RCVTIMEO`) | `main.c` | 요청을 끝까지 안 보내는 클라이언트가 서버를 붙잡지 못하게 |
| **Make** | `Makefile` | 빌드, 테스트 실행 |
| **Docker** (`gcc:13`) | 빌드 환경 | Windows 에서도 Linux 소켓 API 로 빌드·실행 |
| **assert / bash + curl** | `tests/` | 단위 테스트 / 실제 서버를 띄운 기능 테스트 |

---

## 2. 어떻게 만들었나 (요청 하나의 흐름)

```
 클라이언트(curl)                          서버
 ───────────────                          ─────────────────────────────────────────
                              ┌─ main: db_open → open_listener             (시작할 때 한 번)
                              │
 연결 요청  ───────────────▶   ├─ accept            새 연결 fd 를 받는다
 POST /rpc                    ├─ handle_client                                [main.c]
 {"jsonrpc":"2.0",            │    ├─ http_read_request   recv 반복으로 요청을 다 모은다   [http.c]
  "method":"query",           │    │     └─ http_parse    요청 줄·헤더·본문 길이를 해석
  "params":{"sql":...},       │    ├─ route → handle_rpc
  "id":1}                     │    │     └─ rpc_handle                                    [rpc.c]
                              │    │          ├─ jr_skip_value   본문이 JSON 인가? (-32700)  [json.c]
                              │    │          ├─ parse_request   jsonrpc/method/params/id (-32600)
                              │    │          └─ call_method     method 이름으로 분기 (-32601)
                              │    │               └─ query_run                           [query.c]
                              │    │                    ├─ tokenize_next → parse → execute [엔진 src/]
                              │    │                    │       └─ on_row  행마다 JSON 으로
                              │    │                    └─ {"results":[...]}
                              │    │          └─ make_result / make_error  {"jsonrpc","result|error","id"}
                              │    └─ http_send_json      HTTP 200 + JSON-RPC 응답       [http.c]
 ◀───────────────  응답        │
                              └─ close(fd)  →  다시 accept 로 (반복)
```

**계층** (위가 아래를 부른다. 아래는 위를 모른다)

```
main.c  (HTTP 라우팅)        ← 소켓·HTTP 만 안다
  └ rpc.c  (JSON-RPC)        ← HTTP 를 모른다. JSON 본문 → JSON 응답
      └ query.c  (SQL 실행)   ← JSON-RPC 를 모른다. SQL → 결과 JSON
          └ src/  (엔진)      ← 서버를 모른다
http.c / json.c              ← 도구 (HTTP 읽기·쓰기 / JSON 읽기·쓰기)
```

**핵심 설계 포인트**

1. **반복 서버(iterative server)**: 연결을 하나씩 차례로 처리한다. 3단계에서 스레드 풀로 바꾼다.
2. **TCP 는 바이트 스트림**: `recv` 한 번에 요청이 다 온다는 보장이 없다. 받은 바이트를 쌓으면서 "다 왔나?" 를 `http_parse` 로 반복 확인한다.
3. **계층 분리**: 각 계층이 아래 계층만 알아서, 소켓 없이 문자열만으로 단위 테스트할 수 있다.
4. **JSON-RPC 오류는 HTTP 200**: 성공·실패는 본문의 `result` / `error` 로 구분한다. HTTP 상태는 "HTTP 수준" 문제(경로·메서드·크기)에만 쓴다.
5. **JSON 을 두 번 훑는다**: 먼저 `jr_skip_value` 로 본문 전체가 올바른 JSON 인지 검사하고(-32700), 그다음 요청 모양을 본다(-32600). 규격이 두 오류를 구분하기 때문이다.
6. **id 는 원문 그대로 되돌린다**: 숫자든 문자열이든 해석하지 않고 본문 안의 위치만 기억했다가 응답에 복사한다.
7. **엔진은 그대로**: 엔진의 `RowCallback` 자리(CLI 는 `printf`)에 JSON 을 만드는 `on_row` 를 끼워 넣었다.
8. **INSERT 는 응답 전에 디스크로** (`fflush`): 서버가 갑자기 죽어도 "성공 응답 받은 INSERT" 는 파일에 남는다.
9. **상한을 둔다**: 요청 64KB, JSON 중첩 32 단계.

---

## 3. 파일별 기능

```
CDH/dbms/
├── Makefile           빌드·테스트 명령. 엔진은 ../../src, 데이터는 ../../data
├── server/
│   ├── main.c         서버 본체: DB 열기, 리스닝 소켓, accept 반복, HTTP 라우팅, 시그널
│   ├── http.h / .c    HTTP 요청 읽기·파싱, 응답 보내기
│   ├── rpc.h / .c     JSON-RPC 2.0: 요청 해석, 메서드 분기, 응답·오류 만들기, 오류 코드 정의
│   ├── query.h / .c   SQL 실행기: SQL → 엔진 실행 → 결과 JSON
│   └── json.h / .c    JSON 도구: 가변 버퍼에 쓰기, 문자열 읽기, 값 건너뛰기(문법 검사)
└── tests/
    ├── test_http.c    http_parse 단위 테스트
    ├── test_query.c   query_run 단위 테스트 (임시 폴더에 테이블을 만들어서)
    ├── test_rpc.c     JSON 읽기 + rpc_handle 단위 테스트 (오류 코드 전부, 알림)
    └── e2e.sh         서버를 실제로 띄우고 curl 로 확인
```

---

## 4. 핵심 함수

### `server/main.c` — 서버 본체

| 함수 | 하는 일 |
|---|---|
| `main` | 프로그램 시작점. ① 인자 읽기 (포트 기본 8080, 데이터 폴더 기본 `data`) → `db_open` 으로 DB 준비 ② `SIGPIPE` 무시, `SIGINT/SIGTERM` 은 `on_signal` 로 연결 ③ `open_listener` 로 리스닝 소켓 열기 (실패하면 종료 코드 1) ④ `g_stop` 이 설 때까지 `accept` → `handle_client` 반복 ⑤ 소켓과 DB 닫고 종료 코드 0 |
| `on_signal(sig)` | Ctrl+C 를 받으면 `g_stop = 1` 만 한다 (시그널 핸들러 안에서는 플래그만 바꾸는 것이 안전) |
| `open_listener(port)` | `socket` → `SO_REUSEADDR` (재시작 시 포트 충돌 방지) → `bind` → `listen`. 실패하면 -1 |
| `handle_client(fd)` | 연결 하나 처리: recv 타임아웃 설정 → 요청 읽기 → 성공이면 `route`, 실패면 오류 응답 → 로그 → `close` |
| `route(fd, req)` | 경로·메서드로 핸들러 선택. 경로 없음 404, 메서드 틀림 405 |
| `handle_health(fd)` | `GET /health` → `{"ok":true,"status":"up"}` |
| `handle_rpc(fd, req)` | `POST /rpc`. 본문을 `rpc_handle` 에 넘기고 응답을 HTTP 200 으로 보낸다. 알림이면 204, 메모리 부족이면 500 |

### `server/http.c` — HTTP 처리

| 함수 | 하는 일 |
|---|---|
| `http_parse(buf, len, req)` | 받은 바이트를 요청으로 해석. **0** = 완성, **1** = 아직 덜 옴(더 받아라), **4xx** = 잘못된 요청. 빈 줄(`\r\n\r\n`)로 헤더 끝을 찾고, 요청 줄을 `sscanf` 로 나누고, 본문이 `Content-Length` 만큼 왔는지 본다 |
| `find_content_length(...)` | 헤더 줄들에서 `Content-Length` 를 대소문자 무시로 찾아 숫자로. 숫자가 아니면 400, 상한을 넘으면 413 |
| `http_read_request(fd, buf, req)` | `recv` 를 반복해 버퍼에 쌓고 매번 `http_parse` 로 확인. 시간 초과면 408, 아무것도 못 받고 끊기면 -1 |
| `send_all(fd, data, len)` | `send` 는 일부만 보낼 수 있으므로 다 나갈 때까지 반복. `MSG_NOSIGNAL` 로 끊긴 소켓에서도 안 죽는다 |
| `http_send_json(fd, status, json)` | 상태 줄 + `Content-Type` + `Content-Length` + `Connection: close` + 본문을 보낸다 |
| `http_send_error(fd, status, msg)` | `{"ok":false,"error":"..."}` 모양의 HTTP 수준 오류 응답 (404/405/413 등) |
| `http_reason(status)` | 200 → `"OK"`, 404 → `"Not Found"` 같은 표준 문구 |

### `server/rpc.c` — JSON-RPC 2.0

| 함수 | 하는 일 |
|---|---|
| `rpc_handle(db, body, len, &notification)` | **진입점.** ① 본문 전체가 JSON 인지 검사 (아니면 -32700) ② 배치(`[`)면 -32600 ③ `parse_request` 로 요청 해석 (틀리면 -32600) ④ `call_method` 로 실행 ⑤ `id` 가 없던 알림이면 응답을 버리고 `notification = 1` |
| `parse_request(r, req)` | 요청 객체를 훑어 `jsonrpc`·`method` 는 문자열로 꺼내고, `params`·`id` 는 본문 안 위치만 기억한다. 모르는 키는 건너뛴다. `jsonrpc` 가 `"2.0"` 이 아니거나 `method` 가 없거나 `id` 가 객체·배열·true/false 면 -32600 |
| `call_method(db, req)` | 메서드 이름으로 분기. `ping` → `"pong"`, `query` → `get_sql_param` + `query_run`, 그 밖 → -32601 |
| `get_sql_param(req, &sql)` | `params` 에서 SQL 꺼내기. `{"sql":"..."}` 과 `["..."]` 둘 다 받는다. 없거나 문자열이 아니면 -1 (→ -32602) |
| `make_result(req, result_json)` | `{"jsonrpc":"2.0","result":...,"id":...}` 를 만든다 |
| `make_error(id, code, msg, executed)` | `{"jsonrpc":"2.0","error":{"code","message","data"},"id":...}` 를 만든다. SQL 오류면 `data.executed` 를 붙인다 |
| `add_id_and_close(b, id)` | 요청의 `id` 원문을 그대로 붙인다. 모르면 `null` |

### `server/query.c` — SQL 실행기 (서버 ↔ 엔진 연결)

| 함수 | 하는 일 |
|---|---|
| `query_run(db, sql, &err)` | **진입점.** `tokenize_next` 로 문장을 하나씩 잘라 `run_statement` 로 실행하고 `{"results":[...]}` 로 모은다. 오류가 나면 거기서 멈추고 NULL + `err` (오류 코드·메시지·실행된 문장 수). 문장이 하나도 없으면 -32602 |
| `run_statement(...)` | 문장 하나: `parse` → `execute` → `statement_free`. INSERT 면 `fflush` 후 `{"type":"insert","id":N}`, SELECT 면 `rows`·`columns`·`row_count` 객체를 붙인다 |
| `on_row(fields, count, ctx)` | 엔진이 SELECT 로 찾은 행마다 불러 주는 콜백. 행 하나를 `[1,"Kim",20]` 으로 붙인다 |
| `add_columns(b, db, stmt)` | 컬럼 이름 목록. `SELECT *` 면 스키마 순서, 아니면 SQL 에 쓴 순서 |
| `rpc_code_of(st)` | 엔진 오류 코드 → JSON-RPC 오류 코드 (`NO_TABLE`→-32002, `SCHEMA`/`IO`→-32003, 나머지→-32001) |
| `fail(err, code, msg, executed)` | 실패 정보를 채우고 NULL 을 돌려준다 |

### `server/json.c` — JSON 도구

| 함수 | 하는 일 |
|---|---|
| `jb_add` / `jb_str` / `jb_int` | 가변 버퍼에 바이트·문자열·정수를 붙인다. 자리가 모자라면 `realloc` 으로 두 배씩 |
| `jb_json_str(b, s)` | 문자열을 JSON 문자열로. `"` `\` 앞에 `\`, 줄바꿈·제어 문자는 `\n`·`\u00XX` 로 |
| `jb_finish(b)` | 다 쓴 버퍼를 돌려준다. 도중에 메모리가 모자랐으면 NULL |
| `jr_skip_ws` / `jr_eat(r, c)` | 공백 건너뛰기 / 다음 글자가 `c` 면 하나 넘기기 |
| `jr_read_string(r, &out)` | `"..."` 를 읽어 이스케이프를 푼다. `\uXXXX` 는 UTF-8 로, 이모지(서로게이트 쌍)는 하나로 합친다. `\u0000` 은 거절 |
| `jr_skip_value(r)` | 값 하나(객체·배열·문자열·숫자·true/false/null)를 건너뛰면서 문법을 검사한다. 중첩 32 단계 넘으면 오류 |
| `skip_number` / `skip_word` / `add_utf8` / `read_hex4` | 위 함수들의 부품: 숫자 문법, 낱말, 코드 포인트→UTF-8, 16진수 4글자 |

### HTTP 상태 코드 정리

JSON-RPC 의 성공·실패는 본문으로 구분하므로, HTTP 상태는 "HTTP 수준" 문제에만 쓴다.

| 코드 | 언제 |
|---|---|
| 200 | 정상 (JSON-RPC 오류 응답도 200) |
| 204 | JSON-RPC 알림 (응답 없음) |
| 400 | HTTP 요청 형식이 틀림 |
| 404 | 없는 경로 |
| 405 | 경로는 있는데 메서드가 틀림 (예: `GET /rpc`) |
| 408 | 5초 안에 요청을 다 안 보냄 |
| 413 | 요청이 64KB 를 넘음 |
| 500 | 메모리 부족으로 응답을 못 만듦 |

---

## 5. 실행 (Docker 안에서)

저장소 루트에서 dev 컨테이너에 들어간 뒤:

```bash
cd CDH/dbms
make && make test && make e2e
make run                       # 데이터: 저장소 루트의 data/ (users 테이블)
```

다른 터미널에서:

```bash
curl localhost:8080/health
curl -X POST localhost:8080/rpc -d '{"jsonrpc":"2.0","method":"query","params":["SELECT * FROM users;"],"id":1}'
```
