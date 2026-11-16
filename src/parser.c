#include "statement.h"

#include <string.h>

// 파싱하는 동안 "지금 몇 번째 토큰을 보고 있는지"를 들고 다닌다.
// 전역 변수로 두지 않고 함수마다 넘겨서, 여러 스레드가 동시에 파싱해도 섞이지 않게 한다.
typedef struct {
    const Token *toks;
    int          count;
    int          i;
} Parser;

static const Token *peek(const Parser *p)
{
    return &p->toks[p->i];
}

// 마지막 토큰(보통 EOF)에서는 더 나아가지 않는다.
// 그래야 문장이 중간에 끊겨도 배열 밖을 읽지 않고, 다음 검사에서 문법 오류로 끝난다.
static void advance(Parser *p)
{
    if (p->i < p->count - 1) {
        p->i++;
    }
}

// "expected ..., but got ..." 문법 오류를 지금 보고 있는 토큰 위치로 만든다.
static DbStatus syntax_error(const Parser *p, const char *expected, DbError *err)
{
    const Token *t = peek(p);

    if (t->type == TOK_EOF) {
        return error_set(err, DB_ERR_SYNTAX, t->pos, "expected %s, but got end of input", expected);
    }
    // 긴 문자열 토큰이 메시지를 다 차지하지 않게 앞의 40바이트만 보여 준다.
    return error_set(err, DB_ERR_SYNTAX, t->pos, "expected %s, but got '%.40s'", expected, t->text);
}

static DbStatus too_long(const Parser *p, const char *what, int limit, DbError *err)
{
    return error_set(err, DB_ERR_TOO_LONG, peek(p)->pos, "%s (limit is %d)", what, limit);
}

// 지금 토큰이 type 이면 넘어가고, 아니면 문법 오류다.
static DbStatus expect(Parser *p, TokenType type, const char *expected, DbError *err)
{
    if (peek(p)->type != type) {
        return syntax_error(p, expected, err);
    }
    advance(p);
    return DB_OK;
}

// 테이블·컬럼 이름 하나를 dst 에 복사한다. size 는 dst 의 크기다.
static DbStatus parse_name(Parser *p, char *dst, size_t size, const char *expected, DbError *err)
{
    const Token *t = peek(p);

    if (t->type != TOK_IDENTIFIER) {
        return syntax_error(p, expected, err);
    }
    // 토크나이저가 이미 63바이트로 막지만, 파서만 따로 불러도 버퍼를 넘지 않게 여기서도 검사한다.
    size_t len = strnlen(t->text, sizeof t->text);
    if (len >= size) {
        return too_long(p, "name is too long", (int)size - 1, err);
    }
    memcpy(dst, t->text, len);
    dst[len] = '\0';
    advance(p);
    return DB_OK;
}

// 값 하나 (정수 또는 문자열)
static DbStatus parse_value(Parser *p, Value *v, DbError *err)
{
    const Token *t = peek(p);

    if (t->type == TOK_INTEGER) {
        v->type = VAL_INT;
        v->i = t->int_value;
    } else if (t->type == TOK_STRING) {
        size_t len = strnlen(t->text, sizeof t->text);
        if (len >= sizeof v->s) {
            return too_long(p, "string is too long", (int)sizeof v->s - 1, err);
        }
        v->type = VAL_STR;
        memcpy(v->s, t->text, len);
        v->s[len] = '\0';
    } else {
        return syntax_error(p, "a value", err);
    }
    advance(p);
    return DB_OK;
}

// INSERT INTO 테이블 VALUES ( 값 {, 값} )
static DbStatus parse_insert(Parser *p, Statement *stmt, DbError *err)
{
    int max_values = (int)(sizeof stmt->values / sizeof stmt->values[0]);
    DbStatus st;

    stmt->type = STMT_INSERT;
    advance(p);     // INSERT
    if ((st = expect(p, TOK_INTO, "INTO", err)) != DB_OK) return st;
    if ((st = parse_name(p, stmt->table, sizeof stmt->table, "table name", err)) != DB_OK) return st;
    if ((st = expect(p, TOK_VALUES, "VALUES", err)) != DB_OK) return st;
    if ((st = expect(p, TOK_LPAREN, "'('", err)) != DB_OK) return st;

    for (;;) {
        if (stmt->value_count >= max_values) {
            return too_long(p, "too many values", max_values, err);
        }
        st = parse_value(p, &stmt->values[stmt->value_count], err);
        if (st != DB_OK) return st;
        stmt->value_count++;
        if (peek(p)->type != TOK_COMMA) break;
        advance(p);
    }
    return expect(p, TOK_RPAREN, "',' or ')'", err);
}

// SELECT 바로 뒤: * 또는 컬럼 {, 컬럼}
static DbStatus parse_columns(Parser *p, Statement *stmt, DbError *err)
{
    int max_columns = (int)(sizeof stmt->columns / sizeof stmt->columns[0]);
    const char *expected = "'*' or column name";

    if (peek(p)->type == TOK_STAR) {
        stmt->select_all = 1;
        advance(p);
        return expect(p, TOK_FROM, "FROM", err);
    }
    for (;;) {
        if (stmt->column_count >= max_columns) {
            return too_long(p, "too many columns", max_columns, err);
        }
        char *dst = stmt->columns[stmt->column_count];
        DbStatus st = parse_name(p, dst, sizeof stmt->columns[0], expected, err);
        if (st != DB_OK) return st;
        stmt->column_count++;
        if (peek(p)->type != TOK_COMMA) break;
        advance(p);
        expected = "column name";   // 두 번째부터는 * 가 올 수 없다
    }
    return expect(p, TOK_FROM, "',' or FROM", err);
}

// WHERE 컬럼 = 값
static DbStatus parse_where(Parser *p, Statement *stmt, DbError *err)
{
    Where *w = &stmt->where;
    DbStatus st;

    advance(p);     // WHERE
    if ((st = parse_name(p, w->column, sizeof w->column, "column name", err)) != DB_OK) return st;
    if ((st = expect(p, TOK_EQ, "'='", err)) != DB_OK) return st;
    w->op = '=';
    if ((st = parse_value(p, &w->value, err)) != DB_OK) return st;
    stmt->has_where = 1;
    return DB_OK;
}

// SELECT (* | 컬럼들) FROM 테이블 [WHERE 컬럼 = 값]
static DbStatus parse_select(Parser *p, Statement *stmt, DbError *err)
{
    DbStatus st;

    stmt->type = STMT_SELECT;
    advance(p);     // SELECT
    if ((st = parse_columns(p, stmt, err)) != DB_OK) return st;
    if ((st = parse_name(p, stmt->table, sizeof stmt->table, "table name", err)) != DB_OK) return st;
    if (peek(p)->type == TOK_WHERE) {
        return parse_where(p, stmt, err);
    }
    return DB_OK;
}

DbStatus parse(const Token *toks, int count, Statement *stmt, DbError *err)
{
    Parser p = { toks, count, 0 };
    DbStatus st;

    // 앞 문장의 값이 남아 있으면 select_all, has_where 같은 표시가 잘못 켜진 채로 나간다.
    memset(stmt, 0, sizeof *stmt);
    if (count <= 0) {
        return error_set(err, DB_ERR_SYNTAX, -1, "empty statement");
    }

    if (peek(&p)->type == TOK_INSERT) {
        st = parse_insert(&p, stmt, err);
    } else if (peek(&p)->type == TOK_SELECT) {
        st = parse_select(&p, stmt, err);
    } else {
        return syntax_error(&p, "INSERT or SELECT", err);
    }
    if (st != DB_OK) return st;

    // 문장 뒤에 다른 것이 붙어 있으면 안 된다: [;] EOF
    if (peek(&p)->type == TOK_SEMICOLON) {
        advance(&p);
    }
    if (peek(&p)->type != TOK_EOF) {
        return syntax_error(&p, "end of statement", err);
    }
    return DB_OK;
}
