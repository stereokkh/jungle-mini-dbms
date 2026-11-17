#include "bptree.h"

#include <stdlib.h>
#include <string.h>

enum { MAX_KEYS = 31 };

struct BptNode {
    bool leaf;
    int count;
    // 분할 직전에는 키와 자식이 최대 개수보다 하나씩 더 들어간다.
    long long keys[MAX_KEYS + 1];
    union {
        struct {
            long offsets[MAX_KEYS + 1];
            BptNode *next;
        } rows;
        BptNode *children[MAX_KEYS + 2];
    };
};

typedef struct {
    long long key;
    BptNode *right;
} Split;

static BptNode *new_node(bool leaf)
{
    BptNode *node = malloc(sizeof *node);
    if (node != NULL) {
        node->leaf = leaf;
        node->count = 0;
        if (leaf) node->rows.next = NULL;
    }
    return node;
}

// 리프에서는 id 이상인 첫 슬롯, 내부 노드에서는 id보다 큰 첫 구분 키를 찾는다.
static int key_slot(const BptNode *node, long long id)
{
    int lo = 0, hi = node->count;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        long long key = node->keys[mid];
        if (key < id || (!node->leaf && key == id)) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

bool bpt_find(const BptNode *root, long long id, long *offset)
{
    const BptNode *node = root;
    if (node == NULL) return false;
    while (!node->leaf) {
        node = node->children[key_slot(node, id)];
    }
    int slot = key_slot(node, id);
    if (slot == node->count || node->keys[slot] != id) return false;
    *offset = node->rows.offsets[slot];
    return true;
}

static Split split_node(BptNode *left, BptNode *right)
{
    int mid = left->count / 2;
    // 리프의 구분 키는 오른쪽 리프에도 남는다. 내부 노드의 구분 키는 부모로 이동한다.
    int first_right_key = left->leaf ? mid : mid + 1;
    right->count = left->count - first_right_key;
    memcpy(right->keys, left->keys + first_right_key,
           (size_t)right->count * sizeof right->keys[0]);

    if (left->leaf) {
        memcpy(right->rows.offsets, left->rows.offsets + mid,
               (size_t)right->count * sizeof right->rows.offsets[0]);
        right->rows.next = left->rows.next;
        left->rows.next = right;
    } else {
        memcpy(right->children, left->children + first_right_key,
               (size_t)(right->count + 1) * sizeof right->children[0]);
    }
    left->count = mid;
    return (Split){left->keys[mid], right};
}

static BptStatus insert_node(BptNode *node, long long id, long offset, Split *split)
{
    int slot = key_slot(node, id);
    if (node->leaf && slot < node->count && node->keys[slot] == id) {
        return BPT_DUPLICATE;
    }

    // 자식을 바꾸기 전에 분할 공간을 확보해 할당 실패 시 트리가 바뀌지 않게 한다.
    BptNode *right = NULL;
    if (node->count == MAX_KEYS) {
        right = new_node(node->leaf);
        if (right == NULL) return BPT_NO_MEMORY;
    }

    long long key = id;
    size_t tail = (size_t)(node->count - slot);
    if (node->leaf) {
        memmove(node->rows.offsets + slot + 1, node->rows.offsets + slot,
                tail * sizeof node->rows.offsets[0]);
        node->rows.offsets[slot] = offset;
    } else {
        Split child = {0};
        BptStatus status = insert_node(node->children[slot], id, offset, &child);
        if (child.right == NULL) {
            free(right);
            return status;
        }
        key = child.key;
        memmove(node->children + slot + 2, node->children + slot + 1,
                tail * sizeof node->children[0]);
        node->children[slot + 1] = child.right;
    }

    memmove(node->keys + slot + 1, node->keys + slot, tail * sizeof node->keys[0]);
    node->keys[slot] = key;
    node->count++;
    if (right != NULL) *split = split_node(node, right);
    return BPT_OK;
}

BptStatus bpt_insert(BptNode **root, long long id, long offset)
{
    if (*root == NULL) {
        *root = new_node(true);
        if (*root == NULL) return BPT_NO_MEMORY;
    }

    BptNode *new_root = NULL;
    if ((*root)->count == MAX_KEYS) {
        new_root = new_node(false);
        if (new_root == NULL) return BPT_NO_MEMORY;
    }

    Split split = {0};
    BptStatus status = insert_node(*root, id, offset, &split);
    if (split.right == NULL) {
        free(new_root);
    } else {
        new_root->keys[0] = split.key;
        new_root->children[0] = *root;
        new_root->children[1] = split.right;
        new_root->count = 1;
        *root = new_root;
    }
    return status;
}

void bpt_free(BptNode *root)
{
    if (root == NULL) return;
    if (!root->leaf) {
        for (int i = 0; i <= root->count; i++) bpt_free(root->children[i]);
    }
    free(root);
}
