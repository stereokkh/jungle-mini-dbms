#ifndef RPC_H
#define RPC_H

#include <stddef.h>

#include "db.h"

// JSON-RPC 2.0 처리기. HTTP 본문(JSON) 하나를 받아 응답 JSON 하나를 만든다.
// 규격: https://www.jsonrpc.org/specification
//
// 요청:  {"jsonrpc":"2.0", "method":"query", "params":{"sql":"SELECT * FROM users;"}, "id":1}
// 성공:  {"jsonrpc":"2.0", "result":{...}, "id":1}
// 실패:  {"jsonrpc":"2.0", "error":{"code":-32001, "message":"...", "data":{...}}, "id":1}
//
// id 를 빼고 보내면 "알림(notification)" 이다. 실행은 하지만 응답을 보내지 않는다.
//
// 지원 메서드
//   query  params: {"sql":"..."} 또는 ["..."]  → result: {"results":[문장별 결과...]}
//   ping   params: 없음                         → result: "pong"

// ---- 오류 코드 ----
// 규격이 정한 코드 (-32768 ~ -32000 은 규격 예약 영역)
#define RPC_PARSE_ERROR      -32700   // 본문이 JSON 이 아니다
#define RPC_INVALID_REQUEST  -32600   // JSON 이지만 요청 모양이 아니다 (jsonrpc/method 누락 등)
#define RPC_METHOD_NOT_FOUND -32601   // 없는 메서드
#define RPC_INVALID_PARAMS   -32602   // params 가 틀렸다 (sql 이 없음 등)
#define RPC_INTERNAL_ERROR   -32603   // 서버 내부 오류 (메모리 부족)
// 이 서버가 정한 코드 (-32000 ~ -32099 는 "서버 오류" 용으로 쓰라고 규격이 비워 둔 곳)
#define RPC_SQL_ERROR        -32001   // SQL 문법·컬럼·타입 오류
#define RPC_TABLE_NOT_FOUND  -32002   // 없는 테이블
#define RPC_STORAGE_ERROR    -32003   // 스키마·데이터 파일 문제

// [역할] JSON-RPC 요청 본문 하나를 처리하고 응답 본문을 만든다.
// [인자] body/len = HTTP 본문 ('\0' 으로 끝나지 않아도 된다)
//        *notification = 알림 요청이었으면 1 (이때 응답을 보내지 않는다)
// [반환] malloc 한 응답 JSON (부른 쪽이 free). 알림이면 NULL. 메모리가 모자라도 NULL (*notification = 0)
char *rpc_handle(Db *db, const char *body, size_t len, int *notification);

#endif
