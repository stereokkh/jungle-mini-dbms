#ifndef QUERY_H
#define QUERY_H

#include "db.h"

// SQL 실행기: SQL 문자열을 엔진(루트 src/)으로 실행하고, 결과를 JSON 으로 만든다.
// 서버와 엔진 사이의 연결 지점이다. HTTP 도 JSON-RPC 도 모른다 (rpc.c 가 이것을 부른다).
//
// 성공하면 이런 JSON 을 돌려준다 (JSON-RPC 응답의 "result" 자리에 들어간다):
//   {"results":[
//     {"type":"insert","id":3},
//     {"type":"select","columns":["id","name"],"rows":[[1,"Kim"]],"row_count":1}
//   ]}

//
// 여러 스레드가 동시에 불러도 안전하다 (3단계). 문장 하나를 실행하는 동안만 락을 잡는다.
//   - 테이블마다 rwlock: SELECT 는 읽기 락(여럿이 함께), INSERT 는 쓰기 락(혼자)
//   - 테이블 목록 rwlock: 처음 쓰는 테이블을 목록에 등록할 때만 쓰기 락
// 여러 문장을 묶어 보내도 한 덩어리(트랜잭션)는 아니다. 문장 사이에 다른 요청이 끼어들 수 있다.

// 실패 정보
typedef struct {
    int  code;           // JSON-RPC 오류 코드 (rpc.h 의 RPC_SQL_ERROR 등)
    char message[256];   // 엔진이 만든 오류 문장
    int  executed;       // 오류 전에 이미 실행된 문장 수. 그 문장들은 되돌리지 않는다
} QueryError;

// [역할] sql 의 문장들을 차례로 실행하고 결과 JSON 을 만든다.
// [인자] db = 열어 둔 DB, sql = '\0' 으로 끝나는 SQL 문자열, err = 실패 정보를 받을 곳
// [반환] 성공하면 malloc 한 결과 JSON (부른 쪽이 free), 실패하면 NULL (err 가 채워진다)
char *query_run(Db *db, const char *sql, QueryError *err);

#endif
