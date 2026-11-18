#ifndef QUERY_H
#define QUERY_H

#include "db.h"

// SQL API: SQL 문자열을 엔진(루트 src/)으로 실행하고, 결과를 JSON 문자열로 만든다.
// 서버(HTTP)와 엔진(DB) 사이의 연결 지점이다. HTTP 를 모르고, 엔진은 이 파일만 부른다.
//
// 성공 예:
//   {"ok":true,"results":[
//     {"type":"insert","id":3},
//     {"type":"select","columns":["id","name"],"rows":[[1,"Kim"]],"row_count":1}
//   ]}
// 실패 예:
//   {"ok":false,"error":"Syntax error at position 7: ...","executed":1}
//   (executed = 오류 전에 이미 실행된 문장 수. 그 문장들은 되돌리지 않는다)

// [역할] sql 의 문장들을 차례로 실행하고, 응답 본문 JSON 을 만들어 돌려준다.
// [인자] db = 열어 둔 DB, sql = '\0' 으로 끝나는 SQL 문자열, *status = HTTP 상태 코드를 받을 곳
// [반환] malloc 한 JSON 문자열 (부른 쪽이 free). 메모리가 모자라면 NULL (*status = 500)
char *query_run(Db *db, const char *sql, int *status);

#endif
