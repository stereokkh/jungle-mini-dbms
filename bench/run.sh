#!/bin/sh
# 100만 건 성능 비교. 컨테이너 안에서 실행한다:
#   docker compose run --rm dev sh bench/run.sh [행 수]
#
# 기존 CLI(build/mini-dbms)에 SQL 파일을 그대로 넣어 잰다.
# 저장소의 data/ 는 건드리지 않고 /tmp/bench 에서 돈다.
#
# 프로세스를 띄울 때마다 파일을 훑어 인덱스를 다시 만드는 시간이 든다.
# 그래서 "테이블만 여는 실행"(base)의 시간을 빼고, 남은 시간을 질의 수로 나눠 질의 하나의 시간을 구한다.
set -e

N=${1:-1000000}
ID_QUERIES=1000     # 인덱스 조회는 너무 빨라서 많이 돌려야 잴 수 있다
SCAN_QUERIES=10

make >/dev/null
BIN=$(pwd)/build/mini-dbms
SCHEMA=$(pwd)/data/users.schema

rm -rf /tmp/bench
mkdir -p /tmp/bench/data
cp "$SCHEMA" /tmp/bench/data/
cd /tmp/bench

# SQL 파일 만들기 (\047 은 작은따옴표)
awk -v n="$N" 'BEGIN { for (i = 1; i <= n; i++) printf "INSERT INTO users VALUES (\047user%d\047, %d);\n", i, i % 100 }' > insert.sql
awk -v n="$N" -v q="$ID_QUERIES" 'BEGIN { srand(1); for (i = 0; i < q; i++) printf "SELECT * FROM users WHERE id = %d;\n", int(rand() * n) + 1 }' > by_id.sql
awk -v n="$N" -v q="$SCAN_QUERIES" 'BEGIN { srand(2); for (i = 0; i < q; i++) printf "SELECT * FROM users WHERE name = \047user%d\047;\n", int(rand() * n) + 1 }' > by_name.sql
# 같은 id 를 찾지만 `id = N` 모양이 아니라서 인덱스를 못 쓰는 질의
awk -v n="$N" -v q="$SCAN_QUERIES" 'BEGIN { srand(3); for (i = 0; i < q; i++) { k = int(rand() * n) + 1; printf "SELECT * FROM users WHERE id >= %d AND id <= %d;\n", k, k } }' > by_id_scan.sql
echo "SELECT * FROM users WHERE id = 0;" > base.sql

# SQL 파일 하나를 실행하는 데 걸린 시간(마이크로초)
run_us() {
    start=$(date +%s%N)
    "$BIN" "$1" > "$1.out"
    end=$(date +%s%N)
    echo $(( (end - start) / 1000 ))
}

insert_us=$(run_us insert.sql)
base_us=$(run_us base.sql)
id_us=$(run_us by_id.sql)
name_us=$(run_us by_name.sql)
id_scan_us=$(run_us by_id_scan.sql)

# 찾은 행 수가 맞는지 확인: 질의마다 정확히 한 행
test "$(grep -c '^(1 rows)$' by_id.sql.out)" = "$ID_QUERIES"
test "$(grep -c '^(1 rows)$' by_name.sql.out)" = "$SCAN_QUERIES"
test "$(grep -c '^(1 rows)$' by_id_scan.sql.out)" = "$SCAN_QUERIES"

per_id=$(( (id_us - base_us) / ID_QUERIES ))
per_name=$(( (name_us - base_us) / SCAN_QUERIES ))
per_id_scan=$(( (id_scan_us - base_us) / SCAN_QUERIES ))
if [ "$per_id" -lt 1 ]; then per_id=1; fi

echo "rows                           : $N"
echo "data file                      : $(wc -c < data/users.csv) bytes"
echo "INSERT $N rows                 : $(( insert_us / 1000 )) ms"
echo "open table (scan + build index): $(( base_us / 1000 )) ms"
echo "SELECT WHERE id = N   (index)  : $per_id us / query"
echo "SELECT WHERE name = .. (scan)  : $per_name us / query"
echo "SELECT WHERE id >= N AND id <= N (scan): $per_id_scan us / query"
echo "name scan / id index           : $(( per_name / per_id ))x"
