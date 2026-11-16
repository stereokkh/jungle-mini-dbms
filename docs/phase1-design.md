# Phase 1 설계 메모: SQL 처리기

이 문서는 팀이 모임에서 확정한 설계다. 구현은 이 문서를 그대로 따른다.
구조체와 함수 시그니처를 바꿔야 한다고 판단하면, 코드를 쓰기 전에 이유를 설명하고 확인을 받는다.

## 과제 배경

크래프톤 정글 "미니 DBMS 구현" 과제. 세 단계로 만들고, 뒤 단계는 앞 단계 결과물을 그대로 재사용한다.

| 단계 | 내용 |
|---|---|
| Phase 1 (이 문서) | SQL 파일을 CLI로 받아 INSERT / SELECT를 처리하는 파일 기반 SQL 처리기 |
| Phase 2 | 레코드에 id를 자동으로 붙이고, 메모리 기반 B+ 트리에 `id → 파일 offset`을 넣어 `WHERE id = ?`를 빠르게 한다. 100만 건 이상 삽입 후 속도 비교 |
| Phase 3 | Phase 1+2를 내부 엔진으로 쓰는 HTTP API 서버. 스레드 풀로 요청을 병렬 처리 |

그래서 Phase 1에서 미리 지키는 것:
- 전역 변수를 쓰지 않는다 (Phase 3에서 여러 스레드가 함께 쓴다)
- SELECT 결과를 바로 출력하지 않고 콜백으로 돌려준다 (Phase 3에서 JSON으로 보낸다)
- 저장소가 레코드의 시작 offset을 돌려준다 (Phase 2에서 B+ 트리에 넣는다)
- 문장을 하나씩 처리한다 (Phase 2에서 100만 문장을 넣는다)

## 범위와 규칙

- 지원하는 문장: `INSERT INTO 테이블 VALUES (값, ...)`, `SELECT * 또는 컬럼들 FROM 테이블 [WHERE 컬럼 = 값]`
- 타입은 `INT`와 `TEXT` 두 가지뿐이다
- 키워드는 대소문자를 구분하지 않는다. 테이블·컬럼 이름과 문자열 값은 구분한다
- 음수는 토크나이저가 `-` 바로 뒤에 숫자가 오면 `TOK_INTEGER` 하나로 읽는다
- 문자열은 작은따옴표로 감싼다. 안의 `''`는 `'` 하나다. 토큰에는 따옴표를 떼고 바꾼 값을 복사해 담는다
- 문장은 `;`로 구분한다. 마지막 문장의 `;`만 생략할 수 있다. `;`만 있는 빈 문장은 건너뛴다
- id는 엔진이 붙인다. 모든 테이블의 스키마 첫 컬럼은 `id,INT`여야 한다. INSERT에는 id를 뺀 나머지 값만 쓴다. 사용자가 id까지 넣으면(값 개수가 스키마 컬럼 수와 같으면) 오류다
- 다음 id는 테이블을 처음 열 때 데이터 파일을 한 번 훑어 `가장 큰 id + 1`로 정한다. 파일이 없거나 비어 있으면 1이다

## 파일 구성

저장소에 이미 있는 구성(Makefile, Dockerfile, compose.yaml)을 그대로 쓴다. Makefile은 `src/*.c`, `src/*.h`, `tests/test_*.c`를 자동으로 포함하므로 고치지 않는다. 헤더는 `include/`가 아니라 `src/`에 둔다.

```
jungle-mini-dbms/
├── Makefile      (기존 것 그대로. make / make test / make asan / make clean)
├── docs/         phase1-design.md
├── src/          error.h  token.h  statement.h  schema.h  db.h  storage.h  executor.h
│                 tokenizer.c  parser.c  schema.c  db.c  storage.c  executor.c  main.c
├── tests/        test_tokenizer.c  test_parser.c  test_storage.c  test_executor.c
└── data/         users.schema  (users.csv 는 실행하면 생긴다)
```

- 실행 파일은 `build/mini-dbms`다
- `src/main.c`에는 임시 REPL 뼈대가 있다. `main.c` 모듈을 만들 때 이 문서의 CLI로 교체한다
- `data/*.csv`는 실행하면 생기는 파일이므로 `.gitignore`에 추가한다. `data/users.schema`는 커밋한다

## 오류

```c
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

typedef struct {
    DbStatus code;
    int      pos;           // SQL 문자열에서 몇 번째 글자인지 (모르면 -1)
    char     message[256];
} DbError;
```

## 토큰

```c
typedef enum {
    TOK_INSERT, TOK_INTO, TOK_VALUES, TOK_SELECT, TOK_FROM, TOK_WHERE,
    TOK_IDENTIFIER, TOK_INTEGER, TOK_STRING,
    TOK_LPAREN, TOK_RPAREN, TOK_COMMA, TOK_SEMICOLON, TOK_STAR, TOK_EQ,
    TOK_EOF
} TokenType;

typedef struct {
    TokenType type;
    char      text[256];    // 이름 또는 문자열 값 (따옴표 뗀 것, '' 는 ' 로 바꾼 것)
    long long int_value;    // TOK_INTEGER 일 때
    int       pos;          // 원문에서 시작 위치
} Token;
```

## 문장

```c
typedef enum { STMT_SELECT, STMT_INSERT } StmtType;

typedef struct {
    enum { VAL_INT, VAL_STR } type;   // 꼬리표: 지금 뭐가 들었는지
    union {
        long long i;
        char      s[256];
    };
} Value;

typedef struct {
    char  column[64];
    char  op;             // '=' 만 지원
    Value value;
} Where;

typedef struct {
    StmtType type;
    char     table[64];

    // SELECT: 가져올 컬럼
    int      select_all;          // * 이면 1
    char     columns[16][64];
    int      column_count;

    // INSERT: 넣을 값 (id 를 뺀 나머지, 순서 = 스키마 컬럼 순서)
    Value    values[16];
    int      value_count;

    // SELECT: 조건
    int      has_where;
    Where    where;
} Statement;
```

## 스키마와 DB

```c
typedef enum { COL_INT, COL_TEXT } ColumnType;

typedef struct {
    char       name[64];
    ColumnType type;
} Column;

typedef struct {
    Column cols[16];
    int    count;
} Schema;

// 열어 둔 테이블 하나
typedef struct {
    char      name[64];
    Schema    schema;
    FILE     *fp;         // "ab" 로 열어 둔 데이터 파일. INSERT 마다 열고 닫지 않는다
    long long next_id;    // 다음 INSERT 에 붙일 id
} Table;

// 실행 중인 DB 하나. 전역 변수 대신 이것을 함수에 넘긴다
typedef struct {
    char  data_dir[256];
    Table tables[8];
    int   table_count;
} Db;

// execute 가 돌려주는 결과
typedef struct {
    long long inserted_id;   // INSERT 일 때 붙인 id
    int       row_count;     // SELECT 일 때 찾은 행 수
} ExecResult;
```

## 함수

| 함수 | 하는 일 |
|---|---|
| `DbStatus tokenize_next(const char *sql, int *pos, Token *out, int max, int *count, DbError *err)` | `*pos`부터 문장 하나(`;`까지 또는 입력 끝까지)를 토큰으로 만들고 `*pos`를 그 뒤로 옮긴다. 마지막에 `TOK_EOF`를 붙인다. 토큰의 `pos`는 입력 전체 기준 위치다 |
| `DbStatus parse(const Token *toks, int count, Statement *stmt, DbError *err)` | 문장 하나의 토큰을 `Statement`로 바꾼다. 끝은 `[;] EOF`여야 한다 |
| `DbStatus schema_load(const char *path, Schema *out, DbError *err)` | 스키마 파일 읽기. 첫 컬럼이 `id,INT`가 아니면 `DB_ERR_SCHEMA` |
| `DbStatus db_open(const char *data_dir, Db *db, DbError *err)` | DB 준비 (테이블은 처음 쓸 때 연다) |
| `DbStatus db_get_table(Db *db, const char *name, Table **out, DbError *err)` | 이미 열었으면 그것을 돌려준다. 처음이면 스키마를 읽고, 데이터 파일을 훑어 `next_id`를 정하고, 파일을 `"ab"`로 열어 둔다 |
| `void db_close(Db *db)` | 열어 둔 파일을 모두 닫는다 |
| `DbStatus storage_append(Table *t, const char *line, long *offset, DbError *err)` | CSV 한 줄을 파일 끝에 붙이고 그 줄의 시작 offset을 돌려준다 |
| `DbStatus storage_scan(Db *db, Table *t, ScanCallback cb, void *ctx, DbError *err)` | 먼저 `fflush(t->fp)`를 한 뒤, 읽기용으로 따로 열어 처음부터 한 줄씩 읽고 콜백을 부른다 |
| `DbStatus execute(Db *db, const Statement *stmt, RowCallback on_row, void *ctx, ExecResult *result, DbError *err)` | 스키마 검사 후 INSERT는 id를 붙여 저장, SELECT는 찾은 행을 콜백으로 전달 |

```c
// 저장소가 읽은 줄 하나마다 불린다. offset 은 그 줄의 시작 위치 (Phase 2 에서 B+ 트리에 넣는다).
// 0 이 아닌 값을 돌려주면 읽기를 멈춘다.
typedef int (*ScanCallback)(const Value *fields, int field_count, long offset, void *ctx);

// SELECT 가 찾은 행 하나마다 불린다. CLI 는 여기서 출력하고, Phase 3 서버는 여기서 JSON 을 만든다.
typedef void (*RowCallback)(const Value *fields, int field_count, void *ctx);
```

**실행기의 검사 순서**
1. 테이블이 있는가 → 없으면 `DB_ERR_NO_TABLE`
2. INSERT: 값 개수가 `스키마 컬럼 수 − 1`인가 → 아니면 `DB_ERR_COLUMN_COUNT`. 값 개수가 스키마 컬럼 수와 같으면 메시지에 "id는 자동으로 붙는다"를 넣는다
3. INSERT: 값의 종류가 컬럼 타입과 맞는가 (`COL_INT` ↔ `VAL_INT`, `COL_TEXT` ↔ `VAL_STR`) → 아니면 `DB_ERR_TYPE`
4. INSERT: TEXT 값에 줄바꿈이 있으면 `DB_ERR_TYPE`
5. SELECT: 고른 컬럼과 WHERE 컬럼이 스키마에 있는가 → 없으면 `DB_ERR_NO_COLUMN`
6. SELECT: WHERE 값의 종류가 그 컬럼 타입과 맞는가 → 아니면 `DB_ERR_TYPE`

비교는 INT는 숫자로, TEXT는 글자 그대로(대소문자 구분) 한다. 컬럼을 골라 SELECT하면 콜백에는 고른 컬럼만 고른 순서로 넘긴다.

## 오류 메시지 형식

메시지는 영어로 통일한다.

| 종류 | 형식 |
|---|---|
| 토크나이저·파서 | `Syntax error at position 7: expected '*' or column name, but got 'FROM'` |
| 실행기 | `Error: table 'userz' does not exist` |
| | `Error: expected 2 values (name, age), but got 3 (id is assigned automatically)` |
| | `Error: column 'age' expects INT, but got TEXT` |
| | `Error: column 'nope' does not exist in table 'users'` |

## 파일 형식

| 파일 | 형식 | 예시 |
|---|---|---|
| `data/users.schema` | 한 줄에 `이름,타입`. 줄 순서 = 컬럼 순서 | `id,INT` / `name,TEXT` / `age,INT` |
| `data/users.csv` | 한 줄 = 레코드 하나. TEXT는 큰따옴표로 감싸고 안의 `"`는 `""` | `1,"Kim, Jr",20` |

## CLI

```
./build/mini-dbms queries.sql
```

| 상황 | 출력 |
|---|---|
| INSERT 성공 | `OK (id=1)` |
| SELECT 결과 | 행마다 한 줄: `1,"Kim, Jr",20` + 마지막에 `(1 rows)` |
| 오류 | 위 형식의 메시지를 stderr에 출력하고, 그 문장에서 멈추고 종료 코드 1 |

`main.c`의 흐름: SQL 파일 전체를 메모리로 읽는다 → `db_open` → `tokenize_next`로 문장 하나를 토큰으로 → 토큰이 `EOF` 하나뿐이면 끝, `;`뿐이면 건너뜀 → `parse` → `execute` → 결과 출력 → 반복 → `db_close`.

예시 입력 `queries.sql`:

```sql
INSERT INTO users VALUES ('Kim, Jr', 20);
INSERT INTO users VALUES ('Lee', 31);
SELECT * FROM users WHERE id = 1;
```

예상 출력:

```
OK (id=1)
OK (id=2)
1,"Kim, Jr",20
(1 rows)
```

## 한도

| 항목      | 값                |
| ------- | ---------------- |
| 이름 길이   | 63자              |
| 문자열 길이  | 255자             |
| 컬럼·값 개수 | 16개              |
| 한 줄 길이  | 8192바이트 |
| 문장 하나의 토큰 수 | 64개 |
| 열어 둘 테이블 수 | 8개 |

- 길이는 글자 수가 아니라 바이트로 센다. 한글은 한 글자가 3바이트라서 문자열은 85자, 이름은 21자까지 들어간다
- 토큰 64개에는 마지막 `TOK_EOF`가 들어간다. 그래서 문장 하나의 실제 토큰은 63개까지다. `token.h`의 `MAX_TOKENS`(64)를 토큰 배열 크기로 쓴다
- 한 줄 8192바이트에는 끝의 줄바꿈이 들어간다. `storage.h`의 `MAX_LINE_LEN`(8192)을 쓰고, 읽는 버퍼는 `char line[MAX_LINE_LEN + 1]`로 잡는다
- 63, 255, 16, 8은 구조체의 배열 크기(`[64]`, `[256]`, `[16]`, `[8]`)에서 나오므로 따로 상수를 두지 않는다

한도를 넘으면 `DB_ERR_TOO_LONG`을 돌려준다. 프로그램이 죽으면 안 된다.

## 구현 규칙

- 환경: Docker 컨테이너(Debian 12, gcc 12). 호스트(Windows)에는 gcc가 없으므로 모든 빌드와 테스트는 `docker compose run --rm dev make test`처럼 컨테이너에서 실행한다
- C11, 외부 라이브러리 없이 libc와 POSIX만 쓴다. 기존 Makefile의 옵션(`-std=c11 -D_GNU_SOURCE -Wall -Wextra`)에서 경고가 하나도 없어야 한다
- 모듈을 끝낼 때마다 `make test`와 `make asan`을 모두 통과시킨다
- 이 문서의 구조체와 함수 시그니처를 바꾸지 않는다. 바꿔야 한다고 판단하면 코드를 쓰기 전에 이유를 설명하고 확인을 받는다
- 이 문서에 없는 내용을 추측해서 채우지 않는다. 모호하면 먼저 질문한다
- 짧고 단순하게 쓴다. 함수 하나는 40줄 이내로, 한 가지 일만 한다
- 범위 밖 기능을 만들지 않는다 (UPDATE, DELETE, JOIN, AND/OR, CREATE TABLE, 인덱스)
- 버퍼에 복사하기 전에 반드시 길이를 검사하고, 넘으면 `DB_ERR_TOO_LONG`을 돌려준다
- 전역 변수와 static 지역 상태를 쓰지 않는다. 상태는 `Db` 구조체에만 둔다
- 어떤 입력에도 프로그램이 죽으면 안 된다. 모든 실패는 `DbStatus`와 `DbError`로 돌려준다
- 실패해서 빠져나갈 때 열어 둔 파일을 닫는다
- 주석은 한국어로 쓰고, "무엇을 하는지"가 아니라 "왜 이렇게 하는지"를 설명한다
- 단위 테스트는 assert 기반으로 직접 쓴다 (외부 프레임워크 없음). 정상 케이스와 오류 케이스를 모두 넣고, 테스트는 임시 디렉터리의 파일을 쓴다
- 모듈을 끝낼 때마다 (1) 흐름을 5줄 이내로 요약하고 (2) 이 문서에서 애매해서 직접 정한 부분을 목록으로 알린다

## 모듈 순서와 통과 기준

모듈을 하나씩 만든다. 한 모듈이 끝나면 `make test`와 `make asan`을 통과시키고 멈춘다. 다음 모듈은 요청받은 뒤에 만든다.

| 순서 | 모듈 | 통과 기준 |
|---|---|---|
| 1 | 헤더 전체 (`src/*.h`) | 이 문서의 구조체·함수 선언과 같고 경고 없이 컴파일됨 |
| 2 | 토크나이저 | `INSERT INTO users VALUES ('Kim, Jr', 20);` → 토큰 10개 + EOF. 문장 두 개를 이어 붙이면 두 번 호출해서 각각 나옴 |
| 3 | 파서 | 위 문장 → type=INSERT, table="users", values=[{STR,"Kim, Jr"}, {INT,20}] / `SELECT FROM users` → `DB_ERR_SYNTAX` |
| 4 | 스키마·DB·저장소 | 빈 테이블의 `next_id`는 1. `1,"Kim, Jr",20`을 붙이면 offset 0, 한 줄 더 붙이면 offset 15. 다시 열면 `next_id`가 이어짐 |
| 5 | 실행기 | INSERT 두 번 → id 1, 2 / `VALUES (1, 'Kim', 20)` → `DB_ERR_COLUMN_COUNT` / 없는 테이블 → `DB_ERR_NO_TABLE` |
| 6 | `main.c` | 예시 `queries.sql`을 실행하면 예상 출력과 같음. 한 번 더 실행하면 id가 3, 4로 이어짐 |

## 꼭 테스트할 엣지 케이스

| 입력 | 기대 결과 |
|---|---|
| `'O''Brien'` | 저장되고, SELECT하면 `O'Brien` |
| `select * from users` (소문자 키워드) | 정상 동작 |
| `'Kim` (닫히지 않은 따옴표) | 토크나이저 오류 + 위치 |
| 값 개수 하나 부족 | `DB_ERR_COLUMN_COUNT` |
| id까지 넣은 INSERT | `DB_ERR_COLUMN_COUNT` + "id is assigned automatically" |
| `SELECT * FROM users abc` | 파서 오류 |
| 300자 문자열, 값 17개 | `DB_ERR_TOO_LONG`. 죽지 않음 |
| `'He said "hi"'` 넣고 SELECT | 큰따옴표가 그대로 돌아옴 |
| 빈 문자열 `''` | 빈 문자열로 저장 |
| 음수 `-5` | INTEGER -5 |
| `99999999999999999999` | 오류: 숫자가 범위를 넘음 |
| 빈 파일, 공백만, `;`만 | 아무 일 없이 종료 코드 0 |
| 데이터 파일이 없을 때 SELECT | `(0 rows)` |
| `WHERE name = 'Kim'` | 선형 탐색으로 찾음 |
| `WHERE id = 'abc'` | `DB_ERR_TYPE` |
| `WHERE nope = 1` | `DB_ERR_NO_COLUMN` |
| 한글 값 `'김건호'` | 저장되고 그대로 돌아옴 |
| 프로그램을 두 번 실행 | id가 이어서 붙음 |
