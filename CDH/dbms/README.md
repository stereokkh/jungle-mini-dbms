# mini-dbms API 서버 (CDH)

루트 `src/` 의 DB 엔진(Phase 1 SQL 처리기 + Phase 2 B+ 트리)을 HTTP API 로 감싸는 서버.

## 로드맵

| 단계 | 상태 | 끝나면 되는 것 |
|---|---|---|
| 1. 베이스 서버 | ✅ | `GET /health` 응답. 잘못된 요청에 400/404/405/413 |
| 2. SQL API | ⏳ | `POST /query` 에 SQL 을 보내면 엔진이 실행하고 JSON 으로 결과 |
| 3. 스레드 풀 | ⏳ | worker N 개가 요청을 병렬 처리 + 락으로 데이터 보호 |
| 4. 마감 | ⏳ | 안전한 종료, 성능 비교 |

## 파일

```
server/main.c    소켓 열기, accept 반복, 라우팅, 핸들러
server/http.c/h  HTTP 요청 읽기·파싱, 응답 보내기
tests/test_http.c  파서 단위 테스트
tests/e2e.sh       서버를 띄우고 curl 로 확인
```

## 실행 (Docker 안에서)

```bash
make && make test && make e2e
make run                       # 다른 터미널에서: curl localhost:8080/health
```
