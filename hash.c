
#include <stdlib.h>
#include <string.h>
#include "hash.h"
#include "primes.h"

#define HASH_MIN 11
#define HASH_MAX 13845163

typedef struct _pHashNode pHashNode;

struct _pHashNode {
    hptr key;
    hptr value;
    pHashNode *next;
};

struct _pHashTable {
    int size;
    int nnodes;
    pHashNode **nodes;
    pHashFunc hash_func;
    pEqualFunc key_equal;
    pDestroyFunc key_destroy;
    pDestroyFunc value_destroy;
};

static unsigned int
pDefaultHash(void *v)
{
    return ((unsigned long) (v));
}

pHashTable *
pHashNew(pHashFunc hf, pEqualFunc eqf)
{
    return pHashNewFull(hf, eqf, NULL, NULL);
}

pHashTable *
pHashNewFull(pHashFunc hf, pEqualFunc eqf, pDestroyFunc kDest, pDestroyFunc vDest)
{
    pHashTable *ht;
    unsigned int i;

    ht = (pHashTable *) malloc(sizeof(pHashTable));
    ht->size = HASH_MIN;
    ht->nnodes = 0;
    ht->hash_func = hf ? hf : pDefaultHash;
    ht->key_equal = eqf;
    ht->key_destroy = kDest;
    ht->value_destroy = vDest;
    ht->nodes = (pHashNode **) malloc(ht->size * sizeof(pHashNode *));
    memset(ht->nodes, 0, ht->size * sizeof(pHashNode *));
    return ht;
}

unsigned int
pHashSize(pHashTable *ht)
{
    if (NULL == ht) return 0;
    return ht->nnodes;
}

static void
pHashResize(pHashTable *ht)
{
    pHashNode **nnodes, *node, *next;
    unsigned int hash;
    int size, i;

    if ((ht->size >= 3 * ht->nnodes && ht->size > HASH_MIN)
	|| (3 * ht->size <= ht->nnodes && ht->size < HASH_MAX)) {
	size = primeGetClosest(ht->nnodes);
	size = size > HASH_MAX ? HASH_MAX : (size < HASH_MIN ? HASH_MIN : size);
	nnodes = (pHashNode **) malloc(size * sizeof(pHashNode *));
	memset(nnodes, 0, size * sizeof(pHashNode *));
	for (i = 0; i < ht->size; i++)
	for (node = ht->nodes[i]; node; node = next) {
	    next = node->next;
	    hash = (* ht->hash_func)(node->key) % size;
	    node->next = nnodes[hash];
	    nnodes[hash] = node;
	}
	free(ht->nodes);
	ht->nodes = nnodes;
	ht->size = size;
    }
}

static pHashNode *
pHashNodeNew(hptr key, hptr value)
{
    pHashNode *node;
    node = (pHashNode *) malloc(sizeof(pHashNode));
    node->key = key;
    node->value = value;
    node->next = NULL;
    return node;
}

static void
pHashNodeDestroy(pHashNode *node, pDestroyFunc kd, pDestroyFunc vd)
{
    if (kd) kd(node->key);
    if (vd) vd(node->value);
    free(node);
}

void
pHashDestroy(pHashTable *ht)
{
    unsigned int i;

    if (NULL == ht) return;

    for (i = 0; i < ht->size; i++) {
	pHashNode *node = ht->nodes[i];
	while (node) {
	    pHashNode *next = node->next;
	    if (ht->key_destroy) ht->key_destroy(node->key);
	    if (ht->value_destroy) ht->value_destroy(node->value);
	    free(node);
	    node = next;
	}
    }

    free(ht->nodes);
    free(ht);
}

static pHashNode **
pHashLookupNode(pHashTable *ht, hptr key)
{
    pHashNode **node;

    node = &ht->nodes[(*ht->hash_func)(key) % ht->size];

    if (ht->key_equal)
	while (*node && !(ht->key_equal)((*node)->key, key))
	    node = &(*node)->next;
    else
	while (*node && (*node)->key != key)
	    node = &(*node)->next;

    return node;
}

hptr
pHashLookup(pHashTable *ht, hptr key)
{
    pHashNode *node;

    if (NULL == ht) return NULL;

    node = *pHashLookupNode(ht, key);
    return node ? node->value : NULL;
}

int
pHashLookupExt(pHashTable *ht, hptr key, hptr *origKey, hptr *value)
{
    pHashNode *node;
    if (NULL == ht) return 0;

    node = *pHashLookupNode(ht, key);
    if (node) {
	if (origKey)  *origKey = node->key;
	if (value)    *value = node->value;
	return 1;
    }
    return 0;
}

void
pHashInsert(pHashTable *ht, hptr key, hptr value)
{
    pHashNode **node;

    if (NULL == ht) return;
    node = pHashLookupNode(ht, key);
    if (*node) {
	if (ht->key_destroy) ht->key_destroy(key);
	if (ht->value_destroy) ht->value_destroy((*node)->value);
	(*node)->value = value;
    } else {
	*node = pHashNodeNew(key, value);
	ht->nnodes++;
	pHashResize(ht);
    }
}

void
pHashReplace(pHashTable *ht, void *key, void *value)
{
    pHashNode **node;

    if (NULL == ht) return;
    node = pHashLookupNode(ht, key);
    if (*node) {
	if (ht->key_destroy) ht->key_destroy((*node)->key);
	if (ht->value_destroy) ht->value_destroy((*node)->value);
	(*node)->key = key;
	(*node)->value = value;
    } else {
	*node = pHashNodeNew(key, value);
	ht->nnodes++;
	pHashResize(ht);
    }
}

int
pHashRemove(pHashTable *ht, hptr key)
{
    pHashNode **node, *dest;

    if (NULL == ht) return 0;
    node = pHashLookupNode(ht, key);
    if (*node) {
	dest = *node;
	(*node) = dest->next;
	pHashNodeDestroy(dest, ht->key_destroy, ht->value_destroy);
	ht->nnodes--;
	pHashResize(ht);
	return 1;
    }
    return 0;
}

int
pHashSteal(pHashTable *ht, hptr key)
{
    pHashNode **node, *dest;

    if (NULL == ht) return 0;
    node = pHashLookupNode(ht, key);
    if (*node) {
	dest = *node;
	(*node) = dest->next;
	pHashNodeDestroy(dest, NULL, NULL);
	ht->nnodes--;
	pHashResize(ht);
	return 1;
    }
    return 0;
}

static unsigned int
pHashForeachRemoveSteal(pHashTable *ht, pHRFunc func, hptr data, int doit)
{
    pHashNode *node, *prev;
    unsigned int i, delCnt = 0;
    pDestroyFunc kdestroy, vdestroy;

    kdestroy = vdestroy = NULL;
    if (doit) {
	kdestroy = ht->key_destroy;
	vdestroy = ht->value_destroy;
    }
    for (i = 0; i < ht->size; i++) {
    restart: prev = NULL;
	for (node = ht->nodes[i]; node; prev = node, node = node->next)
	if ((*func)(node->key, node->value, data)) {
	    delCnt++;
	    ht->nnodes--;
	    if (prev) {
		prev->next = node->next;
		pHashNodeDestroy(node, kdestroy, vdestroy);
		node = prev;
	    } else {
		ht->nodes[i] = node->next;
		pHashNodeDestroy(node, kdestroy, vdestroy);
		goto restart;
	    }
	}
    }
    pHashResize(ht);
    return delCnt;
}

unsigned int
pHashForeachRemove(pHashTable *ht, pHRFunc func, hptr data)
{
    if (NULL == ht) return 0;
    if (NULL == func) return 0;
    return pHashForeachRemoveSteal(ht, func, data, 1);
}

unsigned int
pHashForeachSteal(pHashTable *ht, pHRFunc func, hptr data)
{
    if (NULL == ht) return 0;
    if (NULL == func) return 0;
    return pHashForeachRemoveSteal(ht, func, data, 0);
}

void
pHashForeach(pHashTable *ht, pHFunc func, hptr data)
{
    pHashNode *node;
    int i;

    if (NULL == ht) return;
    if (NULL == func) return;
    for (i = 0; i < ht->size; i++)
    for (node = ht->nodes[i]; node; node = node->next)
	(*func)(node->key, node->value, data);
}

hptr 
pHashFind(pHashTable *ht, pHRFunc pred, void *data)
{
    pHashNode *node;
    int i;
    if (NULL == ht) return 0;
    if (NULL == pred) return 0;
    for (i = 0; i < ht->size; i++)
    for (node = ht->nodes[i]; node; node = node->next)
	if (pred(node->key, node->value, data))
	    return node->value;
    return NULL;
}
