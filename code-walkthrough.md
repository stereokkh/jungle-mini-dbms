# 예시 SQL 한 개로 코드 따라가기

[code-review.md](code-review.md) 가 "파일·함수 사전"이라면, 이 문서는 **SQL 한 파일을 넣었을 때 코드가 실제로 어떤 순서로 불리는지**를 줄 번호와 함께 따라가는 읽기 순서다.
줄 번호 링크를 눌러 코드를 옆에 띄워 놓고 위에서부터 읽으면 된다.

## 0. 준비: 예시 입력

`data/users.schema` (이미 있음)

```
id,INT
name,TEXT
age,INT
```

`example.sql` (직접 만들어서 실행)

```sql
INSERT INTO users VALUES ('kim', 20);
INSERT INTO users VALUES ('lee', 17);
SELECT name, age FROM users WHERE age >= 18 AND NOT name = 'lee';
```

이 세 문장이면 **INSERT 경로, 테이블 캐시, 토크나이저의 모든 토큰 종류, WHERE 트리(AND·NOT·비교), SELECT 경로**를 전부 지나간다.

실행 (컨테이너 안에서, `data/users.csv` 가 없는 상태라고 가정)

```bash
make && ./build/mini-dbms example.sql
```

예상 출력 (코드를 따라가서 얻은 결과)

```
OK (id=1)
OK (id=2)
"kim",20
(1 rows)
```

실행 뒤 `data/users.csv`

```
1,"kim",20
2,"lee",17
```

---

## 1. 프로그램 시작 — main.c

| 순서  | 위치                                                                    | 하는 일                                                                        |
| --- | --------------------------------------------------------------------- | --------------------------------------------------------------------------- |
| 1   | [main.c:116](src/main.c:116) `main`                                   | 시작점                                                                         |
| 2   | [main.c:121](src/main.c:121)                                          | 인자가 `example.sql` 하나인지 확인                                                   |
| 3   | [main.c:125](src/main.c:125) → [main.c:15](src/main.c:15) `read_file` | 파일 전체를 `malloc` 한 문자열 하나로 읽는다. 끝에 `'\0'` 을 붙인다 ([main.c:33](src/main.c:33)) |
| 4   | [main.c:131](src/main.c:131) → [db.c:9](src/db.c:9) `db_open`         | `db->data_dir = "data"` 저장, `table_count = 0`. **테이블은 아직 안 연다**             |
| 5   | [main.c:133](src/main.c:133) → [main.c:92](src/main.c:92) `run_sql`   | 여기서부터 문장 반복                                                                 |

`run_sql` 의 반복 구조 ([main.c:98-113](src/main.c:98))

```
for (;;) {
    tokenize_next(...)        ← 문장 하나만 토큰으로 (pos 가 다음 문장 시작으로 이동)
    count == 1 이면 끝         ← EOF 토큰 하나뿐 = 더 읽을 문장 없음
    첫 토큰이 ; 이면 건너뜀     ← 빈 문장 ";;"
    run_statement(...)        ← 파싱 + 실행 + 출력
}
```

> 포인트: 파일 전체를 한 번에 토큰으로 만들지 않는다. `Token toks[64]` ([main.c:94](src/main.c:94)) 배열 하나를 문장마다 재사용한다.

---

## 2. 문장 1: `INSERT INTO users VALUES ('kim', 20);`

### 2-1. 토크나이저 — tokenizer.c

[main.c:99](src/main.c:99) → [tokenizer.c:178](src/tokenizer.c:178) `tokenize_next`

| 순서  | 위치                                                                                           | 하는 일                                                               |
| --- | -------------------------------------------------------------------------------------------- | ------------------------------------------------------------------ |
| 1   | [tokenizer.c:180](src/tokenizer.c:180)                                                       | `p = *pos` 복사본으로 진행 (실패하면 `*pos` 를 안 건드리려고)                        |
| 2   | [tokenizer.c:185](src/tokenizer.c:185)                                                       | 토큰이 64개를 넘는지 매번 검사                                                 |
| 3   | [tokenizer.c:189](src/tokenizer.c:189) → [tokenizer.c:152](src/tokenizer.c:152) `next_token` | 공백을 건너뛰고([154](src/tokenizer.c:154)) **첫 글자 하나만 보고** 어떤 함수로 갈지 정한다 |

`next_token` 의 분기 ([tokenizer.c:162-175](src/tokenizer.c:162))

| 첫 글자       | 가는 곳                                                              | 이 문장에서 만든 토큰                                         |
| ---------- | ----------------------------------------------------------------- | ---------------------------------------------------- |
| `'\0'`     | `TOK_EOF`                                                         | —                                                    |
| 영문자 / `_`  | [read_word](src/tokenizer.c:46) → [word_type](src/tokenizer.c:28) | `INSERT` `INTO` `VALUES` (키워드), `users` (IDENTIFIER) |
| 숫자 / `-숫자` | [read_integer](src/tokenizer.c:68)                                | `20` (`strtoll` 로 변환, [84](src/tokenizer.c:84))      |
| `'`        | [read_string](src/tokenizer.c:96)                                 | `'kim'` → text = `kim` (따옴표 뗌)                       |
| 그 밖        | [read_symbol](src/tokenizer.c:126)                                | `(` `,` `)` `;`                                      |

4. `;` 를 만나면 ([tokenizer.c:196](src/tokenizer.c:196)) 바로 뒤에 `TOK_EOF` 를 하나 붙이고([200](src/tokenizer.c:200)) 멈춘다.
5. [tokenizer.c:204](src/tokenizer.c:204) `*pos = p` — 다음 호출은 `;` 바로 뒤(줄바꿈)부터 읽는다.

결과: 토큰 11개

```
[0]INSERT [1]INTO [2]IDENT(users) [3]VALUES [4]( [5]STRING(kim) [6], [7]INTEGER(20) [8]) [9]; [10]EOF
```

### 2-2. 파서 — parser.c

[main.c:109](src/main.c:109) `run_statement` → [main.c:77](src/main.c:77) → [parser.c:347](src/parser.c:347) `parse`

| 순서  | 위치                                                               | 하는 일                                                         |
| --- | ---------------------------------------------------------------- | ------------------------------------------------------------ |
| 1   | [parser.c:349](src/parser.c:349)                                 | `Parser p = {toks, count, i=0}` — "지금 몇 번째 토큰"을 들고 다니는 구조체   |
| 2   | [parser.c:353](src/parser.c:353)                                 | `Statement` 를 0으로 초기화                                        |
| 3   | [parser.c:324](src/parser.c:324) `parse_statement`               | 첫 토큰이 `INSERT` → [parse_insert](src/parser.c:98)             |
| 4   | [parser.c:104](src/parser.c:104)                                 | `advance` — INSERT 넘김                                        |
| 5   | [parser.c:105](src/parser.c:105) → [expect](src/parser.c:46)     | 다음이 `INTO` 인지 확인하고 넘김 (아니면 `syntax_error`)                   |
| 6   | [parser.c:106](src/parser.c:106) → [parse_name](src/parser.c:56) | `users` 를 `stmt->table` 에 복사                                 |
| 7   | [parser.c:107-108](src/parser.c:107)                             | `VALUES`, `(` 확인                                             |
| 8   | [parser.c:110-119](src/parser.c:110)                             | 반복: [parse_value](src/parser.c:75) 로 값 하나 → `,` 면 계속, 아니면 멈춤 |
| 9   | [parser.c:120](src/parser.c:120)                                 | `)` 확인                                                       |
| 10  | [parser.c:338-343](src/parser.c:338)                             | `;` 가 있으면 넘기고, 그 다음이 반드시 EOF 여야 한다                           |

결과 `Statement`

```
type        = STMT_INSERT
table       = "users"
values[0]   = { VAL_STR, "kim" }
values[1]   = { VAL_INT, 20 }
value_count = 2
where       = NULL
```

### 2-3. 실행기 — executor.c → db.c → schema.c → storage.c

[main.c:78](src/main.c:78) → [executor.c:307](src/executor.c:307) `execute`

**(a) 테이블 얻기 — 처음이라 실제로 연다**

| 순서 | 위치 | 하는 일 |
|---|---|---|
| 1 | [executor.c:316](src/executor.c:316) → [db.c:73](src/db.c:73) `db_get_table` | 열린 테이블 목록에서 `users` 를 찾는다 → 없음 |
| 2 | [db.c:83-89](src/db.c:83) | 이름 길이, 테이블 8개 한도 확인 |
| 3 | [db.c:93](src/db.c:93) → [db.c:36](src/db.c:36) `open_table` | |
| 3-1 | [db.c:45-46](src/db.c:45) → [schema.c:66](src/schema.c:66) `schema_load` | `data/users.schema` 를 연다 |
| 3-2 | [schema.c:76](src/schema.c:76) → [schema.c:34](src/schema.c:34) `read_columns` | 한 줄씩 `fgets` → `\r\n` 떼기([47](src/schema.c:47)) → [parse_column](src/schema.c:8) 로 `"name,TEXT"` 를 쉼표로 쪼갬 |
| 3-3 | [schema.c:82](src/schema.c:82) | 첫 컬럼이 `id,INT` 인지 확인 → 스키마 = `[id:INT, name:TEXT, age:INT]` |
| 3-4 | [db.c:55](src/db.c:55) → [storage.c:151](src/storage.c:151) `storage_scan` | 다음 id 를 정하려고 데이터 파일을 훑는다. `users.csv` 가 아직 없으므로 [storage.c:164-165](src/storage.c:164) 에서 "행 0개"로 바로 성공 |
| 3-5 | [db.c:63](src/db.c:63) | `next_id = 0 + 1 = 1` |
| 3-6 | [db.c:66](src/db.c:66) | `data/users.csv` 를 `"ab"`(이어 쓰기)로 열어 `t->fp` 에 **계속 열어 둔다** |
| 4 | [db.c:97](src/db.c:97) | 성공했으니 `table_count++` |

**(b) INSERT 실행**

[executor.c:321](src/executor.c:321) → [executor.c:135](src/executor.c:135) `run_insert`

| 순서  | 위치                                                                                     | 하는 일                                                                                                                    |
| --- | -------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------- |
| 1   | [executor.c:141](src/executor.c:141) → [check_insert](src/executor.c:51)               | 값 개수 2 == 컬럼 3 - 1(id) ✔ → 타입: `kim`↔TEXT ✔, `20`↔INT ✔ ([check_type](src/executor.c:21)) → TEXT 안에 줄바꿈 없음 ✔            |
| 2   | [executor.c:143](src/executor.c:143)                                                   | `next_id` 가 `LLONG_MAX` 가 아닌지                                                                                           |
| 3   | [executor.c:146](src/executor.c:146) → [format_row](src/executor.c:117)                | [put_int](src/executor.c:90)(id) → `,` → [put_text](src/executor.c:103)(`"kim"`) → `,` → put_int(20) → 문자열 `1,"kim",20` |
| 4   | [executor.c:147](src/executor.c:147) → [storage.c:9](src/storage.c:9) `storage_append` | 길이 검사 → `fseek(END)` → `ftell` 로 offset → `fputs` + `'\n'` (**아직 stdio 버퍼에만 있음**)                                       |
| 5   | [executor.c:150-151](src/executor.c:150)                                               | 저장 성공 뒤에야 `inserted_id = 1`, `next_id = 2`                                                                              |

**(c) 마무리** — [main.c:79](src/main.c:79) `statement_free` (WHERE 트리 없음, 아무 일 없음) → [main.c:83](src/main.c:83) `OK (id=1)` 출력

---

## 3. 문장 2: `INSERT INTO users VALUES ('lee', 17);`

토크나이저·파서는 문장 1과 같다. 다른 점은 **테이블 캐시**뿐이다.

- [db.c:77-81](src/db.c:77) — `users` 가 이미 열려 있으므로 스키마를 다시 읽지 않고 그대로 돌려준다.
- `next_id` 가 2 이므로 `2,"lee",17` 이 버퍼에 추가되고 `OK (id=2)` 출력.

---

## 4. 문장 3: `SELECT name, age FROM users WHERE age >= 18 AND NOT name = 'lee';`

### 4-1. 토크나이저

새로 등장하는 것만:

- `>=` — [read_symbol 의 표](src/tokenizer.c:129) 에서 두 글자 기호를 한 글자 `>` 보다 **먼저** 맞춰 보기 때문에 `>` `=` 로 쪼개지지 않는다.
- `AND`, `NOT`, `WHERE`, `SELECT`, `FROM` — 모두 [word_type](src/tokenizer.c:28) 의 키워드 표.

결과: 토큰 17개

```
SELECT name , age FROM users WHERE age >= 18 AND NOT name = 'lee' ; EOF
```

### 4-2. 파서 — SELECT 와 WHERE 트리

[parse_statement](src/parser.c:324) → 첫 토큰 `SELECT` → [parse_select](src/parser.c:308)

| 순서 | 위치 | 하는 일 |
|---|---|---|
| 1 | [parser.c:314](src/parser.c:314) → [parse_columns](src/parser.c:124) | `*` 가 아님 → `name`, `,`, `age` 반복 → `FROM` 확인 |
| 2 | [parser.c:315](src/parser.c:315) | 테이블 이름 `users` |
| 3 | [parser.c:316-318](src/parser.c:316) | `WHERE` 가 있으면 [parse_or](src/parser.c:286) 로 조건 트리 시작 |

**WHERE 트리 만들기 — 이 코드의 핵심.** 함수가 서로를 부르는 순서를 그대로 적으면:

```
parse_or                                   parser.c:286
└─ parse_and                               parser.c:263
   ├─ parse_not   (NOT 아님)               parser.c:247
   │  └─ parse_primary (괄호 아님)          parser.c:227
   │     └─ parse_compare                  parser.c:208
   │        ├─ parse_name     → "age"
   │        ├─ parse_compare_op → OP_GE    parser.c:189
   │        ├─ parse_value    → 18
   │        └─ cond_new(COMPARE)           parser.c:172   ← malloc 은 다 읽은 뒤에
   │
   ├─ 다음 토큰이 AND → advance             parser.c:268-270
   │
   ├─ parse_not   (NOT 맞음 → advance)      parser.c:254
   │  └─ parse_not (재귀)                   parser.c:255
   │     └─ parse_primary → parse_compare  → name = 'lee'
   │  └─ cond_new(NOT, 자식)               parser.c:259
   │
   └─ cond_new(AND, 왼쪽, 오른쪽)           parser.c:277
└─ 다음 토큰이 OR 아님 → 끝                parser.c:291
```

결과 트리 (`stmt->where`)

```
        AND
       /   \
 age >= 18   NOT
              |
         name = 'lee'
```

> 왜 이 순서? `or → and → not → primary` 로 내려갈수록 **먼저 묶이는**(우선순위 높은) 규칙이다. 그래서 `a OR b AND c` 는 `a OR (b AND c)` 가 된다 ([parser.c:151-157](src/parser.c:151) 주석).

### 4-3. 실행기 — SELECT

[execute](src/executor.c:307) → 테이블 캐시 → [run_select](src/executor.c:291)

**(a) 계획 세우기** — [plan_select](src/executor.c:242)

| 순서  | 위치                                                                              | 하는 일                                                       |
| --- | ------------------------------------------------------------------------------- | ---------------------------------------------------------- |
| 1   | [executor.c:252-258](src/executor.c:252)                                        | 고른 컬럼을 위치 번호로: `name → 1`, `age → 2` (줄마다 이름으로 다시 찾지 않으려고) |
| 2   | [executor.c:259](src/executor.c:259) → [check_cond_columns](src/executor.c:158) | 트리의 `age`, `name` 이 스키마에 있는지 재귀로 확인                        |
| 3   | [executor.c:263](src/executor.c:263) → [check_cond_types](src/executor.c:179)   | `age`(INT) ↔ 18 ✔, `name`(TEXT) ↔ 'lee' ✔                  |

**(b) 파일 훑기** — [executor.c:302](src/executor.c:302) → [storage.c:151](src/storage.c:151) `storage_scan`

| 순서 | 위치 | 하는 일 |
|---|---|---|
| 1 | [storage.c:157](src/storage.c:157) | **`fflush(t->fp)`** — 문장 1·2 의 INSERT 가 버퍼에만 있으므로, 먼저 파일로 내려야 지금 읽을 때 보인다 |
| 2 | [storage.c:162](src/storage.c:162) | 같은 파일을 읽기용(`"rb"`)으로 **따로** 연다 |
| 3 | [storage.c:169](src/storage.c:169) → [scan_lines](src/storage.c:115) | 한 줄씩 `fgets` |
| 4 | [storage.c:124-135](src/storage.c:124) | 너무 긴 줄 / 줄바꿈 없는 끊긴 줄 검사 → `'\n'` 제거 |
| 5 | [storage.c:136](src/storage.c:136) → [split_line](src/storage.c:75) | `1,"kim",20` → 첫 글자가 `"` 면 [read_text](src/storage.c:28), 아니면 [read_int](src/storage.c:55) → `[1, "kim", 20]` |
| 6 | [storage.c:137](src/storage.c:137) → [matches_schema](src/storage.c:100) | 필드 3개, 타입 INT/TEXT/INT 가 스키마와 맞는지 |
| 7 | [storage.c:140](src/storage.c:140) | 콜백 호출 → [select_row](src/executor.c:267) |

**(c) 줄마다 조건 평가** — [select_row](src/executor.c:267) → [cond_matches](src/executor.c:207)

줄 1 `[1, "kim", 20]`

```
AND  (executor.c:210)
├─ age >= 18 : compare_values(20, 18) = +1  → OP_GE → 참     (executor.c:216, 197)
└─ NOT       (executor.c:212)
   └─ name = 'lee' : strcmp("kim","lee") ≠ 0 → 거짓 → NOT → 참
→ 참
```

→ [executor.c:276](src/executor.c:276) `row_count++` → [284-287](src/executor.c:284) `name`, `age` 만 골라 `picked` 에 담음 → `on_row` = [print_row](src/main.c:58) → [print_value](src/main.c:41) → `"kim",20` 출력

줄 2 `[2, "lee", 17]`

```
AND
├─ age >= 18 : compare_values(17, 18) = -1 → 거짓
└─ (&& 라서 오른쪽은 계산하지 않음)
→ 거짓 → executor.c:274 에서 return 0 (출력 안 함)
```

**(d) 마무리**

- [executor.c:303](src/executor.c:303) `result->row_count = 1`
- [main.c:79](src/main.c:79) → [statement_free](src/parser.c:366) → [cond_free](src/parser.c:160) 가 트리를 아래부터 재귀로 `free`
- [main.c:85](src/main.c:85) `(1 rows)` 출력

---

## 5. 프로그램 끝

- [main.c:103](src/main.c:103) — 다음 `tokenize_next` 가 EOF 하나만 돌려줌 → `run_sql` 종료
- [main.c:134](src/main.c:134) → [db_close](src/db.c:102) — 열어 둔 `users.csv` 를 `fclose`
- [main.c:136](src/main.c:136) — SQL 문자열 `free`

---

## 6. 보너스: 오류는 어떻게 흐르나

`example.sql` 끝에 한 줄 추가:

```sql
SELECT * FROM users WHERE age = 'x';
```

1. 토크나이저·파서는 통과한다 (모양은 맞으니까).
2. [check_cond_types](src/executor.c:179) → [check_type](src/executor.c:21) 에서 `age` 는 INT 인데 값이 TEXT
3. [error_set](src/error.c:6) 이 `err` 를 `"Error: column 'age' expects INT, but got TEXT"` 로 채우고 `DB_ERR_TYPE` 반환
4. 이 값이 `run_select → execute → run_statement → run_sql → main` 으로 **그대로 거슬러 올라간다** (아무도 `exit()` 하지 않음)
5. [main.c:138-141](src/main.c:138) — 앞 문장들의 출력을 먼저 `fflush` 한 뒤 stderr 에 메시지 출력, 종료 코드 1

문법 오류라면 (예: `SELECT FROM users;`) 파서의 [syntax_error](src/parser.c:29) 가 토큰 위치를 넣어서 `"Syntax error at position 7: expected '*' or column name, but got 'FROM'"` 처럼 만든다.

---

## 7. 정리: 추천 읽기 순서

| 순서  | 파일                                                          | 먼저 볼 함수                                                                     | 이유                      |
| --- | ----------------------------------------------------------- | --------------------------------------------------------------------------- | ----------------------- |
| 1   | [error.h](src/error.h)                                      | `DbStatus`, `error_set`                                                     | 모든 함수가 이 형식으로 실패를 돌려준다  |
| 2   | [main.c](src/main.c)                                        | `main` → `run_sql` → `run_statement`                                        | 전체 뼈대                   |
| 3   | [token.h](src/token.h) → [tokenizer.c](src/tokenizer.c)     | `tokenize_next` → `next_token` → `read_*`                                   | 입력의 첫 단계                |
| 4   | [statement.h](src/statement.h) → [parser.c](src/parser.c)   | `parse` → `parse_insert` / `parse_select` → `parse_or` 이하                   | 구조체 모양을 먼저 보고 파서를 읽는다   |
| 5   | [executor.h](src/executor.h) → [executor.c](src/executor.c) | `execute` → `run_insert` / `run_select`                                     | 검사 순서와 실행               |
| 6   | [db.c](src/db.c)                                            | `db_get_table` → `open_table`                                               | 테이블 캐시와 `next_id`       |
| 7   | [schema.c](src/schema.c)                                    | `schema_load`                                                               | 짧다                      |
| 8   | [storage.c](src/storage.c)                                  | `storage_append`, `storage_scan` → `scan_lines` → `split_line`              | 실제 파일 형식                |
| 9   | [tests/](tests)                                             | `test_tokenizer.c` → `test_parser.c` → `test_storage.c` → `test_executor.c` | 각 함수의 경계 사례를 예시로 볼 수 있다 |
