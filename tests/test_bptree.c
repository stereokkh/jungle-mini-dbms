#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "bptree.h"

// 1..n 을 넣었다고 보고, 모두 찾아지고 값이 key * 10 인지 확인한다.
static void check_all(const BPTree *t, long long n)
{
    for (long long k = 1; k <= n; k++) {
        long off = -1;
        assert(bptree_search(t, k, &off) == 1);
        assert(off == k * 10);
    }
}

static void test_empty(void)
{
    BPTree *t = bptree_create();
    long    off = 123;

    assert(t != NULL);
    assert(t->count == 0);
    assert(t->height == 1);
    assert(bptree_search(t, 1, &off) == 0);
    assert(off == 123);                 // 못 찾으면 *offset 을 건드리지 않는다
    bptree_destroy(t);
}

// 노드 하나에 다 들어가는 동안은 나뉘지 않는다.
static void test_no_split(void)
{
    BPTree *t = bptree_create();

    for (long long k = 1; k <= BPTREE_MAX_KEYS; k++) {
        assert(bptree_insert(t, k, k * 10) == BPTREE_OK);
    }
    assert(t->count == BPTREE_MAX_KEYS);
    assert(t->height == 1);
    check_all(t, BPTREE_MAX_KEYS);
    bptree_destroy(t);
}

// 한 개를 더 넣으면 리프가 나뉘고 루트가 새로 생긴다.
static void test_first_split(void)
{
    BPTree *t = bptree_create();
    long long n = BPTREE_MAX_KEYS + 1;
    long off;

    for (long long k = 1; k <= n; k++) {
        assert(bptree_insert(t, k, k * 10) == BPTREE_OK);
    }
    assert(t->count == n);
    assert(t->height == 2);
    check_all(t, n);
    assert(bptree_search(t, 0, &off) == 0);
    assert(bptree_search(t, n + 1, &off) == 0);
    bptree_destroy(t);
}

// 실제 사용 모양: id 가 1 부터 차례로 들어온다. 내부 노드도 나뉠 만큼 넣는다.
static void test_ascending(void)
{
    BPTree *t = bptree_create();
    long long n = 200000;
    long off;

    for (long long k = 1; k <= n; k++) {
        assert(bptree_insert(t, k, k * 10) == BPTREE_OK);
    }
    assert(t->count == n);
    assert(t->height == 3);
    check_all(t, n);
    assert(bptree_search(t, n + 1, &off) == 0);
    bptree_destroy(t);
}

static void test_descending(void)
{
    BPTree *t = bptree_create();
    long long n = 50000;

    for (long long k = n; k >= 1; k--) {
        assert(bptree_insert(t, k, k * 10) == BPTREE_OK);
    }
    assert(t->count == n);
    check_all(t, n);
    bptree_destroy(t);
}

// 순서를 섞어서 넣어도 모두 찾아진다. 시드를 고정해 매번 같은 순서로 돈다.
static void test_shuffled(void)
{
    enum { N = 50000 };
    static long long keys[N];
    BPTree *t = bptree_create();

    for (int i = 0; i < N; i++) {
        keys[i] = i + 1;
    }
    srand(42);
    for (int i = N - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        long long tmp = keys[i];
        keys[i] = keys[j];
        keys[j] = tmp;
    }
    for (int i = 0; i < N; i++) {
        assert(bptree_insert(t, keys[i], keys[i] * 10) == BPTREE_OK);
    }
    assert(t->count == N);
    check_all(t, N);
    bptree_destroy(t);
}

// 짝수만 넣으면 홀수는 없어야 한다. 음수 키도 된다.
static void test_missing_and_negative(void)
{
    BPTree *t = bptree_create();
    long off;

    for (long long k = -1000; k <= 1000; k += 2) {
        assert(bptree_insert(t, k, 7) == BPTREE_OK);
    }
    for (long long k = -1000; k <= 1000; k++) {
        assert(bptree_search(t, k, &off) == (k % 2 == 0));
    }
    bptree_destroy(t);
}

// 같은 키를 다시 넣으면 BPTREE_DUPLICATE 이고, 개수와 값은 그대로다.
static void test_duplicate_rejected(void)
{
    BPTree *t = bptree_create();
    long off = 0;

    for (long long k = 1; k <= 1000; k++) {
        assert(bptree_insert(t, k, k * 10) == BPTREE_OK);
    }
    assert(bptree_insert(t, 500, 9999) == BPTREE_DUPLICATE);
    assert(bptree_insert(t, 1, 9999) == BPTREE_DUPLICATE);
    assert(bptree_insert(t, 1000, 9999) == BPTREE_DUPLICATE);
    assert(t->count == 1000);
    assert(bptree_search(t, 500, &off) == 1);
    assert(off == 5000);
    check_all(t, 1000);
    bptree_destroy(t);
}

// 꽉 찬 노드를 지나는 중복 삽입: 나뉠 때 쓰려고 미리 만든 노드(새 루트 포함)를 쓰지 않고 해제해야 한다.
// 해제하지 않으면 make asan 의 LeakSanitizer 가 잡는다.
static void test_duplicate_on_full_nodes(void)
{
    BPTree *t = bptree_create();

    // 루트가 꽉 찬 리프 하나: 리프용 노드와 새 루트를 미리 만들었다가 버린다
    for (long long k = 1; k <= BPTREE_MAX_KEYS; k++) {
        assert(bptree_insert(t, k, k * 10) == BPTREE_OK);
    }
    assert(bptree_insert(t, 1, 0) == BPTREE_DUPLICATE);
    assert(t->count == BPTREE_MAX_KEYS && t->height == 1);
    check_all(t, BPTREE_MAX_KEYS);

    // 다시 넣으면 정상적으로 나뉜다
    assert(bptree_insert(t, BPTREE_MAX_KEYS + 1, (BPTREE_MAX_KEYS + 1) * 10) == BPTREE_OK);
    assert(t->height == 2);
    check_all(t, BPTREE_MAX_KEYS + 1);
    bptree_destroy(t);
}

int main(void)
{
    test_empty();
    test_no_split();
    test_first_split();
    test_ascending();
    test_descending();
    test_shuffled();
    test_missing_and_negative();
    test_duplicate_rejected();
    test_duplicate_on_full_nodes();
    bptree_destroy(NULL);
    printf("test_bptree: all passed\n");
    return 0;
}
