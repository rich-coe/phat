#include "rbt.h"

rbt *
newrbt()
{
    rbt *t = (rbt *) malloc(sizeof(rbt));
    t->mem_ctx = talloc_new(NULL);
    t->t = trbt_create(t->mem_ctx, 0);
    return t;
}

void 
freerbt(rbt *t)
{
    talloc_free(t->mem_ctx);
    free(t);
}
