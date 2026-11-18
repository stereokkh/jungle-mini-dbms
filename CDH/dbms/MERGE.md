# 세 PR 통합 기록 (#4 · #5 · #6)

> 통합 뒤 서버 코드는 `CDH/dbms/server/` 에서 **`src/server/`** 로 옮겼다. 이 폴더(`CDH/dbms/`)에는 학습 문서와 테스트만 남는다.
> 빌드: 루트에서 `make server` → `build/dbms-server`. 테스트: `cd CDH/dbms && make test e2e`.

Phase 3 API 서버를 세 사람이 각자 만들었다. 셋을 그대로 다 넣으면 같은 일을 하는 서버가 3개가 된다.
그래서 **PR마다 가장 좋은 부분만 골라서 서버 하나로 합쳤다.**

| PR  | 작성자       | 브랜치                      | 한 줄 요약                                                      |
| --- | --------- | ------------------------ | ----------------------------------------------------------- |
| #4  | 1224kang  | `feat/phase3-api-server` | 루트 `server/` (libmicrohttpd + jansson). **엔진 안에 테이블 락**을 넣음 |
| #5  | CDH       | `CDH/mini-dbms`          | `CDH/dbms/` 독립 서버 (직접 만든 HTTP·JSON-RPC, 스레드 풀, 안전한 종료)      |
| #6  | stereokkh | `feat/kunho-server`      | `src/` 안의 `--server` 모드 (직접 만든 HTTP·JSON). DB 전체에 rwlock 하나 |

## 1. 무엇을 어디서 가져왔나

| 부분 | 가져온 곳 | 이유 | 위치 |
|---|---|---|---|
| 테이블 목록 mutex + 테이블별 rwlock | **#4** | 엔진 안에 있으면 CLI든 서버든 엔진을 쓰기만 하면 보호된다 | `src/db.c`, `src/executor.c` |
| INSERT 직후 `fflush`, 쓰기 실패 후 테이블 잠금(`write_failed`) | **#4** | 응답 전에 데이터가 커널로 내려가고, 반쯤 쓴 파일을 계속 쓰지 않는다 | `src/storage.c` |
| `run_one_sql` (빈 요청·두 문장 이상 거절) | **#4** | `/query` 의 "요청 하나 = 문장 하나" 규칙을 그대로 쓴다 | `src/sql.c` |
| 엔진 단위 테스트 `test_executor`, `make asan` | **#4** | 엔진 락을 검사한다 | `tests/test_executor.c`, `Makefile` |
| 서버 본체: 작업자 N개 + 큐 64칸 + 503 | **#5** | 작업자 수를 바꿀 수 있고 벤치로 측정돼 있다 (#4는 2개 고정, 큐 4칸) | `src/server/pool.c`, `main.c` |
| 안전한 종료 (받은 요청까지 처리, 두 번째 Ctrl+C면 강제 종료) | **#5** | #6에는 없다 | `src/server/main.c` |
| JSON-RPC `/rpc` (여러 문장), e2e·벤치 | **#5** | 기존 기능을 유지한다 | `src/server/rpc.c`, `tests/` |
| 쓰기 우선 rwlock | **#5·#6** | 기본 rwlock은 SELECT가 끊이지 않으면 INSERT가 영원히 못 들어간다. #4 엔진 락에 이 속성을 더했다 | `src/db.c` `open_table` |
| `POST /query` `{"sql"}` API | **#4·#6** 공통 | 팀 둘이 이미 같은 형식을 쓴다. #5 서버에 추가했다 | `src/server/main.c` `handle_query` |
| `/query` 응답 모양 `{"ok":true,...}`, 오류 → 400/404/500 | **#6** | #4 모양에 `ok`가 더해진 것이라 둘 다 읽기 쉽다 | `src/server/query.c` `query_run_one` |
| 스레드 풀 테스트, JSON 테스트 | **#6** | 이 서버의 `pool.c`·`json.c` API로 옮겼다 | `CDH/dbms/tests/test_pool.c`, `test_json.c` |
| 설계 문서 | **#6** | 원안 그대로 참고용으로 둔다 | `docs/phase3-design.md` |

## 2. 뺀 것과 이유

| 뺀 것 | PR | 이유 |
|---|---|---|
| 루트 `server/` (libmicrohttpd + jansson 서버) | #4 | 같은 일을 하는 서버가 둘이 된다. 외부 라이브러리 때문에 Docker 이미지를 다시 빌드해야 한다 |
| `Dockerfile`의 `libmicrohttpd-dev libjansson-dev pkg-config` | #4 | 위 서버를 뺐으니 필요 없다 → 이미지를 다시 빌드하지 않아도 된다 |
| `src/http.c, json.c, server.c, threadpool.c` 와 CLI의 `--server` 모드 | #6 | 같은 일을 하는 서버가 둘이 된다. 또 `src/` 에 있으면 #5 서버와 함수 이름(`http_read_request`)이 겹쳐 링크가 깨졌다 |
| DB 전체 rwlock (`s->db_lock`) | #6 | 테이블별 락(#4)이 더 잘게 나뉘어 다른 테이블 INSERT끼리 기다리지 않는다 |
| `tests/test_json.c`, `tests/test_threadpool.c` (루트) | #6 | 테스트 대상이 빠졌다. 같은 시나리오를 `CDH/dbms/tests/` 로 옮겼다 |
| #5 서버 쪽 락 (`g_catalog`, `g_table_locks`, `lock_for`) | #5 | 엔진(#4)이 같은 락을 잡는다. 두 번 잡으면 느려지기만 한다 |
| #5 서버의 INSERT 뒤 `fflush` | #5 | 엔진(#4)이 쓰기 락 안에서 이미 한다 |

## 3. 순서 (git 기록)

```
CDH/mini-dbms
 ├─ merge feat/phase3-api-server (#4)   충돌: README.md 한 곳 → 두 문단 다 살림
 ├─ merge feat/kunho-server (#6)        충돌: src/main.c include → 둘 다 살림 (이 시점엔 서버 3개 공존, 빌드·테스트 통과)
 └─ 통합 커밋                            중복 서버 제거, 서버 쪽 락 제거, /query 추가, 테스트 옮기기, 문서
```

두 PR을 **merge 로 합쳐서** 원래 커밋과 작성자가 기록에 그대로 남는다.
이 브랜치가 `develop` 에 들어가면 GitHub에서 #4·#6도 "merged" 로 표시된다.

## 4. 합친 뒤 요청 하나의 흐름

```
클라이언트 ── POST /query {"sql":"INSERT ..."} ──▶ main(accept) ── pool_submit ──▶ [큐 64칸]
                                                                         │ 꽉 차면 503
                                         작업자 N개 ◀────────────────────┘
                                           └ handle_query → query_run_one
                                                └ run_one_sql (#4)  한 문장인지 확인 → parse
                                                     └ execute (엔진)
                                                          ├ db_get_table: 목록 mutex (#4)
                                                          ├ 테이블 rwlock: SELECT=읽기, INSERT=쓰기 (#4, 쓰기 우선 #5·#6)
                                                          └ INSERT: 파일 쓰기 + fflush (#4)
                                           ◀ {"ok":true,"inserted_id":3}  (#6 모양)
```

## 5. 검증

| 확인 | 결과 |
|---|---|
| 루트 `make test` (bptree, executor, parser, storage, tokenizer) | 통과 |
| 루트 `make asan` (AddressSanitizer + UBSan) | 통과 |
| `CDH/dbms` `make test` (http, query, rpc, json, pool) | 통과 |
| 같은 단위 테스트를 ThreadSanitizer로 (8 스레드 동시 INSERT/SELECT 포함) | 경고 0개 |
| `CDH/dbms` `make e2e` (31개: /rpc, /query, 동시 INSERT 40개, 안전한 종료, 재시작 후 데이터) | 전부 통과 |
| 루트 `make server` (서버 코드를 `src/server/` 로 옮긴 뒤) | `build/dbms-server` 생성 |

## 6. 남은 일

- `make bench` 로 합치기 전(#5 서버 쪽 락)과 성능을 다시 비교하기. 락을 한 번만 잡으니 같거나 조금 빨라야 한다.
- 이 통합은 #4·#6 작성자와 합의가 필요하다. 각 PR의 원래 코드는 해당 브랜치에 그대로 남아 있다.
