# mini-dbms API 서버 (CDH)
#
#   make          build/dbms-server
#   make test     단위 테스트 (HTTP 파서, SQL 실행기, JSON-RPC, JSON, 스레드 풀)
#   make e2e      서버를 띄우고 curl 로 기능 테스트
#   make bench    작업자 스레드 수(1/2/4/8)별 성능 비교
#   make run      서버 실행 (PORT=8080 THREADS=4, 데이터는 저장소 루트의 data/)
#
# 서버 코드는 저장소 루트의 src/server/, DB 엔진은 src/ (Phase 1+2) 에 있다.
# 이 폴더에는 학습 문서와 테스트만 둔다.

CC      ?= gcc
ENGINE  := ../../src
SERVER  := $(ENGINE)/server
CFLAGS  := -std=c11 -D_GNU_SOURCE -Wall -Wextra -g -O2 -I$(SERVER) -I$(ENGINE)
PORT    ?= 8080
THREADS ?= 4
DATA    ?= ../../data

# 엔진 파일은 이름을 직접 적는다. src/*.c 를 통째로 가져오면 src/ 에 다른 서버 파일
# (예: PR #6 의 src/http.c)이 들어왔을 때 같은 이름 함수가 두 번 생겨 링크가 깨진다.
ENGINE_SRCS := $(addprefix $(ENGINE)/,bptree.c db.c error.c executor.c parser.c schema.c sql.c storage.c tokenizer.c)
SERVER_SRCS := $(addprefix $(SERVER)/,http.c json.c pool.c query.c rpc.c)
HEADERS     := $(wildcard $(SERVER)/*.h) $(wildcard $(ENGINE)/*.h)

.PHONY: all test e2e bench run clean

all: build/dbms-server

build/dbms-server: $(SERVER)/main.c $(SERVER_SRCS) $(ENGINE_SRCS) $(HEADERS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SERVER)/main.c $(SERVER_SRCS) $(ENGINE_SRCS) -pthread

build/test_%: tests/test_%.c $(SERVER_SRCS) $(ENGINE_SRCS) $(HEADERS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $< $(SERVER_SRCS) $(ENGINE_SRCS) -pthread

test: build/test_http build/test_query build/test_rpc build/test_json build/test_pool
	./build/test_http
	./build/test_json
	./build/test_pool
	./build/test_query
	./build/test_rpc

e2e: build/dbms-server
	bash tests/e2e.sh

build/bench: tests/bench.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $< -pthread

bench: build/dbms-server build/bench
	bash tests/bench.sh

run: build/dbms-server
	./build/dbms-server $(PORT) $(DATA) $(THREADS)

clean:
	rm -rf build
