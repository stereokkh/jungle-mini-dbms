#!/usr/bin/env bash
# 성능 비교: 서버 작업자 스레드 수(1, 2, 4, 8)를 바꿔 가며 같은 부하를 주고 처리량을 잰다.
#   사용법: make bench          (ROWS=50000 CLIENTS=16 으로 바꿀 수 있다)
set -u

PORT=18081
ROWS=${ROWS:-50000}
CLIENTS=${CLIENTS:-16}

# 테스트 전용 데이터 폴더에 users 테이블을 ROWS 줄 미리 만든다 (엔진의 CSV 형식: id,"name",age)
DATA=$(mktemp -d /tmp/dbms-bench-XXXXXX)
printf 'id,INT\nname,TEXT\nage,INT\n' > "$DATA/users.schema"
awk -v n="$ROWS" 'BEGIN { for (i = 1; i <= n; i++) printf "%d,\"user%d\",%d\n", i, i, 20 + i % 50 }' \
    > "$DATA/users.csv"

# 부하 종류: 이름 | 요청 수 | SQL
WORKLOADS=(
    "lookup (id = N, B+ 트리)|4000|SELECT * FROM users WHERE id = 777;"
    "scan (WHERE age = -1, 전체 훑기)|400|SELECT id FROM users WHERE age = -1;"
    "insert (쓰기 락)|4000|INSERT INTO users VALUES ('bench', 1);"
)

echo "rows=$ROWS, 동시 클라이언트=$CLIENTS, CPU=$(nproc)"
for w in "${WORKLOADS[@]}"; do
    IFS='|' read -r name total sql <<< "$w"
    echo
    echo "== $name, 요청 $total 개"
    for threads in 1 2 4 8; do
        ./build/dbms-server $PORT "$DATA" "$threads" > /dev/null 2>&1 &
        pid=$!
        sleep 0.3
        printf '  threads=%d  ' "$threads"
        ./build/bench $PORT "$CLIENTS" "$total" "$sql"
        kill -INT $pid
        wait $pid
    done
done

rm -rf "$DATA"
