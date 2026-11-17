#ifndef BPTREE_H
#define BPTREE_H

#include <stdbool.h>

typedef struct BptNode BptNode;

typedef enum {
    BPT_OK,
    BPT_DUPLICATE,
    BPT_NO_MEMORY
} BptStatus;

// 빈 트리의 루트는 NULL이다. 실패하면 기존 트리는 그대로다.
BptStatus bpt_insert(BptNode **root, long long id, long offset);
bool bpt_find(const BptNode *root, long long id, long *offset);
void bpt_free(BptNode *root);

#endif
