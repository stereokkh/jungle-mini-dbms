#include "bptree.h"

#include <stdlib.h>

// 노드는 두 종류다.
//   리프      : keys[i] 의 값이 offsets[i]. 리프끼리는 next 로 왼쪽에서 오른쪽으로 이어진다.
//   내부 노드 : 값은 없고 길 안내만 한다. 키가 n 개면 자식은 n+1 개다.
//               children[i] 아래에는 keys[i-1] 이상, keys[i] 미만인 키만 있다.
//
// 배열을 한 칸씩 더 크게 잡는다. 꽉 찬 노드에도 일단 넣고 나서 나누면
// "넣을 자리 찾기"와 "나누기"를 따로 생각할 수 있어 코드가 단순해진다.
struct BPNode {
    int       is_leaf;
    int       key_count;
    long long keys[BPTREE_MAX_KEYS + 1];
    union {                                      // 노드 종류에 따라 둘 중 하나만 쓴다
        long    offsets[BPTREE_MAX_KEYS + 1];    // 리프
        BPNode *children[BPTREE_MAX_KEYS + 2];   // 내부 노드
    };
    BPNode   *next;                              // 리프 전용: 오른쪽 이웃 리프
};

// 노드가 나뉘었을 때 부모에게 알려 줄 내용.
// 부모는 key 와 right 를 자기 안에 끼워 넣는다.
typedef struct {
    int       happened;   // 0 이면 나뉘지 않았다
    long long key;        // right 쪽에 있는 가장 작은 키 (부모의 길 안내 키가 된다)
    BPNode   *right;      // 새로 생긴 오른쪽 노드
} Split;

static BPNode *node_new(int is_leaf)
{
    BPNode *n = calloc(1, sizeof *n);
    if (n != NULL) {
        n->is_leaf = is_leaf;
    }
    return n;
}

// 리프에서 key 가 있어야 할 자리: key 이상인 첫 키의 위치.
static int leaf_position(const BPNode *leaf, long long key)
{
    int i = 0;
    while (i < leaf->key_count && leaf->keys[i] < key) {
        i++;
    }
    return i;
}

// 내부 노드에서 key 를 찾으러 내려갈 자식의 번호: key 이하인 키의 개수.
// 키가 [10, 20] 이면 5 → 0번, 10 → 1번, 15 → 1번, 20 → 2번 자식.
static int child_index(const BPNode *node, long long key)
{
    int i = 0;
    while (i < node->key_count && node->keys[i] <= key) {
        i++;
    }
    return i;
}

// 넘친 리프의 뒤쪽 절반을 새 리프로 옮긴다.
// 리프의 키는 값과 짝이라 없앨 수 없다. 그래서 오른쪽 첫 키를 부모에게 "복사"해 올린다.
static int split_leaf(BPNode *leaf, Split *split)
{
    BPNode *right = node_new(1);
    if (right == NULL) {
        return -1;
    }

    int mid = leaf->key_count / 2;
    for (int i = mid; i < leaf->key_count; i++) {
        right->keys[i - mid]    = leaf->keys[i];
        right->offsets[i - mid] = leaf->offsets[i];
    }
    right->key_count = leaf->key_count - mid;
    leaf->key_count  = mid;

    // 리프 연결: leaf → right → (leaf 의 원래 이웃)
    right->next = leaf->next;
    leaf->next  = right;

    split->happened = 1;
    split->key      = right->keys[0];
    split->right    = right;
    return 0;
}

// 넘친 내부 노드를 둘로 나눈다.
// 내부 노드의 키는 길 안내일 뿐이라, 가운데 키는 어느 쪽에도 남기지 않고 부모로 "옮겨" 올린다.
static int split_internal(BPNode *node, Split *split)
{
    BPNode *right = node_new(0);
    if (right == NULL) {
        return -1;
    }

    int mid = node->key_count / 2;
    for (int i = mid + 1; i < node->key_count; i++) {
        right->keys[i - mid - 1] = node->keys[i];
    }
    for (int i = mid + 1; i <= node->key_count; i++) {
        right->children[i - mid - 1] = node->children[i];
    }
    right->key_count = node->key_count - mid - 1;

    split->happened = 1;
    split->key      = node->keys[mid];
    split->right    = right;

    node->key_count = mid;
    return 0;
}

// 리프에 (key, offset) 을 넣는다. 새 키를 넣었으면 *added 를 1 로 만든다.
static int insert_into_leaf(BPNode *leaf, long long key, long offset, Split *split, int *added)
{
    int pos = leaf_position(leaf, key);

    // 이미 있는 키면 값만 바꾼다
    if (pos < leaf->key_count && leaf->keys[pos] == key) {
        leaf->offsets[pos] = offset;
        return 0;
    }

    // pos 부터 한 칸씩 뒤로 밀어 자리를 만든다
    for (int i = leaf->key_count; i > pos; i--) {
        leaf->keys[i]    = leaf->keys[i - 1];
        leaf->offsets[i] = leaf->offsets[i - 1];
    }
    leaf->keys[pos]    = key;
    leaf->offsets[pos] = offset;
    leaf->key_count++;
    *added = 1;

    if (leaf->key_count > BPTREE_MAX_KEYS) {
        return split_leaf(leaf, split);
    }
    return 0;
}

// node 아래에 (key, offset) 을 넣는다. node 가 나뉘면 split 에 적어 부모에게 알린다.
static int insert_into(BPNode *node, long long key, long offset, Split *split, int *added)
{
    split->happened = 0;

    if (node->is_leaf) {
        return insert_into_leaf(node, key, offset, split, added);
    }

    // 내부 노드: 알맞은 자식으로 내려간다
    int   idx = child_index(node, key);
    Split child_split;
    if (insert_into(node->children[idx], key, offset, &child_split, added) != 0) {
        return -1;
    }
    if (!child_split.happened) {
        return 0;
    }

    // 자식이 나뉘었다. 올라온 키와 새 오른쪽 자식을 idx 자리에 끼워 넣는다.
    for (int i = node->key_count; i > idx; i--) {
        node->keys[i]         = node->keys[i - 1];
        node->children[i + 1] = node->children[i];
    }
    node->keys[idx]         = child_split.key;
    node->children[idx + 1] = child_split.right;
    node->key_count++;

    if (node->key_count > BPTREE_MAX_KEYS) {
        return split_internal(node, split);
    }
    return 0;
}

BPTree *bptree_create(void)
{
    BPTree *tree = malloc(sizeof *tree);
    if (tree == NULL) {
        return NULL;
    }
    tree->root = node_new(1);
    if (tree->root == NULL) {
        free(tree);
        return NULL;
    }
    tree->count  = 0;
    tree->height = 1;
    return tree;
}

int bptree_insert(BPTree *tree, long long key, long offset)
{
    Split split;
    int   added = 0;

    if (insert_into(tree->root, key, offset, &split, &added) != 0) {
        return -1;
    }
    tree->count += added;

    // 루트까지 나뉘었으면 그 위에 새 루트를 얹는다. 트리는 이때만, 위쪽으로 한 단계 자란다.
    // 그래서 모든 리프가 항상 같은 깊이에 있다.
    if (split.happened) {
        BPNode *new_root = node_new(0);
        if (new_root == NULL) {
            return -1;
        }
        new_root->keys[0]     = split.key;
        new_root->children[0] = tree->root;
        new_root->children[1] = split.right;
        new_root->key_count   = 1;
        tree->root = new_root;
        tree->height++;
    }
    return 0;
}

int bptree_search(const BPTree *tree, long long key, long *offset)
{
    const BPNode *node = tree->root;

    // 리프에 닿을 때까지 내려간다
    while (!node->is_leaf) {
        node = node->children[child_index(node, key)];
    }

    int pos = leaf_position(node, key);
    if (pos < node->key_count && node->keys[pos] == key) {
        *offset = node->offsets[pos];
        return 1;
    }
    return 0;
}

static void node_free(BPNode *node)
{
    if (!node->is_leaf) {
        for (int i = 0; i <= node->key_count; i++) {
            node_free(node->children[i]);
        }
    }
    free(node);
}

void bptree_destroy(BPTree *tree)
{
    if (tree == NULL) {
        return;
    }
    node_free(tree->root);
    free(tree);
}
