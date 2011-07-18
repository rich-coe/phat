#include "rbt.h"

rbt *
newrbt()
{
    rbt *t = (rbt *) talloc(NULL, rbt);
    t->t = trbt_create(t, 0);
    return t;
}

void 
freerbt(rbt *t)
{
    talloc_free(t);
}
