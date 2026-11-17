#ifndef ERROR_H
#define ERROR_H

// 모든 모듈이 같은 오류 코드를 쓴다.
// 실패를 exit() 로 끝내지 않고 값으로 돌려줘야 Phase 3 서버가 요청 하나의 실패로 죽지 않는다.
typedef enum {
    DB_OK = 0,
    DB_ERR_SYNTAX,        // 토크나이저·파서: 모양이 틀림
    DB_ERR_TOO_LONG,      // 이름·문자열·개수가 한도를 넘음
    DB_ERR_NO_TABLE,      // 스키마 파일 없음
    DB_ERR_SCHEMA,        // 스키마 파일 내용이 잘못됨 (첫 컬럼이 id,INT 가 아님 등)
    DB_ERR_NO_COLUMN,     // 스키마에 없는 컬럼
    DB_ERR_COLUMN_COUNT,  // 값 개수 불일치
    DB_ERR_TYPE,          // 타입 불일치
    DB_ERR_IO             // 파일 열기·읽기·쓰기 실패
} DbStatus;

// 오류 내용은 호출한 쪽이 준비한 구조체에 채운다.
// 전역 errno 같은 방식은 여러 스레드가 함께 쓸 수 없기 때문이다.
typedef struct {
    DbStatus code;
    int      pos;           // SQL 문자열에서 몇 번째 글자인지 (모르면 -1)
    char     message[256];
} DbError;

// err 를 채우고 code 를 그대로 돌려준다. `return error_set(...)` 한 줄로 빠져나가기 위한 모양이다.
// fmt 뒤는 printf 와 같은 방식으로 쓴다. SQL 위치와 상관없는 오류는 pos 에 -1 을 넘긴다.
// 메시지 앞머리는 여기서 붙인다:
//   DB_ERR_SYNTAX → "Syntax error at position N: ..."   그 밖 → "Error: ..."
// 모든 모듈이 이 함수 하나로 오류를 만들어서 메시지 형식이 한 곳에서 정해진다.
// __attribute__ 는 fmt 와 뒤의 인자가 맞는지 컴파일러가 printf 처럼 검사하게 한다.
DbStatus error_set(DbError *err, DbStatus code, int pos, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

#endif
