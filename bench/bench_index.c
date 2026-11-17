#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "executor.h"

enum { ROWS = 1000000, TARGET = 750000, TRIALS = 21 };

static void require(bool ok, const char *message)
{
    if (!ok) {
        fprintf(stderr, "%s\n", message);
        exit(EXIT_FAILURE);
    }
}

static double now(void)
{
    struct timespec ts;
    require(clock_gettime(CLOCK_MONOTONIC, &ts) == 0, "cannot read clock");
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void capture_id(const Value *fields, int count, void *ctx)
{
    if (count == 1) *(long long *)ctx = fields[0].i;
}

static double measure_select(Db *db, const Statement *stmt)
{
    long long id = 0;
    ExecResult result;
    DbError err;
    double start = now();
    DbStatus status = execute(db, stmt, capture_id, &id, &result, &err);
    double elapsed = now() - start;
    require(status == DB_OK, err.message);
    require(result.row_count == 1 && id == TARGET, "SELECT returned the wrong row");
    return elapsed;
}

static int compare_times(const void *left, const void *right)
{
    double a = *(const double *)left;
    double b = *(const double *)right;
    return (a > b) - (a < b);
}

int main(void)
{
    char dir[] = "/tmp/mini-dbms-bench-XXXXXX";
    char schema[128], csv[128];
    require(mkdtemp(dir) != NULL, "cannot create benchmark directory");
    snprintf(schema, sizeof schema, "%s/users.schema", dir);
    snprintf(csv, sizeof csv, "%s/users.csv", dir);
    FILE *fp = fopen(schema, "wb");
    require(fp != NULL, "cannot create benchmark schema");
    require(fputs("id,INT\nname,TEXT\nage,INT\n", fp) != EOF, "cannot write benchmark schema");
    require(fclose(fp) == 0, "cannot close benchmark schema");

    Db db;
    DbError err;
    ExecResult result;
    require(db_open(dir, &db, &err) == DB_OK, err.message);
    Statement insert = {
        .type = STMT_INSERT, .table = "users", .value_count = 2,
        .values = {{.type = VAL_STR}, {.type = VAL_INT, .i = 20}},
    };
    double start = now();
    for (int i = 1; i <= ROWS; i++) {
        snprintf(insert.values[0].s, sizeof insert.values[0].s, "user_%d", i);
        require(execute(&db, &insert, NULL, NULL, &result, &err) == DB_OK, err.message);
        require(result.inserted_id == i, "INSERT returned the wrong ID");
    }
    require(db.tables[0].id_index_usable, "index unavailable after inserts");
    db_close(&db);
    double insert_time = now() - start;

    Table *table;
    start = now();
    require(db_open(dir, &db, &err) == DB_OK, err.message);
    require(db_get_table(&db, "users", &table, &err) == DB_OK, err.message);
    double reopen_time = now() - start;
    require(table->next_id == ROWS + 1 && table->id_index_usable, "index rebuild failed");

    Cond by_id = {.type = COND_COMPARE, .column = "id", .op = OP_EQ,
                  .value = {.type = VAL_INT, .i = TARGET}};
    Cond by_name = {.type = COND_COMPARE, .column = "name", .op = OP_EQ,
                    .value = {.type = VAL_STR}};
    snprintf(by_name.value.s, sizeof by_name.value.s, "user_%d", TARGET);
    Statement queries[] = {
        {.type = STMT_SELECT, .table = "users", .columns = {"id"}, .column_count = 1, .where = &by_id},
        {.type = STMT_SELECT, .table = "users", .columns = {"id"}, .column_count = 1, .where = &by_name},
    };
    double times[2][TRIALS];
    for (int i = 0; i < TRIALS; i++) {
        int first = i % 2;
        times[first][i] = measure_select(&db, &queries[first]);
        times[1 - first][i] = measure_select(&db, &queries[1 - first]);
    }
    db_close(&db);
    require(unlink(csv) == 0 && unlink(schema) == 0 && rmdir(dir) == 0, "cannot remove benchmark data");

    qsort(times[0], TRIALS, sizeof times[0][0], compare_times);
    qsort(times[1], TRIALS, sizeof times[1][0], compare_times);
    double indexed = times[0][TRIALS / 2];
    double scanned = times[1][TRIALS / 2];
    printf("%d inserts: %.3f s\n", ROWS, insert_time);
    printf("Open table and rebuild index: %.3f s\n", reopen_time);
    printf("Median of %d SELECTs per condition\n", TRIALS);
    printf("  WHERE id   = %d: %.2f us\n", TARGET, indexed * 1e6);
    printf("  WHERE name = 'user_%d': %.2f ms\n", TARGET, scanned * 1e3);
    if (indexed > 0) printf("  Speedup: %.0fx\n", scanned / indexed);
    return 0;
}
