#include "talloc.h"
#include "rbtree.h"

struct _rbt {
    trbt_tree_t *t;
};
typedef struct _rbt rbt;

#define rbtInsert(x, k, v)      trbt_insert32((x)->t, k, v)
#define rbtLookup(x, k)         trbt_lookup32((x)->t, k)

rbt * newrbt();
void freerbt(rbt *t);
