
typedef struct _pHashTable pHashTable;
typedef void * hptr;

typedef void (*pDestroyFunc)(void * data);
typedef int (*pHRFunc)(hptr key, hptr value, void *data);
typedef void (*pHFunc)(hptr key, hptr value, void *data);
typedef int (*pEqualFunc)(hptr a, hptr b);
typedef unsigned int (*pHashFunc) (hptr key);

pHashTable * pHashNew(pHashFunc hf, pEqualFunc eqf);
pHashTable * pHashNewFull(pHashFunc hf, pEqualFunc eqf, pDestroyFunc kDest, pDestroyFunc vDest);
unsigned int pHashSize(pHashTable *ht);
void pHashDestroy(pHashTable *ht);
hptr pHashLookup(pHashTable *ht, hptr key);
int pHashLookupExt(pHashTable *ht, hptr key, hptr *origKey, hptr *value);
void pHashInsert(pHashTable *ht, hptr key, hptr value);
void pHashReplace(pHashTable *ht, hptr key, hptr value);
int pHashRemove(pHashTable *ht, hptr key);
int pHashSteal(pHashTable *ht, hptr key);
unsigned int pHashForeachRemove(pHashTable *ht, pHRFunc func, void *data);
unsigned int pHashForeachSteal(pHashTable *ht, pHRFunc func, void *data);
void pHashForeach(pHashTable *ht, pHFunc func, void *data);
hptr pHashFind(pHashTable *ht, pHRFunc pred, void *data);
