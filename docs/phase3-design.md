# Phase 3 설계 메모: HTTP API 서버와 스레드 풀

> **초안이다.** 맨 아래 "팀이 확정할 것"을 정한 뒤에 구현을 시작한다.

Phase 1+2의 엔진(`tokenize_next` → `parse` → `execute`)을 그대로 쓰고, 그 앞에 HTTP 서버와 스레드 풀을 붙인다.
엔진의 구조체와 함수 시그니처는 바꾸지 않는다. `phase1-design.md`의 구현 규칙은 이 문서에서 따로 바꾼 것만 빼고 그대로 지킨다.

## 범위

만드는 것:
- `POST /query` 하나. 본문에 SQL 문장 하나를 받아 JSON으로 답한다
- 고정 크기 스레드 풀. 연결 하나를 워커 하나가 처리한다
- 엔진을 감싸는 락 하나

만들지 않는 것:
- keep-alive, chunked 인코딩, HTTPS, 헤더 해석(`Content-Length`만 읽는다)
- 엔드포인트 추가(`GET /tables` 등), 인증
- Mini-Redis 캐시 (서버가 끝난 뒤 별도 문서로 정한다)

## 요청이 지나가는 길

```
클라이언트 ──▶ accept 스레드 ──▶ [ fd 큐 ] ──▶ 워커 스레드 N개
                (연결을 받아            (꽉 차면           │
                 큐에 넣기만 한다)        503)             ▼
                                               http_read_request   요청 읽기
                                               tokenize_next/parse 락 없이 (공유 상태를 안 건드린다)
                                               ┌─ db_lock ───────┐
                                               │ execute          │ 한 번에 한 스레드만
                                               └──────────────────┘
                                               http_write_response 응답 쓰기
                                               close(fd)
```

## 스레드가 함께 쓰는 것

엔진은 전역 변수가 없지만 `Db` 하나를 모든 워커가 함께 쓴다. 락 없이 동시에 `execute`를 부르면 깨지는 곳:

| 공유 상태 | 깨지는 모습 |
|---|---|
| `Db.tables`, `Db.table_count` | 두 스레드가 같은 테이블을 동시에 처음 열어 두 칸에 중복 등록 |
| `Table.next_id` | 두 INSERT가 같은 id를 받음 |
| `Table.fp` | 두 줄이 섞여서 파일에 쓰임 |
| `Table.index` (B+ 트리) | 노드를 쪼개는 중에 다른 스레드가 읽어 잘못된 포인터를 따라감 |

**락 정책: `pthread_mutex_t` 하나로 `execute` 호출 전체를 감싼다.** SELECT끼리도 동시에 못 돈다. 단순해서 틀릴 곳이 없다.
`tokenize_next`와 `parse`는 요청마다 따로 가진 지역 변수만 쓰므로 락 밖에서 부른다.

## API

```
POST /query
본문: SQL 문장 하나 (끝의 ; 는 있어도 되고 없어도 된다)
```

| 상황 | 상태 코드 | 본문 |
|---|---|---|
| INSERT 성공 | 200 | `{"ok":true,"inserted_id":3}` |
| SELECT 성공 | 200 | `{"ok":true,"rows":[[1,"Kim, Jr",20],[2,"Lee",31]],"row_count":2}` |
| `DB_ERR_SYNTAX`, `DB_ERR_TOO_LONG`, `DB_ERR_NO_COLUMN`, `DB_ERR_COLUMN_COUNT`, `DB_ERR_TYPE` | 400 | `{"ok":false,"error":"Error: column 'nope' does not exist in table 'users'"}` |
| `DB_ERR_NO_TABLE` | 404 | 위와 같은 모양 |
| `DB_ERR_SCHEMA`, `DB_ERR_IO`, 메모리 부족 | 500 | 위와 같은 모양 |
| 빈 본문, 문장이 둘 이상, HTTP 요청이 깨짐 | 400 | `{"ok":false,"error":"..."}` |
| 경로가 `/query`가 아님 | 404 | |
| 메서드가 `POST`가 아님 | 405 | |
| 본문이 `MAX_BODY_LEN`을 넘음 | 413 | |
| 작업 큐가 꽉 참 | 503 | |

- 행은 **값의 배열**로 보낸다. `RowCallback`이 컬럼 이름을 주지 않기 때문에, 시그니처를 바꾸지 않으려면 이 모양이 된다
- `error`에는 `DbError.message`를 그대로 넣는다
- 응답 헤더는 `Content-Type: application/json`, `Content-Length`, `Connection: close` 세 개다
- JSON 문자열은 `"`, `\`, 0x20 미만 제어 문자만 이스케이프한다. 한글은 UTF-8 그대로 보낸다

## 파일 구성

```
src/  json.h  json.c            늘어나는 버퍼 + JSON 쓰기
      http.h  http.c            요청 읽기, 응답 쓰기
      threadpool.h threadpool.c 작업 큐와 워커
      server.h server.c         소켓 열기, accept, 요청 하나 처리
      main.c                    --server 모드 추가
tests/ test_json.c  test_http.c  test_threadpool.c  test_server.c
```

Makefile은 고치지 않는다. 실행 파일이 하나뿐이므로(`main.c`만 `main`을 가진다) 서버는 같은 실행 파일의 모드로 넣는다.
Debian 12의 glibc(2.36)는 pthread가 libc 안에 있어서 `-pthread` 없이 링크된다.

```
./build/mini-dbms queries.sql              기존 CLI (그대로)
./build/mini-dbms --server [포트] [워커 수]   기본값 8080, 4
```

## 구조체와 함수

### json

```c
// 응답 본문을 쌓는 버퍼. 행 수를 미리 알 수 없어서 malloc 으로 늘린다.
typedef struct {
    char  *data;     // 항상 '\0' 으로 끝난다. 아직 아무것도 안 넣었으면 NULL
    size_t len;
    size_t cap;
    int    failed;   // 한 번이라도 늘리기에 실패했으면 1. 그 뒤의 쓰기는 무시한다
} Buf;
```

| 함수 | 하는 일 |
|---|---|
| `void buf_init(Buf *b)` | 빈 버퍼로 만든다 |
| `void buf_append(Buf *b, const char *s)` | 문자열을 뒤에 붙인다. 모자라면 두 배로 늘린다. `MAX_RESPONSE_LEN`을 넘거나 `realloc`이 실패하면 `failed = 1` |
| `void buf_free(Buf *b)` | 해제하고 빈 버퍼로 되돌린다. 두 번 불러도 안전하다 |
| `void json_put_string(Buf *b, const char *s)` | 큰따옴표로 감싸고 이스케이프해서 붙인다 |
| `void json_put_value(Buf *b, const Value *v)` | INT는 숫자, TEXT는 `json_put_string` |
| `void json_put_row(Buf *b, const Value *fields, int field_count)` | `[1,"Kim",20]` 한 줄을 붙인다 |

쓰기 함수가 값을 돌려주지 않는 이유: 호출마다 검사하면 코드가 검사로 덮인다. 다 쓴 뒤 `failed`를 한 번만 본다.

### http

```c
typedef struct {
    char method[8];                // "POST"
    char path[64];                 // "/query"
    char body[MAX_BODY_LEN + 1];   // '\0' 으로 끝낸다
    int  body_len;
} HttpRequest;
```

| 함수 | 하는 일 |
|---|---|
| `int http_read_request(int fd, HttpRequest *req)` | 헤더 끝(`\r\n\r\n`)까지 읽고, `Content-Length`만큼 본문을 읽는다. 성공하면 0, 실패하면 보낼 상태 코드(400, 413)를 돌려준다 |
| `int http_write_response(int fd, int status, const char *body, size_t body_len)` | 상태 줄 + 헤더 3개 + 본문을 쓴다. 다 썼으면 0, 아니면 -1 |

- `read`는 요청을 한 번에 다 주지 않을 수 있다. 헤더 끝이 보일 때까지, 본문이 다 찰 때까지 반복해서 읽는다
- `Content-Length`가 없으면 본문 길이는 0이다
- 쓰기는 `send(..., MSG_NOSIGNAL)`을 쓴다. 클라이언트가 먼저 끊었을 때 `SIGPIPE`로 서버 전체가 죽지 않게 한다

### threadpool

```c
// 워커가 연결 하나를 받아서 하는 일. fd 를 닫는 것까지 이 함수의 책임이다.
typedef void (*JobFn)(int client_fd, void *ctx);

typedef struct {
    pthread_t       threads[MAX_WORKERS];
    int             thread_count;

    int             queue[QUEUE_CAP];   // 기다리는 연결의 fd. 원형 큐
    int             head;               // 다음에 꺼낼 자리
    int             count;              // 들어 있는 개수

    pthread_mutex_t lock;               // queue, head, count, stopping 을 지킨다
    pthread_cond_t  not_empty;          // 큐에 일이 들어왔거나 멈추라는 신호
    int             stopping;

    JobFn           fn;
    void           *ctx;
} ThreadPool;
```

| 함수 | 하는 일 |
|---|---|
| `int threadpool_start(ThreadPool *p, int thread_count, JobFn fn, void *ctx)` | 락과 조건 변수를 만들고 워커를 띄운다. 성공 0, 실패 -1 |
| `int threadpool_submit(ThreadPool *p, int client_fd)` | 큐에 넣고 워커 하나를 깨운다. 큐가 꽉 찼으면 넣지 않고 -1 (부른 쪽이 503을 보내고 닫는다) |
| `void threadpool_stop(ThreadPool *p)` | `stopping = 1`로 두고 모두 깨운 뒤 `pthread_join`으로 기다린다. 큐에 남은 fd는 닫는다 |

워커의 반복: 락을 잡는다 → 큐가 비어 있고 `stopping`이 아니면 `pthread_cond_wait` (**`while`로 감싼다**) → `stopping`이면 끝 → fd 하나를 꺼낸다 → 락을 푼다 → `fn(fd, ctx)`.

큐에는 fd(정수)만 넣는다. 작업마다 `malloc`할 것이 없고, 큐가 고정 배열이라 한도가 눈에 보인다.

### server

```c
typedef struct {
    Db             *db;
    pthread_mutex_t db_lock;     // execute 호출을 한 번에 한 스레드로 줄인다
    ThreadPool      pool;
    int             listen_fd;
} Server;
```

| 함수 | 하는 일 |
|---|---|
| `int server_handle_sql(Server *s, const char *sql, Buf *out)` | SQL 하나를 실행해 JSON을 `out`에 쓰고 상태 코드를 돌려준다. 소켓을 쓰지 않아서 단위 테스트가 쉽다 |
| `void server_handle_client(int client_fd, void *ctx)` | `JobFn`. 요청 읽기 → 경로·메서드 검사 → `server_handle_sql` → 응답 쓰기 → `close` |
| `int server_run(Db *db, int port, int thread_count)` | 소켓을 열고(`socket` → `setsockopt(SO_REUSEADDR)` → `bind` → `listen`) 풀을 띄운 뒤 `accept`를 반복한다. 성공 0, 실패 -1 |

`server_handle_sql`의 흐름:
1. `tokenize_next`로 문장 하나를 읽는다. 토큰이 `EOF` 하나뿐이면 400(빈 본문)
2. 한 번 더 `tokenize_next`를 불러 남은 문장이 있으면 400(문장은 하나만)
3. `parse`
4. `db_lock`을 잡고 `execute`. 행 콜백은 `json_put_row`로 `out`에 쌓는다. INSERT였으면 락을 풀기 전에 `fflush`한다
5. 락을 풀고 `statement_free`
6. `out->failed`이면 500, 아니면 `DbStatus`를 상태 코드로 바꾼다

- 연결을 받으면 `SO_RCVTIMEO`를 5초로 건다. 아무것도 안 보내는 클라이언트가 워커를 영원히 붙잡지 못하게 한다
- 오류가 나면 그때까지 `out`에 쌓은 행은 버리고 오류 JSON만 보낸다

### 종료

`storage_append`는 `fflush`를 하지 않는다. 그래서 `Ctrl+C`로 그냥 죽으면 버퍼에 있던 INSERT가 사라진다. 두 가지로 막는다.

- INSERT가 성공하면 락 안에서 `fflush(table->fp)`를 한다 (위 4번). 200을 받은 INSERT는 파일에 있다
- `main`은 스레드를 만들기 전에 `SIGINT`·`SIGTERM`을 막아 두고(`pthread_sigmask`), `server_run`은 별도 스레드에서 돌린다. `main`은 `sigwait`로 신호를 기다렸다가 `shutdown(listen_fd)` → `threadpool_stop` → `db_close` 순서로 끝낸다. 신호 핸들러와 전역 변수가 필요 없다

## 한도

| 항목 | 상수 | 값 |
|---|---|---|
| 요청 본문 | `MAX_BODY_LEN` | 8192바이트 |
| 요청 헤더 전체 | `MAX_HEADER_LEN` | 4096바이트 |
| 응답 본문 | `MAX_RESPONSE_LEN` | 64MB |
| 워커 수 | `MAX_WORKERS` | 16 |
| 작업 큐 | `QUEUE_CAP` | 64 |
| 읽기 제한 시간 | | 5초 |

## 구현 규칙 (Phase 1에서 바뀌는 것)

- `malloc`을 쓰는 곳에 `Buf`가 추가된다. 그 밖에는 여전히 고정 배열이다
- 전역 변수와 static 지역 상태는 여전히 쓰지 않는다. 서버의 상태는 `Server`에만 둔다
- 락을 잡은 채로 소켓을 읽거나 쓰지 않는다. 느린 클라이언트 하나가 DB 전체를 멈추게 하면 안 된다
- 락을 잡고 빠져나가는 모든 길에서 락을 푼다
- 받은 fd는 어떤 길로 끝나든 한 번만 닫는다

## 모듈 순서와 통과 기준

| 순서 | 모듈 | 통과 기준 |
|---|---|---|
| 1 | `json` | `O"Brien\` → `"O\"Brien\\"`. 행 2개 → `[1,"Kim",20],[2,"Lee",31]`. 한도를 넘기면 `failed`가 1이고 죽지 않음 |
| 2 | `http` | `socketpair`로 요청을 두 조각에 나눠 보내도 본문을 다 읽음. 본문 9000바이트 → 413. 응답의 `Content-Length`가 본문 길이와 같음 |
| 3 | `server` (스레드 없이) | `accept` 반복에서 `server_handle_client`를 바로 부른다. `curl -d "SELECT * FROM users" localhost:8080/query`가 JSON을 돌려줌 |
| 4 | `threadpool` | 작업 100개를 넣으면 100번 실행됨. 큐를 채우면 -1. `threadpool_stop` 뒤에 누수 없음 |
| 5 | `server` + 풀 + 락 | 동시에 INSERT 200개를 보낸 뒤 `row_count`가 200이고 id가 겹치지 않음. 락을 빼면 이 테스트가 깨지는 것을 직접 확인 |
| 6 | 종료 | `Ctrl+C` 뒤 다시 켜서 SELECT하면 200을 받은 INSERT가 모두 있음 |

서버와 `curl`은 같은 컨테이너 안에서 돌린다 (`compose.yaml`에 포트 연결이 없다).

```sh
docker compose run --rm dev bash -c "make && (./build/mini-dbms --server 8080 &) && sleep 1 && curl -s -d \"SELECT * FROM users\" localhost:8080/query"
```

## 꼭 테스트할 엣지 케이스

| 입력 | 기대 결과 |
|---|---|
| 본문이 비어 있음, 공백만, `;`만 | 400 |
| `SELECT ...; SELECT ...` | 400 (문장은 하나만) |
| `GET /query`, `POST /nope` | 405, 404 |
| `Content-Length`가 실제 본문보다 큼 | 5초 뒤 400. 워커가 풀려남 |
| 요청 줄이 깨짐 (`hello\r\n\r\n`) | 400. 죽지 않음 |
| 응답을 읽지 않고 끊는 클라이언트 | 서버가 죽지 않음 (`SIGPIPE`) |
| 값에 `"`, `\`, 한글이 든 행 SELECT | 올바른 JSON (`test_json.c`에서 기대 문자열과 비교) |
| 0행 SELECT | `{"ok":true,"rows":[],"row_count":0}` |
| 없는 테이블 | 404 + 엔진의 메시지 |
| 동시에 같은 테이블을 처음 여는 요청 여러 개 | `table_count`가 1 |
| 동시 INSERT 200개 | id 1~200이 한 번씩 |
| 큐가 꽉 찬 상태에서 새 연결 | 503 |

## 팀이 확정할 것

초안에서 임의로 정한 값이다. 바꾸면 위 내용도 같이 고친다.

1. **행을 배열로 보낸다** (`[1,"Kim",20]`). 객체(`{"id":1,"name":"Kim"}`)로 하려면 `RowCallback`에 컬럼 이름을 넘기도록 시그니처를 바꿔야 한다
2. **요청 하나에 문장 하나.** 여러 문장을 허용하면 중간에 실패했을 때 응답 모양을 따로 정해야 한다
3. **락은 mutex 하나.** `rwlock`으로 SELECT끼리 동시에 돌리려면 `storage_scan`의 `fflush(t->fp)`가 읽기 락 안에서 불리는 문제를 먼저 풀어야 한다
4. **INSERT마다 `fflush`.** 안전하지만 INSERT가 느려진다. 100만 건 넣기는 CLI로 한다고 가정했다
5. **큐가 꽉 차면 503.** accept 스레드가 자리가 날 때까지 기다리는 방법도 있다
6. **종료 처리(모듈 6)** 를 할 것인가. 시간이 모자라면 4번의 `fflush`만으로도 데이터는 지켜진다
7. 응답 한도 64MB, 워커 기본 4개, 큐 64칸
