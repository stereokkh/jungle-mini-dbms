# Phase 3 개념 학습: API 와 API 서버

Phase 3 과제는 "Phase 1+2 엔진을 **외부 클라이언트가 호출할 수 있는 API 서버**로 감싸고, **스레드 풀**로 요청을 병렬 처리"하는 것이다.
이 문서는 코드를 쓰기 전에 알아야 할 개념을 위에서부터 차례로 쌓는다.

1. API 란 무엇인가
2. API 서버란 무엇인가
3. 네트워크 기초: 클라이언트·서버·IP·포트·TCP·소켓
4. HTTP: 요청과 응답의 모양
5. 세상에 나와 있는 API 방식들 (REST, RPC, GraphQL, WebSocket, DB 전용 프로토콜 …)
6. 실제 DB 들은 API 를 어떻게 제공하나
7. 데이터 포맷: JSON
8. API 를 사용하는 방법 (curl, 브라우저, Postman, 코드)
9. 서버 아키텍처: 반복 서버 → 요청마다 스레드 → 스레드 풀
10. 동시성 문제: 우리 엔진에서 무엇이 깨지나
11. 우리 mini-dbms API 설계안
12. 구현할 때 자주 밟는 함정
13. 테스트 방법
14. 용어 정리 · 스스로 점검 질문

---

## 1. API 란 무엇인가

**API (Application Programming Interface)** 는 "프로그램이 다른 프로그램의 기능을 쓰는 **약속된 방법**"이다.
핵심은 **약속(계약)** 이다. 쓰는 쪽은 "이렇게 부르면 이런 결과가 온다"만 알면 되고, 안이 어떻게 구현됐는지는 몰라도 된다.

식당으로 비유하면:

| 식당 | 프로그램 |
|---|---|
| 손님 | API 를 쓰는 쪽 (클라이언트) |
| 메뉴판 | API 명세 (무엇을, 어떤 형식으로 요청할 수 있는지) |
| 주문서 | 요청 (request) |
| 나온 음식 | 응답 (response) |
| 주방 | 실제 구현 (손님은 볼 필요 없음) |

### API 는 네트워크만의 이야기가 아니다

우리는 이미 API 를 만들었다. **C 함수 선언도 API 다.**

```c
// src/executor.h — 우리 엔진의 "내부 API"
DbStatus execute(Db *db, const Statement *stmt, RowCallback on_row, void *ctx,
                 ExecResult *result, DbError *err);
```

`main.c` 는 `execute` 안에서 CSV 를 어떻게 읽는지 모른다. 인자와 반환값이라는 **약속**만 안다. 이것이 API 다.

API 의 층을 나눠 보면:

| 종류               | 예                                          | 호출 방법                       |
| ---------------- | ------------------------------------------ | --------------------------- |
| 라이브러리 API        | `printf`, `pthread_create`, 우리 `execute()` | 같은 프로세스 안에서 함수 호출           |
| OS API (시스템 콜)   | `open`, `read`, `socket`, `accept`         | 커널에게 부탁                     |
| **웹 / 네트워크 API** | `POST /query` 같은 HTTP 요청                   | **다른 컴퓨터(프로세스)에게 네트워크로 부탁** |

Phase 3 에서 만드는 것은 마지막 줄이다. 지금은 `execute()` 를 **같은 프로그램 안에서만** 부를 수 있는데, 이것을 **네트워크 너머에서도** 부를 수 있게 만드는 것이 과제다.

---

## 2. API 서버란 무엇인가

**API 서버** 는 네트워크로 들어온 요청을 받아서, 약속된 기능을 실행하고, 결과를 응답으로 돌려주는 **항상 켜져 있는 프로그램**이다.

```
 [클라이언트]                          [API 서버 = Phase 3 결과물]
 curl / 브라우저 / 파이썬             ┌──────────────────────────────────────┐
                                    │  네트워크 계층 (소켓, HTTP 파싱)       │
   POST /query                       │        │                             │
   "SELECT * FROM users"   ────────▶ │        ▼                             │
                                    │  스레드 풀 (요청을 일꾼에게 배분)      │
                                    │        │                             │
                                    │        ▼                             │
                                    │  DB 엔진 (Phase 1 + Phase 2)          │
   200 OK                            │  tokenize → parse → execute          │
   {"rows":[...]}          ◀──────── │  storage + B+ tree                   │
                                    └──────────────────────────────────────┘
```

Phase 1 CLI 와 비교하면 차이가 분명하다.

|        | Phase 1 CLI                       | Phase 3 API 서버               |
| ------ | --------------------------------- | ---------------------------- |
| 입력     | 파일 경로 (`./build/mini-dbms q.sql`) | 네트워크로 들어온 HTTP 요청            |
| 출력     | 터미널에 `printf`                     | 응답 본문 (JSON)                 |
| 수명     | 파일 다 처리하면 종료                      | 계속 켜져서 다음 요청을 기다림            |
| 동시 사용자 | 1명                                | 여러 명이 동시에                    |
| 오류 처리  | 메시지 찍고 끝                          | **요청 하나가 실패해도 서버는 살아 있어야** 함 |

그래서 Phase 1 설계에서 미리 지킨 규칙들이 여기서 효과를 낸다 ([phase1-design.md](phase1-design.md) 참고).

- 전역 변수 없음 → 여러 스레드가 같은 함수를 동시에 불러도 서로 덮어쓰지 않는다
- SELECT 결과를 콜백으로 → CLI 는 `printf`, 서버는 JSON 버퍼에 쓰면 된다
- 실패를 `exit()` 대신 `DbStatus` 로 → 요청 하나의 오류로 서버가 죽지 않는다

---

## 3. 네트워크 기초

### 3.1 클라이언트와 서버

- **서버**: 먼저 켜져서 "여기로 연락하세요" 하고 **기다리는** 쪽
- **클라이언트**: 서버 주소를 알고 **먼저 연락하는** 쪽

### 3.2 IP 주소와 포트

- **IP 주소** = 건물 주소 (어느 컴퓨터인가). `127.0.0.1` 은 "나 자신" (localhost)
- **포트** = 건물 안의 호수 (그 컴퓨터의 어느 프로그램인가). 0~65535 의 숫자

`http://127.0.0.1:8080/query` → 내 컴퓨터의 8080번 포트에서 기다리는 프로그램에게 `/query` 를 요청한다는 뜻이다.

자주 보는 포트: HTTP 80, HTTPS 443, MySQL 3306, PostgreSQL 5432, Redis 6379. 1024 미만은 관리자 권한이 필요하므로 개발 중에는 8080 같은 번호를 쓴다.

### 3.3 TCP

인터넷의 데이터는 작은 **패킷**으로 쪼개져 오간다. 패킷은 잃어버리거나 순서가 바뀔 수 있다.
**TCP** 는 그 위에서 "보낸 순서대로, 빠짐없이 도착하는 **바이트 스트림**"을 만들어 주는 규약이다.

여기서 가장 중요한 성질:

> **TCP 는 "메시지"가 아니라 "바이트의 흐름"이다.**
> 클라이언트가 한 번에 1000바이트를 보내도, 서버의 `recv()` 는 300바이트, 700바이트로 나눠 받을 수 있다.
> 반대로 두 요청이 한 번의 `recv()` 에 붙어서 올 수도 있다.

그래서 "요청 하나가 어디서 끝나는지"는 TCP 가 알려주지 않는다. 그 위의 프로토콜(HTTP)이 정한다 (→ 4장의 `Content-Length`).

### 3.4 소켓 (socket)

**소켓**은 OS 가 주는 "네트워크 연결의 손잡이"다. C 에서는 파일처럼 **정수 하나(파일 디스크립터)** 로 다룬다. `read`/`write` 대신 `recv`/`send` 를 쓴다는 점만 빼면 파일과 비슷하다.

서버 쪽 소켓 API 순서:

```
socket()   소켓 만들기 (전화기 사기)
   │
bind()     IP·포트 붙이기 (전화번호 개통)
   │
listen()   연결 기다리기 시작 (벨소리 켜기)
   │
accept()   ◀─── 클라이언트가 connect() 하면 새 소켓을 돌려줌 (전화 받기)
   │            ※ 듣는 소켓과 대화용 소켓이 따로다
recv()     요청 바이트 읽기
send()     응답 바이트 쓰기
close()    끊기
```

최소한의 C 코드 (오류 처리 생략, 개념용):

```c
#include <arpa/inet.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void)
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);          // TCP 소켓

    int yes = 1;                                              // 서버를 껐다 켰을 때
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes); // "Address already in use" 방지

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);                 // 모든 네트워크 카드에서 받기
    addr.sin_port        = htons(8080);                       // 포트는 네트워크 바이트 순서로
    bind(listen_fd, (struct sockaddr *)&addr, sizeof addr);
    listen(listen_fd, 128);                                   // 대기열 길이

    for (;;) {
        int client_fd = accept(listen_fd, NULL, NULL);        // 클라이언트가 올 때까지 멈춤
        char buf[4096];
        recv(client_fd, buf, sizeof buf, 0);                  // (실제로는 끝까지 반복해서 읽어야 함)

        const char *resp =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 5\r\n"
            "Connection: close\r\n"
            "\r\n"
            "hello";
        send(client_fd, resp, strlen(resp), 0);
        close(client_fd);
    }
}
```

이것만으로도 `curl http://127.0.0.1:8080/` 을 치면 `hello` 가 온다. **API 서버는 결국 이 루프를 키운 것**이다.

---

## 4. HTTP: 요청과 응답의 모양

**HTTP** 는 TCP 위에서 "요청과 응답을 어떤 글자 모양으로 주고받을지" 정한 약속이다. HTTP/1.1 은 **사람이 읽을 수 있는 텍스트**라서 C 로 직접 파싱할 수 있다.

### 4.1 요청 (Request)

```
POST /query HTTP/1.1\r\n                  ← 요청 줄: 메서드 경로 버전
Host: 127.0.0.1:8080\r\n                  ← 헤더들 (이름: 값)
Content-Type: application/json\r\n
Content-Length: 34\r\n
\r\n                                      ← 빈 줄 = 헤더 끝
{"sql":"SELECT * FROM users;"}            ← 본문 (body), 정확히 Content-Length 바이트
```

- 줄 끝은 `\r\n` (CRLF) 이다
- **헤더가 끝나는 곳 = `\r\n\r\n`** 이 처음 나오는 곳
- **본문이 끝나는 곳 = 헤더 끝 + `Content-Length` 바이트**
  → 3.3 에서 말한 "TCP 는 끝을 모른다" 문제를 HTTP 는 이 두 규칙으로 푼다

### 4.2 응답 (Response)

```
HTTP/1.1 200 OK\r\n                       ← 상태 줄: 버전 상태코드 이유문구
Content-Type: application/json\r\n
Content-Length: 52\r\n
Connection: close\r\n
\r\n
{"ok":true,"columns":["id","name"],"rows":[[1,"kim"]]}
```

### 4.3 메서드 (Method) — "무엇을 하려는가"

| 메서드 | 의미 | 본문 | 예 |
|---|---|---|---|
| `GET` | 조회 (서버 상태를 바꾸지 않음) | 보통 없음 | `GET /tables` |
| `POST` | 생성 / 처리 요청 | 있음 | `POST /query` |
| `PUT` | 통째로 교체 | 있음 | `PUT /users/1` |
| `PATCH` | 일부 수정 | 있음 | `PATCH /users/1` |
| `DELETE` | 삭제 | 보통 없음 | `DELETE /users/1` |

### 4.4 상태 코드 (Status Code) — "결과가 어땠나"

| 범위 | 의미 | 자주 쓰는 것 |
|---|---|---|
| 2xx | 성공 | `200 OK`, `201 Created`, `204 No Content` |
| 3xx | 다른 곳으로 가라 | `301`, `304` |
| 4xx | **클라이언트 잘못** | `400 Bad Request`, `404 Not Found`, `405 Method Not Allowed`, `413 Payload Too Large` |
| 5xx | **서버 잘못** | `500 Internal Server Error`, `503 Service Unavailable` |

4xx 와 5xx 를 구분하는 기준: "같은 요청을 다시 보내면 성공할 가능성이 있나?" SQL 문법 오류는 몇 번을 보내도 실패하니 4xx, 디스크 쓰기 실패는 서버 사정이니 5xx.

### 4.5 Keep-Alive

HTTP/1.1 은 기본적으로 **연결 하나로 요청을 여러 번** 주고받는다 (keep-alive). 처음 구현할 때는 응답에 `Connection: close` 를 넣고 **요청 하나 처리 후 연결을 닫는** 방식이 훨씬 단순하다. 다 되면 keep-alive 를 차별화 요소로 넣을 수 있다.

### 4.6 HTTPS

HTTPS = HTTP 를 TLS 로 암호화한 것. 내용은 같고 포장만 다르다. 과제 범위에서는 필요 없다 (실무에서는 앞단의 nginx 같은 프록시가 TLS 를 처리하고 뒤의 서버는 평문 HTTP 를 받는 구성이 흔하다).

---

## 5. 세상에 나와 있는 API 방식들

"네트워크로 기능을 제공한다"는 목표는 같지만, **요청을 어떤 모양으로 만들지**에 따라 여러 스타일이 있다.

### 5.1 REST

**자원(resource)을 URL 로, 행동을 HTTP 메서드로** 표현한다. 웹 API 에서 가장 흔하다.

```
GET    /tables/users/rows          users 의 모든 행
GET    /tables/users/rows/42       id=42 인 행
POST   /tables/users/rows          새 행 추가  (본문: {"name":"kim","age":20})
DELETE /tables/users/rows/42       id=42 삭제
```

- 장점: URL 만 봐도 뜻이 보임, HTTP 기능(캐시, 상태코드)을 그대로 활용
- 단점: 복잡한 조건 검색(`WHERE age >= 18 AND NOT ...`)을 URL 로 표현하기 어려움

### 5.2 RPC (Remote Procedure Call)

**"원격에 있는 함수를 호출한다"** 는 발상. URL 은 하나 또는 몇 개로 두고, 본문에 "어떤 함수를 어떤 인자로"를 담는다.

```
POST /query
{"sql": "SELECT name FROM users WHERE age >= 18"}
```

우리 엔진은 이미 "SQL 문자열 → 실행"이라는 함수 하나로 되어 있으므로 **RPC 스타일이 가장 자연스럽다.**

RPC 의 구체적인 규격들:
- **JSON-RPC**: `{"jsonrpc":"2.0","method":"query","params":[...],"id":1}` 처럼 형식을 표준화한 것
- **gRPC**: Google 이 만든 것. HTTP/2 위에서 **Protocol Buffers(이진 포맷)** 로 주고받고, `.proto` 파일로 함수 목록을 정의하면 코드를 자동 생성해 준다. 서비스 간 내부 통신에 많이 씀

### 5.3 GraphQL

엔드포인트 하나(`POST /graphql`)에 **클라이언트가 원하는 필드 모양을 질의**로 보낸다.

```graphql
{ user(id: 42) { name  age } }
```

필요한 것만 정확히 가져올 수 있어 프론트엔드가 많은 서비스에서 쓴다. SQL 엔진을 감싸는 과제에는 과하다.

### 5.4 SOAP

XML 로 된 엄격한 규격. 은행·공공기관 등 오래된 시스템에서 아직 보인다. 새로 만드는 일은 드물다.

### 5.5 WebSocket

HTTP 로 연결을 시작한 뒤 **양방향으로 계속 열어 두는** 방식. 서버가 먼저 클라이언트에게 보낼 수 있어서 채팅, 실시간 알림에 쓴다. "요청 → 응답" 이 기본인 DB 질의에는 필요 없다.

### 5.6 전용 바이너리 프로토콜 (TCP 직접)

HTTP 를 쓰지 않고 **TCP 위에 자기만의 규칙**을 정한다. 대부분의 "진짜" DB 서버가 이 방식이다 (6장).

### 한눈에 비교

| 방식         | 전송            | 데이터      | 한 줄 요약             | 우리 과제와의 궁합     |
| ---------- | ------------- | -------- | ------------------ | -------------- |
| REST       | HTTP          | 주로 JSON  | URL = 자원, 메서드 = 행동 | 보조 엔드포인트용      |
| RPC (JSON) | HTTP          | JSON     | URL 하나에 "함수+인자"    | **★ 본 기능에 추천** |
| gRPC       | HTTP/2        | Protobuf | 코드 자동생성 RPC        | C 구현 부담 큼      |
| GraphQL    | HTTP          | JSON     | 원하는 모양을 질의         | 과함             |
| WebSocket  | TCP(HTTP로 시작) | 자유       | 양방향 상시 연결          | 불필요            |
| 전용 프로토콜    | TCP           | 이진/텍스트   | DB 서버들의 방식         | 차별화 요소로 가능     |

---

## 6. 실제 DB 들은 API 를 어떻게 제공하나

"DB 를 외부에서 쓰게 한다"는 문제는 실제 제품들이 이미 풀었다. 크게 두 갈래다.

### 6.1 전용 와이어 프로토콜 (TCP)

| DB         | 포트   | 방식                                                       |
| ---------- | ---- | -------------------------------------------------------- |
| MySQL      | 3306 | 자체 이진 프로토콜. 패킷마다 길이 헤더                                   |
| PostgreSQL | 5432 | 자체 이진 프로토콜. "메시지 타입 1바이트 + 길이 4바이트 + 내용"                 |
| Redis      | 6379 | **RESP** 라는 텍스트 프로토콜. `*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n` |

이 DB 들을 쓸 때 우리가 소켓을 직접 다루지 않는 이유는 **드라이버(클라이언트 라이브러리)** 가 프로토콜을 대신 처리해 주기 때문이다. `mysql` CLI, Python 의 `psycopg`, Java 의 JDBC 등이 그 예다.

Redis 의 RESP 는 단순해서 "TCP 위에 내 프로토콜을 설계한다"는 예시로 공부하기 좋다. 길이를 앞에 적어 **메시지 끝을 알 수 있게** 한 점이 HTTP 의 `Content-Length` 와 같은 발상이다.

### 6.2 HTTP API 를 제공하는 DB

| 제품                                | 방식                               | 예                                                                  |
| --------------------------------- | -------------------------------- | ------------------------------------------------------------------ |
| **ClickHouse**                    | HTTP 로 SQL 을 그대로 보냄 (기본 8123 포트) | `curl 'http://localhost:8123/' --data-binary 'SELECT 1'`           |
| **rqlite** (SQLite 를 분산 서버로 감싼 것) | 쓰기/읽기 엔드포인트를 나눔                  | `POST /db/execute` 에 `["INSERT ..."]`, `GET /db/query?q=SELECT...` |
| **PostgREST**                     | PostgreSQL 테이블을 자동으로 REST 로 노출   | `GET /users?age=gte.18`                                            |
| **Elasticsearch**                 | 전부 REST + JSON (기본 9200)         | `GET /users/_search`                                               |
| **CouchDB**                       | 전부 REST + JSON (기본 5984)         | `GET /db/doc_id`                                                   |

> **우리 과제와 가장 닮은 것은 ClickHouse HTTP 인터페이스와 rqlite** 다. 둘 다 "SQL 문자열을 HTTP 본문으로 받아 실행하고 결과를 돌려준다". rqlite 처럼 **쓰기(execute)와 읽기(query)를 나누는** 설계는 스레드 풀에서 락을 다르게 잡는 근거가 된다 (10장).

---

## 7. 데이터 포맷: JSON

API 응답의 사실상 표준은 **JSON** 이다.

```json
{
  "ok": true,
  "columns": ["id", "name", "age"],
  "rows": [
    [1, "kim", 20],
    [2, "lee", 17]
  ],
  "row_count": 2
}
```

| JSON 타입 | 예 | 우리 엔진 타입 |
|---|---|---|
| 숫자 | `20`, `-3` | `INT` (`long long`) |
| 문자열 | `"kim"` | `TEXT` |
| 배열 | `[1, 2]` | 한 행 / 행 목록 |
| 객체 | `{"k": v}` | 응답 전체 |
| 불리언 / null | `true`, `null` | (상태 표시용) |

### C 로 JSON 을 만들 때 반드시 할 일: 이스케이프

TEXT 값을 그대로 `"` 사이에 넣으면 깨진다. 이름이 `ab"c` 면 `"ab"c"` 가 되어 JSON 이 망가진다.

| 원래 글자               | JSON 안에서    |
| ------------------- | ----------- |
| `"`                 | `\"`        |
| `\`                 | `\\`        |
| 줄바꿈                 | `\n`        |
| 탭 / CR              | `\t` / `\r` |
| 그 밖의 0x00~0x1F 제어문자 | `\u00XX`    |

Phase 1 의 `print_value` 가 CSV 용으로 `"` → `""` 를 했던 것과 같은 일을 JSON 규칙으로 하는 함수가 필요하다.

**요청** 본문을 JSON 으로 받기로 하면 JSON **파싱**도 필요하다. 직접 짜기 부담되면 두 가지 선택지가 있다.
- 요청 본문은 **SQL 원문 그대로** (`Content-Type: text/plain`) 받고, 응답만 JSON 으로 만든다 → 파서 불필요 (ClickHouse 방식)
- 작은 단일 파일 라이브러리(cJSON 등)를 쓴다

---

## 8. API 를 사용하는 방법

서버를 만들었으면 **클라이언트 입장에서 호출**해 봐야 한다. 대표적인 도구들:

### 8.1 curl — 터미널에서 HTTP 요청 (가장 많이 씀)

```bash
# GET
curl http://127.0.0.1:8080/health

# POST, 본문에 SQL 원문
curl -X POST http://127.0.0.1:8080/query --data-binary "SELECT * FROM users;"

# POST, JSON 본문
curl -X POST http://127.0.0.1:8080/query \
     -H "Content-Type: application/json" \
     -d '{"sql":"INSERT INTO users VALUES (\"kim\", 20);"}'

# 요청/응답 헤더까지 전부 보기 (디버깅할 때)
curl -v http://127.0.0.1:8080/health

# 응답 JSON 예쁘게 보기
curl -s -X POST http://127.0.0.1:8080/query --data-binary "SELECT * FROM users;" | jq .
```

| 옵션 | 뜻 |
|---|---|
| `-X POST` | 메서드 지정 |
| `-H "이름: 값"` | 헤더 추가 |
| `-d` / `--data-binary` | 본문 (`--data-binary` 는 줄바꿈을 그대로 보냄) |
| `-v` | 주고받는 원문을 모두 출력 |
| `-s` | 진행률 표시 끄기 |
| `-i` | 응답 헤더도 출력 |

### 8.2 nc (netcat) — HTTP 원문을 손으로 보내 보기

HTTP 가 그냥 텍스트라는 걸 직접 확인할 수 있다. 서버의 파서 테스트에도 좋다 (예: 일부러 잘못된 요청 보내기).

```bash
printf 'GET /health HTTP/1.1\r\nHost: x\r\n\r\n' | nc 127.0.0.1 8080
```

### 8.3 브라우저

주소창은 **GET** 만 보낼 수 있다. `http://127.0.0.1:8080/health` 확인용으로 충분하다.
브라우저의 개발자 도구(F12) → Network 탭에서 요청·응답 원문을 볼 수 있다.

### 8.4 Postman / Insomnia / VS Code REST Client

GUI 로 요청을 만들고 저장해 두는 도구. 팀원끼리 요청 모음을 공유하기 좋다.
VS Code 의 REST Client 확장은 `.http` 파일에 요청을 적어 두고 클릭해서 보낸다.

```http
### 헬스체크
GET http://127.0.0.1:8080/health

### 조회
POST http://127.0.0.1:8080/query
Content-Type: text/plain

SELECT * FROM users WHERE id = 1;
```

### 8.5 코드에서 호출 (예: Python)

```python
import requests

r = requests.post("http://127.0.0.1:8080/query", data="SELECT * FROM users;")
print(r.status_code)   # 200
print(r.json())        # {'ok': True, 'rows': [...]}
```

### 8.6 부하 테스트 도구

스레드 풀이 정말 병렬로 처리하는지 보려면 **동시 요청**을 쏴야 한다.

```bash
# ab (Apache Bench): 총 1000개 요청을 동시 50개씩
ab -n 1000 -c 50 http://127.0.0.1:8080/health

# wrk: 스레드 4개, 연결 100개로 10초 동안
wrk -t4 -c100 -d10s http://127.0.0.1:8080/health

# hey: POST 본문도 쉽게
hey -n 1000 -c 50 -m POST -d "SELECT * FROM users WHERE id = 7;" http://127.0.0.1:8080/query
```

결과에서 볼 것: **초당 처리량(Requests/sec)**, **지연 시간(latency) 분포**, **실패 수**. 스레드 수를 1, 2, 4, 8 로 바꿔 가며 비교하면 발표 자료가 된다.

### 8.7 Docker 에서 띄울 때

지금 `compose.yaml` 의 `dev` 서비스에는 포트 설정이 없다. 컨테이너 안에서 띄운 서버를 **호스트(내 PC)의 curl/브라우저**로 부르려면 포트를 열어야 한다.

```yaml
services:
  dev:
    # ...기존 설정...
    ports:
      - "8080:8080"     # 호스트 8080 → 컨테이너 8080
```

그리고 서버는 `127.0.0.1` 이 아니라 **`0.0.0.0` (`INADDR_ANY`)** 에 bind 해야 컨테이너 밖에서 들어오는 연결을 받는다. 컨테이너 안에서 curl 로만 테스트할 거면 포트 설정 없이도 된다.

---

## 9. 서버 아키텍처: 반복 서버 → 스레드 → 스레드 풀

### 9.1 반복 서버 (iterative)

3.4 의 코드처럼 `accept → 처리 → close → accept …` 를 한 스레드가 반복한다.

```
요청A ████████
요청B         ████████          ← A 가 끝날 때까지 B 는 기다린다
요청C                 ████████
```

SELECT 하나가 100만 행을 선형 탐색하면, 그동안 다른 모든 사용자가 멈춘다.

### 9.2 요청마다 스레드 생성 (thread-per-request)

`accept` 할 때마다 `pthread_create` 로 새 스레드를 만든다.

```
요청A ████████
요청B  ████████                 ← 동시에 처리
요청C   ████████
```

문제:
- 스레드 생성·소멸 비용이 매번 든다
- 요청 1만 개가 몰리면 스레드 1만 개 → 메모리 고갈, 문맥 전환 폭증 → 서버가 쓰러진다 (**상한이 없다**)

### 9.3 스레드 풀 (thread pool) ★ 과제 요구사항

**미리 N개의 일꾼(worker) 스레드를 만들어 두고**, 들어온 일을 **작업 큐(queue)** 에 넣으면 놀고 있는 일꾼이 꺼내 처리한다.

```
                   ┌─────────────── 작업 큐 (최대 크기 제한) ───────────────┐
 main 스레드        │                                                       │
 accept() ──push──▶ │  [fd=7] [fd=9] [fd=12] ...                            │
                   └────────┬──────────────┬──────────────┬────────────────┘
                            │pop           │pop           │pop
                       ┌────▼───┐     ┌────▼───┐     ┌────▼───┐
                       │worker 1│     │worker 2│     │worker N│   ← 미리 만든 N개
                       └────────┘     └────────┘     └────────┘
                       요청 읽기 → SQL 실행 → JSON 응답 → close → 다시 pop
```

이것이 **생산자–소비자 (producer–consumer)** 패턴이다. main 스레드가 생산자(연결을 넣음), worker 들이 소비자(꺼내 처리).

|           | thread-per-request | thread pool                     |
| --------- | ------------------ | ------------------------------- |
| 스레드 생성 비용 | 요청마다               | 시작할 때 한 번                       |
| 최대 스레드 수  | 무제한 (위험)           | N 개로 고정                         |
| 과부하 시     | 서버 다운              | 큐에서 기다림, 큐가 꽉 차면 `503` 으로 거절 가능 |

### 9.4 스레드 풀을 만드는 재료: mutex 와 condition variable

큐는 main 과 여러 worker 가 **동시에 건드리는 공유 자료**다. 두 가지 도구가 필요하다.

- **mutex (`pthread_mutex_t`)**: "한 번에 한 스레드만 이 구역에 들어온다" — 큐의 head/tail 을 동시에 고치지 못하게
- **condition variable (`pthread_cond_t`)**: "조건이 될 때까지 잠들어 있다가 깨워 주면 일어난다" — 큐가 비었으면 worker 는 CPU 를 쓰지 않고 잔다

```c
typedef struct {
    int             fds[QUEUE_CAP];   // 원형 큐: 클라이언트 소켓 번호
    int             head, tail, count;
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;        // worker 가 기다리는 조건
    pthread_cond_t  not_full;         // main 이 기다리는 조건 (또는 꽉 차면 바로 503)
    int             shutting_down;
} JobQueue;

// main 스레드 (생산자)
void queue_push(JobQueue *q, int fd)
{
    pthread_mutex_lock(&q->lock);
    while (q->count == QUEUE_CAP)
        pthread_cond_wait(&q->not_full, &q->lock);
    q->fds[q->tail] = fd;
    q->tail = (q->tail + 1) % QUEUE_CAP;
    q->count++;
    pthread_cond_signal(&q->not_empty);     // 자는 worker 하나 깨우기
    pthread_mutex_unlock(&q->lock);
}

// worker 스레드 (소비자)
int queue_pop(JobQueue *q)
{
    pthread_mutex_lock(&q->lock);
    while (q->count == 0 && !q->shutting_down)   // if 가 아니라 while! (아래 설명)
        pthread_cond_wait(&q->not_empty, &q->lock);
    if (q->count == 0) {                         // 종료 중이고 일이 없음
        pthread_mutex_unlock(&q->lock);
        return -1;
    }
    int fd = q->fds[q->head];
    q->head = (q->head + 1) % QUEUE_CAP;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->lock);
    return fd;
}

void *worker_main(void *arg)
{
    Server *srv = arg;
    int fd;
    while ((fd = queue_pop(&srv->queue)) >= 0) {
        handle_client(srv, fd);   // 요청 읽기 → 엔진 실행 → 응답 → close(fd)
    }
    return NULL;
}
```

알아 둘 점:
- `pthread_cond_wait` 는 **잠들면서 mutex 를 풀고, 깨어나면서 다시 잡는다.** 그래서 반드시 mutex 를 잡은 상태에서 부른다
- 조건 검사는 **`if` 가 아니라 `while`**. 깨어났는데 다른 worker 가 먼저 가져갔을 수도 있고, 이유 없이 깨는 경우(spurious wakeup)도 있다
- 빌드에 `-pthread` 가 필요하다 (Makefile 의 `LDFLAGS`/`CFLAGS`)

### 9.5 (참고) 다른 서버 모델

| 모델               | 설명                          | 예                                   |
| ---------------- | --------------------------- | ----------------------------------- |
| 프로세스 fork        | 연결마다 `fork()`               | 옛 Apache, PostgreSQL(연결마다 백엔드 프로세스) |
| 스레드 풀            | 이 과제                        | MySQL(연결당 스레드 + 스레드 풀 옵션), Java 서버들 |
| 이벤트 루프 (I/O 다중화) | 스레드 하나가 `epoll` 로 수천 연결을 감시 | nginx, Redis, Node.js               |
| 이벤트 루프 + 스레드 풀   | 네트워크는 epoll, 무거운 일은 풀       | 고성능 서버의 흔한 조합                       |

발표 때 "왜 스레드 풀인가 / 다른 선택지는 뭐가 있나"를 이 표로 설명할 수 있다.

---

## 10. 동시성 문제: 우리 엔진에서 무엇이 깨지나

> 과제의 중점 포인트 첫 줄이 **"멀티 스레드 동시성 이슈"** 다. 여기가 Phase 3 의 핵심이다.

### 10.1 경쟁 상태 (race condition) 란

두 스레드가 같은 데이터를 **동시에 읽고-고치고-쓰면** 결과가 실행 순서에 따라 달라지는 상태.

우리 코드의 실제 예 — [executor.c](../src/executor.c) 의 INSERT:

```c
result->inserted_id = t->next_id;   // ① 읽기
t->next_id++;                       // ② 증가
```

```
스레드 A: next_id 읽음 = 5
스레드 B: next_id 읽음 = 5          ← A 가 아직 ++ 하기 전
스레드 A: id=5 로 저장, next_id = 6
스레드 B: id=5 로 저장, next_id = 6   ← id 5 가 두 줄! (B+ 트리도 꼬임)
```

`next_id++` 한 줄도 기계어로는 "읽기 → 더하기 → 쓰기" 세 단계라서 **원자적이지 않다.**

### 10.2 우리 엔진의 공유 자원 목록

`Db` 하나를 모든 worker 가 함께 쓴다고 하면:

| 공유 자원                        | 위치                            | 누가 건드리나                                | 위험                                  |
| ---------------------------- | ----------------------------- | -------------------------------------- | ----------------------------------- |
| `Db.tables[]`, `table_count` | [db.h](../src/db.h)           | `db_get_table` 이 **처음 열 때 추가**         | 두 스레드가 동시에 같은 테이블을 처음 열면 두 칸에 중복 등록 |
| `Table.next_id`              | [db.h](../src/db.h)           | INSERT                                 | 10.1 의 id 중복                        |
| `Table.fp` (쓰기용 FILE*)       | [storage.c](../src/storage.c) | INSERT 의 append, **SELECT 의 `fflush`** | SELECT 도 쓰기 fp 를 건드린다는 점에 주의        |
| 데이터 파일 내용                    | `data/*.csv`                  | INSERT 쓰기, SELECT 읽기                   | 쓰는 중인 반쪽짜리 줄을 읽을 수 있음               |
| B+ 트리 (Phase 2)              | 메모리                           | INSERT 삽입·노드 분할, SELECT 탐색             | **분할 도중 탐색하면 끊어진 포인터를 따라감 → 크래시**   |

반대로 **안전한 것** (Phase 1 설계 덕분):
- `Token[]`, `Statement`, `DbError`, `ExecResult` → 요청마다 각자 지역 변수로 만든다
- 콜백의 `ctx` → 요청마다 자기 JSON 버퍼를 넘기면 된다
- 전역 변수가 없다

### 10.3 해결 전략 (단순한 것 → 정교한 것)

**① 전역 락 하나 (Big Lock)**
엔진 호출 전체를 mutex 하나로 감싼다.

```c
pthread_mutex_lock(&srv->db_lock);
st = execute(&srv->db, &stmt, json_row_cb, &out, &result, &err);
pthread_mutex_unlock(&srv->db_lock);
```

- 장점: 확실히 안전, 구현 5분
- 단점: **DB 작업은 한 번에 하나** → 병렬인 것은 "소켓 읽기·HTTP 파싱·JSON 만들기·응답 보내기" 뿐
- 그래도 첫 단계로는 이것부터 하고, 테스트가 다 통과한 뒤 ②로 간다

**② 읽기-쓰기 락 (`pthread_rwlock_t`)**
"SELECT 는 여럿이 동시에, INSERT 는 혼자서"

```c
if (stmt.type == STMT_SELECT) pthread_rwlock_rdlock(&srv->db_rwlock);
else                          pthread_rwlock_wrlock(&srv->db_rwlock);
st = execute(...);
pthread_rwlock_unlock(&srv->db_rwlock);
```

- 읽기가 많은 부하에서 처리량이 크게 오른다 → **스레드 풀의 효과를 보여 줄 수 있는 지점**
- 주의 1: SELECT 경로가 정말 "읽기만" 하는지 확인해야 한다. 지금 `storage_scan` 은 쓰기용 `t->fp` 를 `fflush` 하고, `db_get_table` 은 처음 열 때 `tables[]` 에 추가한다 → 서버 시작 시 테이블을 **미리 다 열어 두거나**, 이 부분만 따로 mutex 로 보호해야 한다
- 주의 2: 쓰기가 계속 들어오면 읽기가 굶거나 그 반대가 생길 수 있다 (starvation)

**③ 테이블별 락**
`Table` 마다 rwlock 을 둔다. `users` 에 INSERT 하는 동안 `orders` 를 SELECT 할 수 있다.

**④ 더 나아가면 (차별화 요소)**
- `next_id` 를 `stdatomic.h` 의 원자 연산으로
- B+ 트리 노드 단위 락 (latch crabbing)
- MVCC (실제 PostgreSQL 이 쓰는 방식, 과제 범위 밖)

### 10.4 교착 상태 (deadlock) 주의

락을 2개 이상 쓰기 시작하면 생긴다.
```
스레드 A: users 락 잡음 → orders 락 기다림
스레드 B: orders 락 잡음 → users 락 기다림   → 둘 다 영원히 멈춤
```
규칙: **여러 락을 잡을 때는 항상 같은 순서로** (예: 테이블 이름 순).

### 10.5 C 라이브러리의 스레드 안전성

- `strtok`, `localtime`, `rand` 등은 내부에 정적 상태가 있어 **스레드 안전하지 않다** → `strtok_r`, `localtime_r` 를 쓴다
- `errno` 는 스레드마다 따로라서 괜찮다
- glibc 의 `FILE*` 함수 하나하나는 내부 락이 있지만, **여러 호출을 묶은 동작**(위치 확인 → 쓰기)은 보장되지 않는다

---

## 11. 우리 mini-dbms API 설계안

팀이 확정하기 전의 **초안**이다. 엔진이 "SQL 문자열 → 실행" 구조이므로 RPC 스타일을 중심으로 한다.

### 11.1 엔드포인트

| 메서드    | 경로        | 본문                                       | 설명                             |
| ------ | --------- | ---------------------------------------- | ------------------------------ |
| `GET`  | `/health` | 없음                                       | 서버가 살아 있는지. `{"ok":true}`      |
| `POST` | `/query`  | SQL 원문 (`text/plain`) 또는 `{"sql":"..."}` | 문장 실행. **핵심**                  |
| `GET`  | `/tables` | 없음                                       | (선택) 테이블 목록과 스키마               |
| `GET`  | `/stats`  | 없음                                       | (선택·차별화) 처리한 요청 수, 큐 길이, 스레드 수 |

### 11.2 응답 모양

INSERT 성공
```http
HTTP/1.1 200 OK
Content-Type: application/json

{"ok":true,"type":"insert","inserted_id":3}
```

SELECT 성공
```http
HTTP/1.1 200 OK
Content-Type: application/json

{"ok":true,"type":"select","columns":["name","age"],"rows":[["kim",20]],"row_count":1}
```

실패
```http
HTTP/1.1 400 Bad Request
Content-Type: application/json

{"ok":false,"error":{"code":"SYNTAX","position":14,"message":"Syntax error at position 14: ..."}}
```

`DbError` 의 `code`, `pos`, `message` 를 그대로 옮기면 된다 ([error.h](../src/error.h)).

### 11.3 `DbStatus` → HTTP 상태 코드

| DbStatus | HTTP | 이유 |
|---|---|---|
| `DB_OK` | 200 | 성공 |
| `DB_ERR_SYNTAX` | 400 | 요청(SQL)이 잘못됨 |
| `DB_ERR_TOO_LONG` | 400 (또는 413) | 요청이 한도 초과 |
| `DB_ERR_NO_TABLE` | 404 | 없는 테이블 |
| `DB_ERR_NO_COLUMN` | 400 | 없는 컬럼 |
| `DB_ERR_COLUMN_COUNT` | 400 | 값 개수 불일치 |
| `DB_ERR_TYPE` | 400 | 타입 불일치 |
| `DB_ERR_SCHEMA` | 500 | 서버 쪽 스키마 파일이 망가짐 |
| `DB_ERR_IO` | 500 | 디스크 문제 |
| (HTTP 레벨) | 404 / 405 / 413 / 503 | 없는 경로 / 안 되는 메서드 / 본문 너무 큼 / 큐 꽉 참 |

### 11.4 한 요청에 SQL 여러 문장이 오면?

선택지:
- (a) 첫 문장만 실행, 나머지는 400
- (b) 전부 실행하고 결과 배열을 돌려줌 `{"results":[{...},{...}]}`
- (c) 중간에 실패하면 거기서 멈추고 그때까지 결과 + 오류

(b)/(c)는 CLI 의 동작과 맞추기 쉽다. 팀이 정해서 문서에 적는다.

### 11.5 요청 하나가 엔진을 지나는 길

```
worker: fd 꺼냄
  │
  ├─ recv 반복 → "\r\n\r\n" 찾기 → 요청 줄·헤더 파싱 → Content-Length 만큼 본문 더 읽기
  ├─ 경로/메서드 확인 (없으면 404/405)
  ├─ 본문에서 SQL 꺼내기
  ├─ tokenize → parse                         (락 필요 없음: 요청마다 지역 변수)
  ├─ 락 잡기 (SELECT 면 읽기 락, INSERT 면 쓰기 락)
  │    execute(db, &stmt, json_row_cb, &json_buf, &result, &err)
  │         └─ 행마다 json_row_cb → json_buf 에 [..] 추가 (JSON 이스케이프)
  ├─ 락 풀기                                  ← 응답 보내기 전에 푼다 (느린 클라이언트가 락을 붙잡지 않도록)
  ├─ statement_free
  ├─ 상태 코드 정하기 + JSON 마무리
  ├─ send 반복 (전부 보낼 때까지)
  └─ close(fd)
```

Phase 1 의 [main.c](../src/main.c) `print_row` 를 JSON 버전으로 바꾼 것이 `json_row_cb` 다. 엔진 코드는 거의 그대로 두고 **바깥만 갈아 끼우는 구조**가 과제가 말하는 "내부 DB 엔진과 외부 API 서버 사이 연결 설계"다.

---

## 12. 구현할 때 자주 밟는 함정

| 함정                                 | 증상                                          | 대처                                                              |
| ---------------------------------- | ------------------------------------------- | --------------------------------------------------------------- |
| `recv` 한 번에 다 왔다고 가정               | 큰 요청이나 느린 네트워크에서 본문이 잘림                     | `\r\n\r\n` 과 `Content-Length` 를 다 채울 때까지 반복해서 읽기                |
| `send` 한 번에 다 갔다고 가정               | 큰 SELECT 결과가 잘림                             | 반환값(보낸 바이트 수)을 보고 남은 만큼 반복                                      |
| `Content-Length` 를 그대로 믿고 `malloc` | 악의적인 `Content-Length: 99999999999` 로 메모리 고갈 | 상한을 두고 넘으면 413                                                  |
| 클라이언트가 먼저 끊었는데 `send`              | **SIGPIPE 로 서버 프로세스 전체가 죽음**                | 시작 시 `signal(SIGPIPE, SIG_IGN)` 또는 `send(..., MSG_NOSIGNAL)`    |
| 아무것도 안 보내는 연결                      | worker 하나가 영원히 `recv` 에서 멈춤 → 풀 고갈          | `setsockopt(SO_RCVTIMEO)` 로 읽기 타임아웃                             |
| 서버 재시작 시 bind 실패                   | `Address already in use`                    | `SO_REUSEADDR`                                                  |
| `close` 를 안 함                      | 파일 디스크립터 누수 → 결국 `accept` 실패                | 모든 경로에서 `close(fd)` (오류 경로 포함)                                  |
| 락을 잡은 채 `return`                   | 다음 요청부터 전부 멈춤 (deadlock)                    | 오류 경로에서도 반드시 unlock. `goto out;` 패턴이 유용                         |
| SELECT 결과를 고정 크기 버퍼에               | 100만 행 결과에서 오버플로                            | 늘어나는 버퍼(`realloc`) 또는 결과 행 수 상한                                 |
| `Ctrl+C` 로 종료                      | 버퍼에 남은 INSERT 가 파일에 안 써짐                    | SIGINT 처리 → 큐 종료 → worker join → `db_close` (graceful shutdown) |

---

## 13. 테스트 방법

과제의 품질 항목: "API 서버를 준비하고 해당 서버에서 **기능 테스트**를 통해 API 가 제대로 동작하는지 확인".

### 13.1 단위 테스트 (기존 `make test` 에 추가)
- HTTP 요청 파서: 정상 / 헤더만 / 본문 잘림 / `Content-Length` 없음 / 너무 큼
- JSON 이스케이프: `"`, `\`, 줄바꿈, 제어문자
- 작업 큐: push/pop 순서, 꽉 참, 비었을 때 종료

### 13.2 기능 테스트 (서버 띄우고 curl 로)

```bash
./build/mini-dbms-server 8080 &
SERVER=$!

curl -s localhost:8080/health
curl -s -X POST localhost:8080/query --data-binary "INSERT INTO users VALUES ('kim', 20);"
curl -s -X POST localhost:8080/query --data-binary "SELECT * FROM users WHERE id = 1;"
curl -s -o /dev/null -w "%{http_code}\n" -X POST localhost:8080/query --data-binary "SELEC"   # 400 기대
curl -s -o /dev/null -w "%{http_code}\n" localhost:8080/nope                                  # 404 기대

kill $SERVER
```

기대 결과와 비교하는 스크립트(`tests/api_test.sh`)로 만들어 두면 반복 확인이 쉽다.

### 13.3 동시성 테스트 — 가장 중요

```bash
# 100개 INSERT 를 동시에
for i in $(seq 1 100); do
  curl -s -X POST localhost:8080/query --data-binary "INSERT INTO users VALUES ('u$i', $i);" &
done
wait

# 확인할 것: 100행이 다 들어갔나? id 가 중복 없이 1..100 인가?
cut -d, -f1 data/users.csv | sort -n | uniq -d     # 출력이 없어야 정상 (중복 id 없음)
wc -l data/users.csv
```

- **락을 일부러 빼고** 돌려서 id 중복이 생기는 걸 먼저 보고, 락을 넣어 사라지는 걸 확인하면 동시성 이슈를 설명하는 좋은 자료가 된다
- `make` 에 **ThreadSanitizer** 빌드(`-fsanitize=thread`)를 추가하면 데이터 경쟁을 자동으로 찾아 준다 (ASan 과 동시에는 못 씀)

### 13.4 성능 비교
- 스레드 수 1 / 2 / 4 / 8 × 전역 락 / rwlock 조합으로 `wrk` 또는 `hey` 결과 표 만들기
- 읽기 100% / 읽기 90%+쓰기 10% / 쓰기 100% 부하에서 각각 비교

---

## 14. 용어 정리 · 스스로 점검 질문

### 용어

| 용어                 | 한 줄 정의                              |
| ------------------ | ----------------------------------- |
| API                | 프로그램끼리 기능을 쓰는 약속된 방법                |
| API 서버             | 네트워크 요청을 받아 API 를 실행하고 응답하는 상주 프로그램 |
| 엔드포인트              | 요청을 보내는 주소 하나 (`POST /query`)       |
| 클라이언트 / 서버         | 먼저 연락하는 쪽 / 기다리는 쪽                  |
| 포트                 | 한 컴퓨터 안에서 프로그램을 구분하는 번호             |
| TCP                | 순서대로·빠짐없이 오는 바이트 스트림                |
| 소켓                 | 네트워크 연결을 다루는 파일 디스크립터               |
| HTTP               | TCP 위의 요청/응답 텍스트 규약                 |
| 상태 코드              | 응답 결과를 나타내는 세 자리 숫자                 |
| REST / RPC         | 자원+메서드 중심 / 함수 호출 중심 API 스타일        |
| JSON               | 응답 데이터의 표준 텍스트 포맷                   |
| 스레드 풀              | 미리 만든 N개의 스레드가 작업 큐를 나눠 처리하는 구조     |
| 생산자–소비자            | 넣는 쪽과 꺼내는 쪽이 큐로 연결된 패턴              |
| mutex              | 한 번에 한 스레드만 들어가게 하는 자물쇠             |
| condition variable | 조건이 될 때까지 잠들게 하는 도구                 |
| rwlock             | 읽기는 여럿, 쓰기는 혼자                      |
| race condition     | 실행 순서에 따라 결과가 달라지는 버그               |
| deadlock           | 서로의 락을 기다려 영원히 멈춤                   |
| graceful shutdown  | 하던 요청은 마치고 자원을 정리한 뒤 종료             |

### 스스로 점검 질문 (발표·면접 대비)

1. C 함수 `execute()` 와 `POST /query` 는 둘 다 API 다. 무엇이 같고 무엇이 다른가?
2. TCP 가 "바이트 스트림"이라는 게 서버 구현에 어떤 영향을 주나? HTTP 는 요청의 끝을 어떻게 알려 주나?
3. SQL 문법 오류는 왜 500 이 아니라 400 인가?
4. 우리 API 를 REST 가 아니라 RPC 스타일로 고른 이유는?
5. 요청마다 스레드를 만드는 방식의 문제는? 스레드 풀은 그걸 어떻게 해결하나?
6. `pthread_cond_wait` 를 `if` 가 아니라 `while` 안에서 부르는 이유는?
7. 우리 엔진에서 락 없이 동시에 INSERT 하면 구체적으로 무엇이 깨지나?
8. 전역 락과 rwlock 의 성능 차이는 어떤 부하에서 크게 나나? 그 이유는?
9. Phase 1 에서 "전역 변수 금지", "결과를 콜백으로" 규칙을 둔 것이 Phase 3 에서 어떻게 도움이 됐나?
10. 클라이언트가 응답 도중 연결을 끊으면 서버에 무슨 일이 생길 수 있나? 어떻게 막았나?
11. 서버를 `Ctrl+C` 로 끌 때 데이터가 손실되지 않게 하려면?
12. 스레드 수를 늘리면 처리량이 계속 늘어나나? 어디서 멈추고, 왜?
