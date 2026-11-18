# mini-dbms API 서버 (CDH)

루트 `src/` 의 DB 엔진(Phase 1 SQL 처리기 + Phase 2 B+ 트리)을 **JSON-RPC 2.0 API** 로 감싸는 서버.
외부 클라이언트가 `curl` 같은 도구로 DB 를 쓸 수 있게 만드는 것이 목표다.

## 로드맵

| 단계         | 상태  | 끝나면 되는 것                                      |
| ---------- | --- | --------------------------------------------- |
| 1. 베이스 서버  | ✅   | `GET /health` 응답. 잘못된 요청에 400/404/405/408/413 |
| 2. SQL API | ✅   | `POST /rpc` 에 JSON-RPC 로 SQL 을 보내면 엔진이 실행하고 결과를 돌려준다 |
| 3. 스레드 풀   | ✅   | 작업자 N 개가 요청을 병렬 처리 + 테이블마다 rwlock 으로 데이터 보호  |
| 4. 마감      | ✅   | 안전한 종료(진행 중 요청 끝까지, 두 번째 Ctrl+C 강제), 스레드 수별 성능 비교 |

## API

| HTTP | 경로        | 용도                                                         |
| ---- | --------- | ---------------------------------------------------------- |
| GET  | `/health` | 살아 있는지 확인 (모니터링 도구용 단순 HTTP) → `{"ok":true,"status":"up"}` |
| POST | `/rpc`    | **JSON-RPC 2.0** 요청 하나                                     |

### JSON-RPC 메서드

| method  | params                                        | result                     |
| ------- | --------------------------------------------- | -------------------------- |
| `query` | `{"sql":"..."}` 또는 `["..."]`. 여러 문장은 `;` 로 구분 | `{"results":[문장마다 결과 하나]}` |
| `ping`  | 없음                                            | `"pong"`                   |

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
| **POSIX 시그널** (`sigaction`, `SIGPIPE`, `SIGINT`, `pthread_sigmask`) | `main.c` | 끊긴 소켓에 써도 안 죽기, Ctrl+C 로 정상 종료. 시그널은 main 스레드만 받게 한다 |
| **pthread 스레드 풀** (`pthread_create/join`, `mutex`, `cond`) | `pool.c` | 작업자 N 개가 연결 큐에서 일을 꺼내 동시에 처리 |
| **pthread rwlock** (쓰기 우선) | `query.c` | 테이블마다: SELECT 는 함께, INSERT 는 혼자 |
| **ThreadSanitizer** (`-fsanitize=thread`) | 검증 | 데이터 경합(data race)이 없는지 확인 |
| **소켓 타임아웃** (`SO_RCVTIMEO`) | `main.c` | 요청을 끝까지 안 보내는 클라이언트가 서버를 붙잡지 못하게 |
| **Make** | `Makefile` | 빌드, 테스트 실행 |
| **Docker** (`gcc:13`) | 빌드 환경 | Windows 에서도 Linux 소켓 API 로 빌드·실행 |
| **assert / bash + curl** | `tests/` | 단위 테스트 / 실제 서버를 띄운 기능 테스트 |

---

## 2. 어떻게 만들었나 (요청 하나의 흐름)

```
 클라이언트(curl)                          서버
 ───────────────                          ─────────────────────────────────────────
                              ┌─ main: db_open → open_listener → pool_start(N)   (시작할 때 한 번)
                              │
 연결 요청  ───────────────▶   ├─ [main 스레드] accept → pool_submit(fd)   큐에 넣고 바로 다음 accept
                              │                         (큐가 꽉 차면 503)      [pool.c]
                              │        ▼  큐: fd fd fd ...  (mutex + cond)
 POST /rpc                    ├─ [작업자 N 개 중 하나] worker → handle_client   [main.c]
 {"jsonrpc":"2.0",            │    ├─ http_read_request   recv 반복으로 요청을 다 모은다   [http.c]
  "method":"query",           │    │     └─ http_parse    요청 줄·헤더·본문 길이를 해석
  "params":{"sql":...},       │    ├─ route → handle_rpc
  "id":1}                     │    │     └─ rpc_handle                                    [rpc.c]
                              │    │          ├─ jr_skip_value   본문이 JSON 인가? (-32700)  [json.c]
                              │    │          ├─ parse_request   jsonrpc/method/params/id (-32600)
                              │    │          └─ call_method     method 이름으로 분기 (-32601)
                              │    │               └─ query_run                           [query.c]
                              │    │                    ├─ tokenize_next → parse          (락 없음)
                              │    │                    ├─ lock_for   테이블 rwlock (SELECT=읽기, INSERT=쓰기)
                              │    │                    ├─ execute                         [엔진 src/]
                              │    │                    │       └─ on_row  행마다 JSON 으로
                              │    │                    ├─ unlock_for
                              │    │                    └─ {"results":[...]}
                              │    │          └─ make_result / make_error  {"jsonrpc","result|error","id"}
                              │    └─ http_send_json      HTTP 200 + JSON-RPC 응답       [http.c]
 ◀───────────────  응답        │
                              └─ close(fd)  →  작업자는 다시 큐에서 다음 연결을 꺼낸다
```

**계층** (위가 아래를 부른다. 아래는 위를 모른다)

```
main.c  (HTTP 라우팅)        ← 소켓·HTTP 만 안다
  └ rpc.c  (JSON-RPC)        ← HTTP 를 모른다. JSON 본문 → JSON 응답
      └ query.c  (SQL 실행)   ← JSON-RPC 를 모른다. SQL → 결과 JSON
          └ src/  (엔진)      ← 서버를 모른다
http.c / json.c / pool.c     ← 도구 (HTTP 읽기·쓰기 / JSON 읽기·쓰기 / 스레드 풀)
```

**핵심 설계 포인트**

1. **스레드 풀 (생산자-소비자)**: main 스레드는 `accept` 만 하고 연결을 큐에 넣는다(생산자). 작업자 N 개가 큐에서 꺼내 처리한다(소비자). 느린 손님 하나가 작업자 하나를 붙잡아도 나머지가 계속 일한다. 스레드를 요청마다 만들지 않고 미리 만들어 두므로 생성 비용도 없다.
2. **TCP 는 바이트 스트림**: `recv` 한 번에 요청이 다 온다는 보장이 없다. 받은 바이트를 쌓으면서 "다 왔나?" 를 `http_parse` 로 반복 확인한다.
3. **계층 분리**: 각 계층이 아래 계층만 알아서, 소켓 없이 문자열만으로 단위 테스트할 수 있다.
4. **JSON-RPC 오류는 HTTP 200**: 성공·실패는 본문의 `result` / `error` 로 구분한다. HTTP 상태는 "HTTP 수준" 문제(경로·메서드·크기)에만 쓴다.
5. **JSON 을 두 번 훑는다**: 먼저 `jr_skip_value` 로 본문 전체가 올바른 JSON 인지 검사하고(-32700), 그다음 요청 모양을 본다(-32600). 규격이 두 오류를 구분하기 때문이다.
6. **id 는 원문 그대로 되돌린다**: 숫자든 문자열이든 해석하지 않고 본문 안의 위치만 기억했다가 응답에 복사한다.
7. **엔진은 그대로**: 엔진의 `RowCallback` 자리(CLI 는 `printf`)에 JSON 을 만드는 `on_row` 를 끼워 넣었다.
8. **INSERT 는 응답 전에 디스크로** (`fflush`): 서버가 갑자기 죽어도 "성공 응답 받은 INSERT" 는 파일에 남는다.
9. **상한을 둔다**: 요청 64KB, JSON 중첩 32 단계, 대기 큐 64 개 (넘치면 503).
10. **락은 엔진 밖에서**: 엔진(`src/`)은 스레드를 모르므로 고치지 않고, 엔진을 부르는 `query.c` 가 락을 잡는다.
11. **테이블마다 rwlock**: SELECT 는 읽기 락이라 같은 테이블도 여럿이 동시에 읽는다. INSERT 는 쓰기 락이라 `next_id`·B+ 트리·파일 쓰기를 혼자 한다 → id 가 겹치거나 행이 사라지지 않는다.
12. **테이블 목록 락 (`g_catalog`)**: 엔진은 테이블을 처음 쓸 때 목록(`Db.tables`)에 등록한다. 목록을 바꾸는 이 순간만 쓰기 락, 평소엔 읽기 락.
13. **락 순서는 하나**: 항상 `g_catalog` → 테이블 락. 순서가 하나라 교착(deadlock)이 없다. 락은 문장 하나 동안만 잡는다 (트랜잭션 아님).
14. **쓰기 우선 rwlock**: 기본 rwlock 은 SELECT 가 끊이지 않으면 INSERT 가 영원히 못 들어간다(기아). `PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP` 로 막는다.
15. **시그널은 main 만**: 작업자를 만들기 전에 `SIGINT/SIGTERM` 을 막고, main 에서만 푼다. 작업자가 Ctrl+C 를 받아 버리면 main 의 `accept` 가 안 깨어나기 때문이다.
16. **우아한 종료(graceful shutdown)**: Ctrl+C → `accept` 중단·리스닝 소켓 닫기(새 연결은 거절) → `pool_stop` 이 처리 중·대기 중인 요청까지 끝내고 작업자를 `join` → `db_close` 로 파일을 디스크로 내림 → 종료 코드 0. 요청을 덜 보낸 손님이 있어도 `TIMEOUT_SEC`(5초) 뒤 408 로 끝나므로 종료가 영원히 걸리지 않는다.
17. **두 번째 Ctrl+C 는 강제 종료**: 정리를 기다리기 싫으면 한 번 더 누른다. 핸들러가 `_exit(130)` 을 부른다 (`_exit` 는 시그널 핸들러에서 불러도 안전한 함수). INSERT 는 응답 전에 이미 `fflush` 했으므로 성공 응답을 받은 데이터는 남는다.

**다른 설계안: 테이블 담당 스레드** (검토했지만 안 씀)

테이블마다 그 테이블만 만지는 스레드를 정해 두면 테이블 데이터에는 락이 필요 없다. 하지만
- 같은 테이블의 SELECT 도 줄을 선다. 테이블이 하나(users)면 사실상 반복 서버다.
- 락이 없어지는 게 아니라 "담당 스레드의 작업 큐" 로 옮겨 가고, 결과를 돌려받는 장치(대기·깨우기)가 더 필요하다.
- 테이블을 알려면 SQL 을 먼저 파싱해야 하고, 한 요청에 여러 문장이 있으면 문장마다 다른 스레드로 보내야 한다.

그래서 읽기가 많은 상황에 맞고 코드가 짧은 테이블별 rwlock 을 골랐다.

---

## 3. 파일별 기능

```
CDH/dbms/
├── Makefile           빌드·테스트 명령. 엔진은 ../../src, 데이터는 ../../data
├── server/
│   ├── main.c         서버 본체: DB 열기, 리스닝 소켓, accept 반복, HTTP 라우팅, 시그널
│   ├── pool.h / .c    스레드 풀: 연결 큐(mutex + cond) + 작업자 스레드 N 개
│   ├── http.h / .c    HTTP 요청 읽기·파싱, 응답 보내기
│   ├── rpc.h / .c     JSON-RPC 2.0: 요청 해석, 메서드 분기, 응답·오류 만들기, 오류 코드 정의
│   ├── query.h / .c   SQL 실행기: SQL → (테이블 락) → 엔진 실행 → 결과 JSON
│   └── json.h / .c    JSON 도구: 가변 버퍼에 쓰기, 문자열 읽기, 값 건너뛰기(문법 검사)
└── tests/
    ├── test_http.c    http_parse 단위 테스트
    ├── test_query.c   query_run 단위 테스트 (임시 폴더에 테이블을 만들어서) + 스레드 8 개 동시 실행
    ├── test_rpc.c     JSON 읽기 + rpc_handle 단위 테스트 (오류 코드 전부, 알림)
    ├── e2e.sh         서버를 실제로 띄우고 curl 로 확인 (느린 손님, 동시 INSERT 40 개, 종료 시나리오)
    ├── bench.c        부하 생성기: 클라이언트 스레드 N 개가 같은 SQL 을 동시에 보내고 처리량·지연을 잰다
    └── bench.sh       작업자 스레드 1/2/4/8 개로 서버를 바꿔 띄우며 bench 를 돌린다 (make bench)
```

---

## 4. 핵심 함수

### `server/main.c` — 서버 본체

| 함수                    | 하는 일                                                                                                                                                                                                                                        |
| --------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `main`                | 프로그램 시작점. ① 인자 읽기 (포트 8080, 데이터 폴더 `data`, 작업자 4 개) → `db_open` ② `SIGPIPE` 무시, `SIGINT/SIGTERM` 은 `on_signal` 로 연결 ③ `open_listener` 로 리스닝 소켓 열기 (실패하면 종료 코드 1) ④ 시그널을 막은 채 `pool_start` (작업자는 시그널을 안 받음) ⑤ `g_stop` 이 설 때까지 `accept` → `pool_submit` (큐가 꽉 차면 503) ⑥ 소켓 닫기 → `pool_stop` → DB 닫고 종료 코드 0 |
| `on_signal(sig)`      | Ctrl+C 를 받으면 `g_stop = 1` 만 한다 (시그널 핸들러 안에서는 플래그만 바꾸는 것이 안전). 이미 1 이면(두 번째 Ctrl+C) `_exit(130)` 으로 바로 끝낸다 |
| `open_listener(port)` | `socket` → `SO_REUSEADDR` (재시작 시 포트 충돌 방지) → `bind` → `listen`. 실패하면 -1                                                                                                                                                                     |
| `handle_client(fd)`   | 연결 하나 처리 (작업자 스레드가 부름): recv 타임아웃 설정 → 요청 읽기 → 성공이면 `route`, 실패면 오류 응답 → 로그 → `close`. 버퍼는 스레드마다 자기 스택에 둔다 |
| `route(fd, req)`      | 경로·메서드로 핸들러 선택. 경로 없음 404, 메서드 틀림 405                                                                                                                                                                                                       |
| `handle_health(fd)`   | `GET /health` → `{"ok":true,"status":"up"}`                                                                                                                                                                                                 |
| `handle_rpc(fd, req)` | `POST /rpc`. 본문을 `rpc_handle` 에 넘기고 응답을 HTTP 200 으로 보낸다. 알림이면 204, 메모리 부족이면 500                                                                                                                                                             |

### `server/pool.c` — 스레드 풀

| 함수 | 하는 일 |
|---|---|
| `pool_start(n, handler)` | 작업자 스레드 n 개(1~64)를 `pthread_create` 로 띄운다 |
| `pool_submit(fd)` | 연결을 원형 큐 뒤에 넣고 `pthread_cond_signal` 로 잠든 작업자 하나를 깨운다. 큐(64)가 꽉 차면 -1 (→ main 이 503) |
| `worker(arg)` | 작업자 본체. 락 → 큐가 빌 동안 `pthread_cond_wait` 로 잠듦 → fd 하나 꺼냄 → **락을 풀고** `handler(fd)`. 종료 중이고 큐가 비면 끝난다 |
| `pool_stop()` | 종료 플래그 → `pthread_cond_broadcast` 로 모두 깨움 → 남은 일을 마친 작업자들을 `pthread_join` |

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
| `run_statement(...)` | 문장 하나: `parse` → `lock_for` → `execute` → `unlock_for` → `statement_free`. INSERT 면 `fflush` 후 `{"type":"insert","id":N}`, SELECT 면 `rows`·`columns`·`row_count` 객체를 붙인다 |
| `lock_for(db, stmt, &slot)` | 실행 전 락 잡기. `g_catalog` 읽기 락 → 테이블 찾기 → 없으면 쓰기 락으로 바꿔 `db_get_table` 로 등록 후 다시 → 있으면 테이블 락 (SELECT 읽기 / INSERT 쓰기). 테이블이 없으면 락 없이 오류 |
| `unlock_for(slot)` | 테이블 락 → `g_catalog` 순서로 푼다 |
| `init_locks()` | `pthread_once` 로 딱 한 번: 락들을 "쓰기 우선" 속성으로 초기화 |
| `find_open_table(db, name)` | 열린 테이블 목록에서 이름으로 위치 찾기 (-1 = 아직 안 열림) |
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
| 503 | 작업자도 대기 큐(64)도 꽉 참 |

---

## 5. 성능 비교 (`make bench`)

users 테이블에 5만 줄을 넣어 두고, 클라이언트 16 개가 동시에 요청을 보낸다. 서버 작업자 수만 바꿔 가며 같은 부하를 준다.
(Docker, CPU 24 개. 요청마다 연결을 새로 맺는다. 숫자는 돌릴 때마다 조금씩 달라진다.)

**총 소요 시간** (요청 전부를 끝내는 데 걸린 시간, 작을수록 빠름)

| 부하 | 요청 수 | 작업자 1 | 2 | 4 | 8 |
|---|---|---|---|---|---|
| **scan** `WHERE age = -1` (5만 줄 전체 훑기) | 400 | **916.8 ms** | 459.9 ms | 243.8 ms | **136.8 ms** (6.7 배 빠름) |
| **lookup** `WHERE id = 777` (B+ 트리, 행 1 개) | 4,000 | 72.1 ms | 45.9 ms | 44.2 ms | 53.8 ms |
| **insert** (쓰기 락) | 4,000 | 62.3 ms | 53.4 ms | 53.6 ms | 52.5 ms |

**요청 하나의 평균 응답 시간** (괄호 안은 p99: 100 개 중 가장 느린 1 개 수준)

| 부하 | 작업자 1 | 2 | 4 | 8 |
|---|---|---|---|---|
| **scan** | 35.99 ms (42.26) | 18.05 ms (22.26) | 9.53 ms (15.40) | **5.31 ms** (15.50) |
| **lookup** | 0.28 ms (0.76) | 0.18 ms (0.35) | 0.17 ms (0.31) | 0.21 ms (0.48) |
| **insert** | 0.25 ms (0.68) | 0.21 ms (0.41) | 0.21 ms (0.42) | 0.21 ms (0.55) |

**읽는 법**

- **scan 은 작업자 수에 거의 정비례** (1 → 8 개에서 916.8 ms → 136.8 ms, 약 6.7 배). 요청 하나가 무거워서(36 ms) 일이 대부분 작업자 안에서 일어나고, SELECT 는 **읽기 락**이라 같은 테이블을 8 개가 동시에 훑는다. 스레드 풀 + rwlock 이 제 몫을 하는 경우다.
- **lookup 은 2 개 이후로 안 빨라진다** (72.1 → 약 45 ms 에서 멈춤). B+ 트리 덕에 요청 하나가 수 μs 라, 시간 대부분이 연결 맺기·`accept`·`send` 같은 네트워크 처리다. 그런데 `accept` 는 main 스레드 하나만 하므로, 작업자를 늘려도 그 앞에서 막힌다 (병목이 옮겨 간다).
- **insert 는 거의 그대로다** (52~62 ms). 같은 테이블의 INSERT 는 **쓰기 락**이라 한 번에 하나씩만 돈다. `next_id`·B+ 트리·파일 끝 쓰기가 순서대로여야 하므로 의도한 결과다. (테이블이 여럿이면 테이블끼리는 동시에 된다.)
- 정리: **스레드 풀은 "요청 하나에 드는 일" 이 클수록, 그리고 읽기일수록 효과가 크다.** 가벼운 요청은 연결 처리 비용이, 쓰기는 락이 상한을 정한다.

더 빠르게 하려면 (이번 범위 밖): 연결 재사용(HTTP keep-alive), `accept` 를 여러 스레드가 함께 하기(`SO_REUSEPORT`), 한 요청에 INSERT 여러 개 묶어 보내기.

---

## 6. 실행 (Docker 안에서)

저장소 루트에서 dev 컨테이너에 들어간 뒤:

```bash
cd CDH/dbms
make && make test && make e2e
make run                       # 데이터: 저장소 루트의 data/ (users 테이블), 작업자 4 개
make run THREADS=8             # 작업자 수 바꾸기
make bench                     # 성능 비교 (1~2 분)
```

다른 터미널에서:

```bash
curl localhost:8080/health
curl -X POST localhost:8080/rpc -d '{"jsonrpc":"2.0","method":"query","params":["SELECT * FROM users;"],"id":1}'
```
