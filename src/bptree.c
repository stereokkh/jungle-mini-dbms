#include "bptree.h"

#include <stdlib.h>

// 노드는 두 종류다.
//   리프      : keys[i] 의 값이 offsets[i]. 리프끼리는 next 로 왼쪽에서 오른쪽으로 이어진다.
//   내부 노드 : 값은 없고 길 안내만 한다. 키가 n 개면 자식은 n+1 개다.
//               children[i] 아래에는 keys[i-1] 이상, keys[i] 미만인 키만 있다.
//
// 배열을 한 칸씩 더 크게 잡는다. 꽉 찬 노드에도 일단 넣고 나서 나누면
// "넣을 자리 찾기"와 "나누기"를 따로 생각할 수 있어 코드가 단순해진다.
//
// 메모리 부족 대비: 나눌 때 필요한 새 노드는 트리를 바꾸기 "전에" 미리 만들어 둔다.
// 꽉 찬 노드(키가 BPTREE_MAX_KEYS 개)만 나뉠 수 있으므로, 내려가는 길에 꽉 찬 노드를 만날 때마다
// 오른쪽 노드 하나를 만든다. 만들다 실패하면 아직 아무것도 바꾸지 않았으니 그냥 돌아가면 된다.
// 결국 나뉘지 않아 쓰지 않은 노드는 해제한다.
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

// 이 노드가 꽉 찼으면, 나뉠 때 쓸 오른쪽 노드를 미리 만든다.
// 꽉 차지 않았으면 *spare 는 NULL 이고 할 일이 없다. 만들다 실패하면 -1.
static int prepare_spare(const BPNode *node, BPNode **spare)
{
    *spare = NULL;
    if (node->key_count < BPTREE_MAX_KEYS) {
        return 0;
    }
    *spare = node_new(node->is_leaf);
    return *spare == NULL ? -1 : 0;
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

// 넘친 리프의 뒤쪽 절반을 미리 만들어 둔 빈 리프 right 로 옮긴다.
// 리프의 키는 값과 짝이라 없앨 수 없다. 그래서 오른쪽 첫 키를 부모에게 "복사"해 올린다.
static void split_leaf(BPNode *leaf, BPNode *right, Split *split)
{
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
}

// 넘친 내부 노드를 둘로 나눈다. 뒤쪽 절반은 미리 만들어 둔 빈 노드 right 로 옮긴다.
// 내부 노드의 키는 길 안내일 뿐이라, 가운데 키는 어느 쪽에도 남기지 않고 부모로 "옮겨" 올린다.
static void split_internal(BPNode *node, BPNode *right, Split *split)
{
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
}

// 리프에 (key, offset) 을 넣는다.
static BPTreeStatus insert_into_leaf(BPNode *leaf, long long key, long offset, Split *split)
{
    int pos = leaf_position(leaf, key);
    BPNode *spare;

    if (pos < leaf->key_count && leaf->keys[pos] == key) {
        return BPTREE_DUPLICATE;
    }
    if (prepare_spare(leaf, &spare) != 0) {
        return BPTREE_NO_MEMORY;
    }

    // 여기부터는 실패할 일이 없다. pos 부터 한 칸씩 뒤로 밀어 자리를 만든다.
    for (int i = leaf->key_count; i > pos; i--) {
        leaf->keys[i]    = leaf->keys[i - 1];
        leaf->offsets[i] = leaf->offsets[i - 1];
    }
    leaf->keys[pos]    = key;
    leaf->offsets[pos] = offset;
    leaf->key_count++;

    // 꽉 찬 리프에 넣었으면 spare 가 있고, 지금 키가 한 개 넘친다
    if (spare != NULL) {
        split_leaf(leaf, spare, split);
    }
    return BPTREE_OK;
}

// node 아래에 (key, offset) 을 넣는다. node 가 나뉘면 split 에 적어 부모에게 알린다.
static BPTreeStatus insert_into(BPNode *node, long long key, long offset, Split *split)
{
    split->happened = 0;

    if (node->is_leaf) {
        return insert_into_leaf(node, key, offset, split);
    }

    // 내려가기 전에, 이 노드가 나뉠 때 쓸 노드를 먼저 만든다
    BPNode *spare;
    if (prepare_spare(node, &spare) != 0) {
        return BPTREE_NO_MEMORY;
    }

    // 내부 노드: 알맞은 자식으로 내려간다
    int   idx = child_index(node, key);
    Split child_split;
    BPTreeStatus st = insert_into(node->children[idx], key, offset, &child_split);
    if (st != BPTREE_OK || !child_split.happened) {
        free(spare);    // 실패했거나 자식이 나뉘지 않았으면 이 노드도 나뉘지 않는다
        return st;
    }

    // 자식이 나뉘었다. 올라온 키와 새 오른쪽 자식을 idx 자리에 끼워 넣는다.
    for (int i = node->key_count; i > idx; i--) {
        node->keys[i]         = node->keys[i - 1];
        node->children[i + 1] = node->children[i];
    }
    node->keys[idx]         = child_split.key;
    node->children[idx + 1] = child_split.right;
    node->key_count++;

    if (spare != NULL) {
        split_internal(node, spare, split);
    }
    return BPTREE_OK;
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

BPTreeStatus bptree_insert(BPTree *tree, long long key, long offset)
{
    Split split;

    // 루트가 꽉 찼으면 루트가 나뉠 수 있다. 그때 위에 얹을 새 루트도 미리 만든다.
    BPNode *new_root = NULL;
    if (tree->root->key_count == BPTREE_MAX_KEYS) {
        new_root = node_new(0);
        if (new_root == NULL) {
            return BPTREE_NO_MEMORY;
        }
    }

    BPTreeStatus st = insert_into(tree->root, key, offset, &split);
    if (st != BPTREE_OK || !split.happened) {
        free(new_root);
        if (st == BPTREE_OK) {
            tree->count++;
        }
        return st;
    }

    // 루트까지 나뉘었으면 그 위에 새 루트를 얹는다. 트리는 이때만, 위쪽으로 한 단계 자란다.
    // 그래서 모든 리프가 항상 같은 깊이에 있다.
    new_root->keys[0]     = split.key;
    new_root->children[0] = tree->root;
    new_root->children[1] = split.right;
    new_root->key_count   = 1;
    tree->root = new_root;
    tree->height++;
    tree->count++;
    return BPTREE_OK;
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
