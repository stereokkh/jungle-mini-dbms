# mini-dbms API 서버 (CDH)
#
#   make          build/dbms-server
#   make test     HTTP 파서 단위 테스트
#   make e2e      서버를 띄우고 curl 로 기능 테스트
#   make run      서버 실행 (PORT=8080)
#
# DB 엔진은 저장소 루트의 src/ (Phase 1+2) 를 그대로 가져다 쓴다.

CC      ?= gcc
ENGINE  := ../../src
CFLAGS  := -std=c11 -D_GNU_SOURCE -Wall -Wextra -g -O2 -Iserver -I$(ENGINE)
PORT    ?= 8080

ENGINE_SRCS := $(filter-out $(ENGINE)/main.c,$(wildcard $(ENGINE)/*.c))
SERVER_SRCS := server/http.c

.PHONY: all test e2e run clean

all: build/dbms-server

build/dbms-server: server/main.c $(SERVER_SRCS) server/http.h $(ENGINE_SRCS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ server/main.c $(SERVER_SRCS) $(ENGINE_SRCS) -pthread

build/test_http: tests/test_http.c $(SERVER_SRCS) server/http.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/test_http.c $(SERVER_SRCS)

test: build/test_http
	./build/test_http

e2e: build/dbms-server
	bash tests/e2e.sh

run: build/dbms-server
	./build/dbms-server $(PORT)

clean:
	rm -rf build
