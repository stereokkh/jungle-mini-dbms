#include "executor.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "storage.h"

// 이름으로 컬럼 위치를 찾는다. 없으면 -1.
static int find_column(const Schema *s, const char *name)
{
    for (int i = 0; i < s->count; i++) {
        if (strcmp(s->cols[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

// 값의 종류가 컬럼 타입과 맞는지 본다 (COL_INT ↔ VAL_INT, COL_TEXT ↔ VAL_STR).
static DbStatus check_type(const Column *col, const Value *v, DbError *err)
{
    int col_is_int = (col->type == COL_INT);
    int val_is_int = (v->type == VAL_INT);

    if (col_is_int != val_is_int) {
        return error_set(err, DB_ERR_TYPE, -1, "column '%s' expects %s, but got %s", col->name,
                         col_is_int ? "INT" : "TEXT", val_is_int ? "INT" : "TEXT");
    }
    return DB_OK;
}

// ---------- INSERT ----------

// id 를 뺀 컬럼 이름을 "name, age" 모양으로 이어 붙인다. 오류 메시지에 쓴다.
static void join_column_names(const Schema *s, char *out, size_t size)
{
    size_t len = 0;

    out[0] = '\0';
    for (int i = 1; i < s->count; i++) {
        int n = snprintf(out + len, size - len, "%s%s", i > 1 ? ", " : "", s->cols[i].name);
        if (n < 0 || (size_t)n >= size - len) {
            break;      // 버퍼가 찼다. 메시지용이라 잘린 채로 둔다
        }
        len += (size_t)n;
    }
}

// 검사 순서 2~4: 값 개수 → 값의 종류 → TEXT 안의 줄바꿈
static DbStatus check_insert(const Table *t, const Statement *stmt, DbError *err)
{
    const Schema *s = &t->schema;

    if (stmt->value_count != s->count - 1) {
        char names[128];
        join_column_names(s, names, sizeof names);
        // 값이 스키마 컬럼 수와 같으면 id 까지 넣은 것이다. 흔한 실수라 이유를 알려 준다.
        const char *hint = (stmt->value_count == s->count) ? " (id is assigned automatically)" : "";
        return error_set(err, DB_ERR_COLUMN_COUNT, -1, "expected %d values (%s), but got %d%s",
                         s->count - 1, names, stmt->value_count, hint);
    }
    for (int i = 0; i < stmt->value_count; i++) {
        const Column *col = &s->cols[i + 1];    // 값 i 는 id 다음 컬럼부터 짝이 된다
        const Value *v = &stmt->values[i];
        DbStatus st = check_type(col, v, err);
        if (st != DB_OK) {
            return st;
        }
        // 한 줄이 레코드 하나라서, 값 안에 줄바꿈이 있으면 파일에서 두 줄로 갈라진다.
        if (v->type == VAL_STR && strchr(v->s, '\n') != NULL) {
            return error_set(err, DB_ERR_TYPE, -1, "column '%s' cannot contain a line break",
                             col->name);
        }
    }
    return DB_OK;
}

// line 끝에 글자 하나를 붙인다. 자리가 없으면 0 이 아닌 값을 돌려준다.
// 저장소가 붙일 줄바꿈과 끝의 '\0' 자리를 남겨야 해서 내용은 MAX_LINE_LEN - 1 바이트까지다.
static int put_char(char *line, size_t *len, char c)
{
    if (*len >= MAX_LINE_LEN - 1) {
        return -1;
    }
    line[(*len)++] = c;
    return 0;
}

static int put_int(char *line, size_t *len, long long value)
{
    char digits[32];
    int bad = 0;

    snprintf(digits, sizeof digits, "%lld", value);
    for (const char *p = digits; *p != '\0'; p++) {
        bad |= put_char(line, len, *p);
    }
    return bad;
}

// TEXT 는 큰따옴표로 감싸고 안의 " 는 "" 로 쓴다. 값에 쉼표가 있어도 필드가 갈라지지 않는다.
static int put_text(char *line, size_t *len, const char *s)
{
    int bad = put_char(line, len, '"');

    for (; *s != '\0'; s++) {
        if (*s == '"') {
            bad |= put_char(line, len, '"');
        }
        bad |= put_char(line, len, *s);
    }
    return bad | put_char(line, len, '"');
}

// id 와 값들을 CSV 한 줄로 만든다. line 의 크기는 MAX_LINE_LEN 이어야 한다.
static DbStatus format_row(long long id, const Statement *stmt, char *line, DbError *err)
{
    size_t len = 0;
    int bad = put_int(line, &len, id);

    for (int i = 0; i < stmt->value_count; i++) {
        const Value *v = &stmt->values[i];
        bad |= put_char(line, &len, ',');
        bad |= (v->type == VAL_INT) ? put_int(line, &len, v->i) : put_text(line, &len, v->s);
    }
    if (bad) {
        return error_set(err, DB_ERR_TOO_LONG, -1, "row is too long (limit is %d bytes)",
                         MAX_LINE_LEN);
    }
    line[len] = '\0';
    return DB_OK;
}

static DbStatus run_insert(Table *t, const Statement *stmt, ExecResult *result, DbError *err)
{
    char line[MAX_LINE_LEN];
    long offset;
    DbStatus st;

    if ((st = check_insert(t, stmt, err)) != DB_OK) return st;
    // 마지막 id 를 쓰고 나면 next_id 를 더 늘릴 수 없다 (+1 이 넘친다). 쓰기 전에 막는다.
    if (t->next_id == LLONG_MAX) {
        return error_set(err, DB_ERR_IO, -1, "table '%s' has no ids left", t->name);
    }
    if ((st = format_row(t->next_id, stmt, line, err)) != DB_OK) return st;
    if ((st = storage_append(t, line, &offset, err)) != DB_OK) return st;

    db_index_row(t, t->next_id, offset);
    // 저장에 성공한 뒤에만 id 를 넘긴다. 실패한 INSERT 가 id 를 먹지 않게 하려는 것이다.
    result->inserted_id = t->next_id;
    t->next_id++;
    return DB_OK;
}

// ---------- SELECT ----------

// 검사 순서 5: 조건 트리의 모든 컬럼이 스키마에 있는지 본다.
static DbStatus check_cond_columns(const Table *t, const Cond *c, DbError *err)
{
    if (c == NULL) {
        return DB_OK;
    }
    if (c->type == COND_COMPARE) {
        if (find_column(&t->schema, c->column) < 0) {
            return error_set(err, DB_ERR_NO_COLUMN, -1, "column '%s' does not exist in table '%s'",
                             c->column, t->name);
        }
        return DB_OK;
    }
    DbStatus st = check_cond_columns(t, c->left, err);
    if (st != DB_OK) {
        return st;
    }
    return check_cond_columns(t, c->right, err);
}

// 검사 순서 6: 조건 트리의 모든 비교에서 값의 종류가 컬럼 타입과 맞는지 본다.
// check_cond_columns 를 통과한 뒤에 부르므로 컬럼은 반드시 찾아진다.
static DbStatus check_cond_types(const Schema *s, const Cond *c, DbError *err)
{
    if (c == NULL) {
        return DB_OK;
    }
    if (c->type == COND_COMPARE) {
        return check_type(&s->cols[find_column(s, c->column)], &c->value, err);
    }
    DbStatus st = check_cond_types(s, c->left, err);
    if (st != DB_OK) {
        return st;
    }
    return check_cond_types(s, c->right, err);
}

// a 가 b 보다 작으면 음수, 같으면 0, 크면 양수.
// INT 는 숫자로, TEXT 는 글자 그대로(바이트 순서, 대소문자 구분) 비교한다.
// 두 값의 종류가 같다는 것은 check_cond_types 와 저장소가 이미 확인했다.
static int compare_values(const Value *a, const Value *b)
{
    if (a->type == VAL_INT) {
        return (a->i > b->i) - (a->i < b->i);   // a - b 는 넘칠 수 있어서 쓰지 않는다
    }
    return strcmp(a->s, b->s);
}

// 행 하나(fields)가 조건 트리 c 를 만족하면 1, 아니면 0.
// 가지는 자식의 결과를 합치고, 잎은 그 행의 컬럼 값과 조건의 값을 비교한다.
static int cond_matches(const Schema *s, const Cond *c, const Value *fields)
{
    switch (c->type) {
    case COND_AND: return cond_matches(s, c->left, fields) && cond_matches(s, c->right, fields);
    case COND_OR:  return cond_matches(s, c->left, fields) || cond_matches(s, c->right, fields);
    case COND_NOT: return !cond_matches(s, c->left, fields);
    case COND_COMPARE: break;
    }

    int cmp = compare_values(&fields[find_column(s, c->column)], &c->value);
    switch (c->op) {
    case OP_EQ: return cmp == 0;
    case OP_NE: return cmp != 0;
    case OP_LT: return cmp < 0;
    case OP_GT: return cmp > 0;
    case OP_LE: return cmp <= 0;
    case OP_GE: return cmp >= 0;
    }
    return 0;
}

// 저장소가 줄마다 콜백을 부를 때 필요한 것을 모아 둔 것.
// 고른 컬럼은 줄마다 이름으로 다시 찾지 않도록, 훑기 전에 위치(번호)로 바꿔 둔다.
typedef struct {
    const Schema *schema;
    int           select_all;
    int           cols[16];     // 고른 컬럼의 위치, 고른 순서대로
    int           col_count;
    const Cond   *where;        // 조건 트리. 조건이 없으면 NULL
    RowCallback   on_row;
    void         *ctx;
    int           row_count;
} SelectPlan;

// 검사 순서 5~6: 고른 컬럼과 WHERE 컬럼이 있는지 → WHERE 값의 종류
static DbStatus plan_select(const Table *t, const Statement *stmt, SelectPlan *plan, DbError *err)
{
    const Schema *s = &t->schema;

    plan->schema = s;
    plan->select_all = stmt->select_all;
    plan->col_count = stmt->column_count;
    plan->where = stmt->where;
    plan->row_count = 0;

    for (int i = 0; i < stmt->column_count; i++) {
        plan->cols[i] = find_column(s, stmt->columns[i]);
        if (plan->cols[i] < 0) {
            return error_set(err, DB_ERR_NO_COLUMN, -1, "column '%s' does not exist in table '%s'",
                             stmt->columns[i], t->name);
        }
    }
    DbStatus st = check_cond_columns(t, stmt->where, err);
    if (st != DB_OK) {
        return st;
    }
    return check_cond_types(s, stmt->where, err);
}

// storage_scan 콜백: 조건에 맞는 줄이면 고른 컬럼만 골라 on_row 로 넘긴다.
static int select_row(const Value *fields, int field_count, long offset, void *ctx)
{
    SelectPlan *plan = ctx;
    Value picked[16];

    (void)offset;
    if (plan->where != NULL && !cond_matches(plan->schema, plan->where, fields)) {
        return 0;
    }
    plan->row_count++;
    if (plan->on_row == NULL) {
        return 0;
    }
    if (plan->select_all) {
        plan->on_row(fields, field_count, plan->ctx);
        return 0;
    }
    for (int i = 0; i < plan->col_count; i++) {
        picked[i] = fields[plan->cols[i]];
    }
    plan->on_row(picked, plan->col_count, plan->ctx);
    return 0;
}

static DbStatus run_select(Db *db, Table *t, const Statement *stmt, RowCallback on_row,
                           void *ctx, ExecResult *result, DbError *err)
{
    SelectPlan plan;
    DbStatus st = plan_select(t, stmt, &plan, err);

    if (st != DB_OK) {
        return st;
    }
    plan.on_row = on_row;
    plan.ctx = ctx;
    const Cond *where = plan.where;
    if (t->id_index_usable && where != NULL && where->type == COND_COMPARE &&
        where->op == OP_EQ && strcmp(where->column, "id") == 0) {
        long offset;
        if (bpt_find(t->id_index, where->value.i, &offset)) {
            st = storage_read_at(db, t, offset, select_row, &plan, err);
        }
    } else {
        st = storage_scan(db, t, select_row, &plan, err);
    }
    result->row_count = plan.row_count;
    return st;
}

DbStatus execute(Db *db, const Statement *stmt, RowCallback on_row, void *ctx,
                 ExecResult *result, DbError *err)
{
    Table *t;

    result->inserted_id = 0;
    result->row_count = 0;

    // 검사 순서 1: 테이블이 있는가
    DbStatus st = db_get_table(db, stmt->table, &t, err);
    if (st != DB_OK) {
        return st;
    }
    if (stmt->type == STMT_INSERT) {
        return run_insert(t, stmt, result, err);
    }
    return run_select(db, t, stmt, on_row, ctx, result, err);
}
