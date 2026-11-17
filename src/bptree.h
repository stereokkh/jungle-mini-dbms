#ifndef BPTREE_H
#define BPTREE_H

// 메모리 기반 B+ 트리. 키는 행의 id, 값은 그 행이 데이터 파일에서 시작하는 offset 이다.
// 행 자체는 트리에 넣지 않는다. 트리로 offset 을 찾고, 파일에서 그 한 줄만 읽는다.
//
// 삽입과 검색만 있다. DELETE 문이 없어서 삭제는 만들지 않았다.

// 노드 하나에 들어가는 키의 최대 개수. 이 수를 넘으면 노드를 둘로 나눈다.
// 클수록 트리가 낮아진다. 128 이면 100만 건이 3~4 단계에 들어간다.
#define BPTREE_MAX_KEYS 128

typedef struct BPNode BPNode;

typedef struct {
    BPNode   *root;     // 처음에는 빈 리프 하나
    long long count;    // 들어 있는 키의 개수
    int       height;   // 루트에서 리프까지의 단계 수. 리프 하나뿐이면 1
} BPTree;

// bptree_insert 의 결과. 트리는 무슨 일이 있었는지만 알려 주고, 어떻게 할지는 부른 쪽이 정한다.
typedef enum {
    BPTREE_OK,          // 넣었다
    BPTREE_DUPLICATE,   // 이미 있는 key 다. 트리는 바뀌지 않았다
    BPTREE_NO_MEMORY    // 메모리가 없다. 트리는 바뀌지 않았다
} BPTreeStatus;

// 빈 트리를 만든다. 메모리가 없으면 NULL.
BPTree *bptree_create(void);

// (key, offset) 을 넣는다.
// 실패해도(BPTREE_DUPLICATE, BPTREE_NO_MEMORY) 트리는 넣기 전 그대로라 계속 쓸 수 있다.
BPTreeStatus bptree_insert(BPTree *tree, long long key, long offset);

// key 를 찾으면 *offset 에 값을 넣고 1, 없으면 0 을 돌려준다.
int bptree_search(const BPTree *tree, long long key, long *offset);

// 모든 노드와 트리를 해제한다. NULL 을 넘겨도 된다.
void bptree_destroy(BPTree *tree);

#endif
