#include <assert.h>
#include <limits.h>
#include <stdio.h>

#include "bptree.h"

int main(void)
{
    enum { ROWS = 4096 };
    BptNode *root = NULL;
    long offset;
    assert(!bpt_find(root, 0, &offset));

    // 서로 다른 위치에 삽입해 리프와 내부 노드의 분할을 일으킨다.
    for (int i = 0; i < ROWS; i++) {
        int key = i * 1999 % ROWS;
        assert(bpt_insert(&root, key * 2LL, key * 17L) == BPT_OK);
    }
    assert(bpt_insert(&root, LLONG_MIN, 42) == BPT_OK);
    assert(bpt_insert(&root, LLONG_MAX, 43) == BPT_OK);
    assert(bpt_insert(&root, 0, 123) == BPT_DUPLICATE);

    for (int key = 0; key < ROWS; key++) {
        assert(bpt_find(root, key * 2LL, &offset) && offset == key * 17L);
        assert(!bpt_find(root, key * 2LL + 1, &offset));
    }
    assert(bpt_find(root, LLONG_MIN, &offset) && offset == 42);
    assert(bpt_find(root, LLONG_MAX, &offset) && offset == 43);
    bpt_free(root);
    puts("test_bptree: ok");
    return 0;
}
