# mini-dbms API 서버 (CDH)

루트 `src/` 의 DB 엔진(Phase 1 SQL 처리기 + Phase 2 B+ 트리)을 **HTTP API** 로 감싸는 서버.
외부 클라이언트가 `curl` 같은 도구로 DB 를 쓸 수 있게 만드는 것이 목표다.

## 로드맵

| 단계 | 상태 | 끝나면 되는 것 |
|---|---|---|
| 1. 베이스 서버 | ✅ | `GET /health` 응답. 잘못된 요청에 400/404/405/408/413 |
| 2. SQL API | ⏳ | `POST /query` 에 SQL 을 보내면 엔진이 실행하고 JSON 으로 결과 |
| 3. 스레드 풀 | ⏳ | worker N 개가 요청을 병렬 처리 + 락으로 데이터 보호 |
| 4. 마감 | ⏳ | 안전한 종료, 성능 비교 |

---

## 1. 사용한 기술

| 기술                                                                    | 어디에                | 왜                                           |
| --------------------------------------------------------------------- | ------------------ | ------------------------------------------- |
| **C11**                                                               | 서버 전체              | 엔진(`src/`)이 C 라서 그대로 링크해 쓴다                 |
| **BSD 소켓 API** (`socket`, `bind`, `listen`, `accept`, `recv`, `send`) | `main.c`, `http.c` | 라이브러리 없이 TCP 서버를 직접 만든다                     |
| **HTTP/1.1** (직접 파싱)                                                  | `http.c`           | 요청 줄 + 헤더 + `Content-Length` 본문만 지원하는 최소 구현 |
| **JSON** (문자열로 직접 작성)                                                 | 응답 본문              | 클라이언트가 결과를 읽기 쉽다                            |
| **POSIX 시그널** (`sigaction`, `SIGPIPE`, `SIGINT`)                      | `main.c`           | 끊긴 소켓에 써도 안 죽기, Ctrl+C 로 정상 종료              |
| **소켓 타임아웃** (`SO_RCVTIMEO`)                                           | `main.c`           | 요청을 끝까지 안 보내는 클라이언트가 서버를 붙잡지 못하게            |
| **Make**                                                              | `Makefile`         | 빌드, 테스트 실행                                  |
| **Docker** (`gcc:13`)                                                 | 빌드 환경              | Windows 에서도 Linux 소켓 API 로 빌드·실행            |
| **assert / bash + curl**                                              | `tests/`           | 단위 테스트 / 실제 서버를 띄운 기능 테스트                   |

---

## 2. 어떻게 만들었나 (요청 하나의 흐름)

```
 클라이언트(curl)                          서버 (main.c + http.c)
 ───────────────                          ─────────────────────
                              ┌─ open_listener: socket → bind → listen   (시작할 때 한 번)
                              │
 연결 요청  ───────────────▶   ├─ accept            새 연결 fd 를 받는다
 "GET /health HTTP/1.1 ..."   ├─ handle_client
                              │    ├─ http_read_request   recv 를 반복해서 요청을 다 모은다
                              │    │     └─ http_parse    요청 줄·헤더·본문 길이를 해석
                              │    ├─ route               경로·메서드로 핸들러 선택
                              │    │     └─ handle_health
                              │    └─ http_send_json      상태 줄 + 헤더 + JSON 을 보낸다
 ◀───────────────  응답        │
                              └─ close(fd)  →  다시 accept 로 (반복)
```

**핵심 설계 포인트**

1. **반복 서버(iterative server)**: 연결을 하나씩 차례로 처리한다. 가장 단순한 형태로 먼저 정확히 동작하게 만들고, 3단계에서 스레드 풀로 바꾼다.
2. **TCP 는 바이트 스트림**: `recv` 한 번에 요청이 다 온다는 보장이 없다. 그래서 받은 바이트를 버퍼에 쌓으면서 "다 왔나?" 를 `http_parse` 로 반복 확인한다.
3. **파서와 소켓 분리**: `http_parse` 는 문자열만 본다. 그래서 네트워크 없이 단위 테스트할 수 있다.
4. **상한을 둔다**: 요청 전체 64KB. `Content-Length` 가 크면 본문을 받기 전에 413 으로 거절한다 (메모리 폭주 방지).
5. **요청 하나에 연결 하나** (`Connection: close`): Keep-Alive 를 빼서 상태 관리가 필요 없다.

---

## 3. 파일별 기능

```
CDH/dbms/
├── Makefile           빌드·테스트 명령. 엔진은 ../../src 에서 가져온다
├── server/
│   ├── main.c         서버 본체: 리스닝 소켓, accept 반복, 라우팅, 핸들러, 시그널
│   ├── http.h         HTTP 모듈의 공개 인터페이스 (HttpRequest 구조체, 함수 선언)
│   └── http.c         HTTP 요청 읽기·파싱, 응답 보내기
└── tests/
    ├── test_http.c    http_parse 단위 테스트 (소켓 없이 문자열로)
    └── e2e.sh         서버를 실제로 띄우고 curl 로 상태 코드 확인
```

---

## 4. 핵심 함수

### `server/main.c` — 서버 본체

| 함수 | 하는 일 |
|---|---|
| `main` | 프로그램 시작점. ① 포트 정하기 (인자 없으면 8080) ② `SIGPIPE` 무시, `SIGINT/SIGTERM` 은 `on_signal` 로 연결 ③ `open_listener` 로 리스닝 소켓 열기 (실패하면 종료 코드 1) ④ `g_stop` 이 설 때까지 `accept` → `handle_client` 반복 ⑤ 소켓 닫고 종료 코드 0 |
| `open_listener(port)` | `socket` → `SO_REUSEADDR` (재시작 시 포트 충돌 방지) → `bind` → `listen`. 실패하면 -1 |
| `handle_client(fd)` | 연결 하나 처리: recv 타임아웃 설정 → 요청 읽기 → 성공이면 `route`, 실패면 오류 응답 → `close` |
| `route(fd, req)` | 경로·메서드로 핸들러 선택. 경로 없음 404, 메서드 틀림 405 |
| `handle_health(fd)` | `GET /health` → `{"ok":true,"status":"up"}` |
| `on_signal(sig)` | Ctrl+C 를 받으면 `g_stop = 1` 만 한다 (시그널 핸들러 안에서는 플래그만 바꾸는 것이 안전) |

### `server/http.c` — HTTP 처리

| 함수 | 하는 일 |
|---|---|
| `http_parse(buf, len, req)` | 받은 바이트를 요청으로 해석. **0** = 완성, **1** = 아직 덜 옴(더 받아라), **4xx** = 잘못된 요청. 빈 줄(`\r\n\r\n`)로 헤더 끝을 찾고, 요청 줄을 `sscanf` 로 나누고, 본문이 `Content-Length` 만큼 왔는지 본다 |
| `find_content_length(...)` | 헤더 줄들에서 `Content-Length` 를 대소문자 무시로 찾아 숫자로. 숫자가 아니면 400, 상한을 넘으면 413 |
| `http_read_request(fd, buf, req)` | `recv` 를 반복해 버퍼에 쌓고 매번 `http_parse` 로 확인. 시간 초과면 408, 아무것도 못 받고 끊기면 -1 |
| `send_all(fd, data, len)` | `send` 는 일부만 보낼 수 있으므로 다 나갈 때까지 반복. `MSG_NOSIGNAL` 로 끊긴 소켓에서도 안 죽는다 |
| `http_send_json(fd, status, json)` | 상태 줄 + `Content-Type` + `Content-Length` + `Connection: close` + 본문을 보낸다 |
| `http_send_error(fd, status, msg)` | `{"ok":false,"error":"..."}` 모양의 오류 응답 |
| `http_reason(status)` | 200 → `"OK"`, 404 → `"Not Found"` 같은 표준 문구 |

### 상태 코드 정리

| 코드 | 언제 |
|---|---|
| 200 | 정상 |
| 400 | 요청 줄 형식이 틀림, `Content-Length` 가 숫자가 아님, 도중에 끊김 |
| 404 | 없는 경로 |
| 405 | 경로는 있는데 메서드가 틀림 (예: `POST /health`) |
| 408 | 5초 안에 요청을 다 안 보냄 |
| 413 | 요청이 64KB 를 넘음 |

---

## 5. 실행 (Docker 안에서)

저장소 루트에서 dev 컨테이너에 들어간 뒤:

```bash
cd CDH/dbms
make && make test && make e2e
make run                       # 다른 터미널에서: curl localhost:8080/health
```
