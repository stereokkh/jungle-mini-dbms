# Mini DBMS build
#
#   make             build/mini-dbms
#   make test        tests/test_*.c 를 각각 빌드해서 실행
#   make bench       100만 행의 ID 인덱스 조회와 전체 스캔 비교
#   make asan        AddressSanitizer/UBSan 빌드로 make test 실행
#   make clean
#
# 새 소스(src/*.c)나 단위 테스트(tests/test_*.c)는 이 파일을 고치지 않아도
# 와일드카드로 자동 포함된다.

CC          ?= gcc
CFLAGS_BASE := -std=c11 -D_GNU_SOURCE -Wall -Wextra -g -Isrc
OPTFLAGS    ?= -O2
CFLAGS      ?= $(CFLAGS_BASE) $(OPTFLAGS)
LDFLAGS     ?=

BUILD       ?= build

SRCS        := $(wildcard src/*.c)
HDRS        := $(wildcard src/*.h)
LIB_SRCS    := $(filter-out src/main.c,$(SRCS))
TEST_SRCS   := $(wildcard tests/test_*.c)
TESTS       := $(patsubst tests/%.c,$(BUILD)/%,$(TEST_SRCS))
BIN         := $(BUILD)/mini-dbms
BENCH       := $(BUILD)/bench_index

SAN_FLAGS   := -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: all test bench asan clean

all: $(BIN)

$(BUILD):
	mkdir -p $@

$(BIN): $(SRCS) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

# 단위 테스트: 테스트 파일 + main.c 를 뺀 모든 소스
$(BUILD)/test_%: tests/test_%.c $(LIB_SRCS) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(LIB_SRCS) $(LDFLAGS)

$(BENCH): bench/bench_index.c $(LIB_SRCS) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(LIB_SRCS) $(LDFLAGS)

test: $(TESTS)
	@set -e; for t in $(TESTS); do echo "== $$t"; $$t; done
	@echo "== all tests passed"

bench: $(BENCH)
	./$(BENCH)

asan:
	$(MAKE) BUILD=build/asan CFLAGS="$(CFLAGS_BASE) -O1 $(SAN_FLAGS)" LDFLAGS="$(SAN_FLAGS)" test

clean:
	rm -rf build
