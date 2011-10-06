/* parse java hprof results file */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <endian.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "talloc.h"
#include "rbtree.h"

#define _FILE_OFFSET_BITS 64
#define MAGIC_HEADER 0x4a415641   // JAVA

struct jdump {          // java dump
    FILE *fin;
    int fVersion;
    unsigned int identsz;
    trbt_tree_t *sbTable,               // string const table
        *cTable,                // class table, by ident
        *hTable,                // object table
        *rsbTable,              // revers string table, by name
        *rcTable;               // reverse class table, by name
    struct _cinfo *javaLangClass, *javaLangString, *javaLangClassLoader;
    char *fclass;
};

union hvalue {
    long long ident;
    unsigned char b;
    unsigned short c;
    unsigned int i;
    unsigned long long j;
    float f;
    double d;
};

struct _hobject {           // heap object
#define H_INSTANCE 0x01
#define H_VARRAY   0x02
#define H_OARRAY   0x03
    int htype;
    long long instId, classId;
    long fpos;
    char resolved, visit;
    unsigned int count;
    int size;
    union hvalue *hvalues;
};
typedef struct _hobject hobject;

struct _finfo {         // Field Info
    long long ident;    // field identifier
    char *name;         // [decoded] field name
    char ftype;         // field type
    char resolved;      // passed through resolver
    int valsz;          // sizeof object
    off_t offset;       // for resolved objects
};
typedef struct _finfo finfo;

struct _cinfo {         // Class Info
    long long ident, nident;
    char *name;
    unsigned long count;
    long long superId, loaderId, signerId, domainId;
    unsigned short cstats, cfields;
    int tfields;
    char resolved;
    hobject *statics;
    finfo *fields;              // this class's fields, self
    finfo **values;             // this class's value fields, self + super(s)
};
typedef struct _cinfo cinfo;

int readVersion(FILE *fin);
int readUint(FILE *fin, unsigned int *input);
int readUlong(FILE *fin, unsigned long long *input);
long long readIdent(struct jdump *);
struct jdump *readDump(char *findclass, char *dumpfile);
void readHeap(struct jdump *, unsigned int hsize);
char *hideSpecials(char *);
void resolveInstance(struct jdump *jf, hobject *ho);

int debug = 0;

extern int optind;

cinfo *
mkcinfo(trbt_tree_t *tab, long long cid, long long nid, char *name)
{
    cinfo *ci = (cinfo *) talloc(tab, cinfo);
    ci->ident = cid;
    ci->nident = nid;
    ci->name = name;
    ci->tfields = ci->resolved = ci->count = 0;
    return ci;
}

int
main(int argc, char **argv)
{
    int opt;
    char *baseline = NULL;
    char *findclass = NULL;
    struct jdump *df, *bf;

    while (-1 != (opt = getopt(argc, argv, "b:C:d"))) {
    switch (opt) {
    case 'b': baseline = strdup(optarg); break;
    case 'd': debug++; break;
    case 'C': findclass = strdup(optarg); break;
    default: 
        printf("opt %d\n");
        break;
    }
    }

    df = readDump(findclass, argv[optind]);
    if (baseline) {
        bf = readDump(NULL, baseline);
    }

    exit(0);
}

struct jdump *
readDump(char *fclass, char *dumpfile) 
{
    struct jdump *df;
    int rc;
    unsigned int magic;
    unsigned long long date;
    time_t tdate;
    struct tm *ltime;
    char rtype;

    df = (struct jdump *) malloc(sizeof(struct jdump));

    df->fclass = fclass;

    if (NULL == (df->fin = fopen(dumpfile, "r"))) {
        fprintf(stderr, "cannot open '%s' for reading, errno %d\n", dumpfile, errno);
        exit(1);
    }

    // magic number
    if (rc = readUint(df->fin, &magic)) {
        fprintf(stderr, "cannot read header from '%s' for reading, errno %d\n", dumpfile, errno);
        exit(2);
    }

    if (MAGIC_HEADER != magic) {
        fprintf(stderr, "header 0x%x from '%s' does not match 0x%x\n", magic, dumpfile, MAGIC_HEADER);
        exit(2);
    }

    // profile version
    if (-1 == (df->fVersion = readVersion(df->fin))) {
        fprintf(stderr, "unknown version\n");
        exit(3);
    }

    // data
    if (rc = readUint(df->fin, &df->identsz)) {
        fprintf(stderr, "cannot read ident size from '%s' for reading, errno %d\n", dumpfile, errno);
        exit(2);
    }

    if (4 != df->identsz && 8 != df->identsz) {
        fprintf(stderr, "invalid identifer size %d\n", df->identsz);
        exit(4);
    }

    if (rc = readUlong(df->fin, &date)) {
        fprintf(stderr, "cannot read date from '%s' for reading, errno %d\n", dumpfile, errno);
        exit(2);
    }
    tdate = date / 1000;
    ltime = localtime(&tdate);
    printf("Dump file created %s\n\n", ctime(&tdate));

    df->sbTable = trbt_create(NULL, 0);    // table of strings
    // df->rsbTable = trbt_create(NULL, 0);   // reverse lookup of sbTable
    df->cTable = trbt_create(NULL, 0);     // table of classes
    df->rcTable = trbt_create(NULL, 0);    // reverse lookup of cTable
    df->hTable = trbt_create(NULL, 0);     // table of heap objs

    while (EOF != (rtype = getc(df->fin))) {
        unsigned int rlen, ts, pos;

        pos = ftell(df->fin) - 1;

        if (rc = readUint(df->fin, &ts)) {
            puts("");
            fprintf(stderr, "cannot read timestamp from '%s' for reading, errno %d\n", dumpfile, errno);
            exit(2);
        }

        if (rc = readUint(df->fin, &rlen)) {
            puts("");
            fprintf(stderr, "cannot read record length type 0x%x from '%s' for reading, errno %d\n", rtype, dumpfile, errno);
            exit(2);
        }

        if (debug > 2)
            printf("0x%08x Read record 0x%02x : 0x%08x\t", pos, rtype, rlen);

        switch ((unsigned char) rtype) {
        case 0x01 : { // HPROF_UTF8
            unsigned long skey = crc32(0, Z_NULL, 0);;
            long long ident = readIdent(df);
            int  slen = rlen - df->identsz;
            char *usb = talloc_zero_array(df->sbTable, char, 1 + rlen - df->identsz);
            rc = fread(usb, 1, slen, df->fin);
            usb = hideSpecials(usb);
            if (debug)
            printf("0x%8x %s\n", ident, usb);
            trbt_insert32(df->sbTable, (long) ident, usb);
            // trbt_insert32(df->rsbTable, crc32(skey, usb, slen), (void *) ident);
            break;
        }
        case 0x02 : { // HPROF_LOAD_CLASS
            unsigned long ckey = crc32(0, Z_NULL, 0);
            long key, nkey;
            cinfo *ci;
            int serial, stackNum;
            long long classId, classNameId;

            readUint(df->fin, &serial);             // serialNumber
            classId = readIdent(df);  // classId
            readUint(df->fin, &stackNum);           // stackTraceSerialNumber
            classNameId = readIdent(df); // classNameID

            // name = getNameFromID(classNameId);
            // classNameFromObjectID
            // classNameFromSerialNumber 

            key = (long) classId;
            nkey = (long) classNameId;
            ci = mkcinfo(df->cTable, classId, classNameId, (char *) trbt_lookup32(df->sbTable, nkey));
            trbt_insert32(df->cTable, key, ci);
            ckey = crc32(ckey, ci->name, strlen(ci->name));
            trbt_insert32(df->rcTable, ckey, ci);
            if (debug) {
            if (4 < df->identsz)
                printf("0x%016lx classId 0x%08x 0x%016lx %s\n", classId, serial, classNameId, ci->name);
            else
                printf("0x%08lx classId 0x%08x 0x%08lx %s\n", classId, serial, classNameId, ci->name);
            }
            break;
        }
        case 0x05 : { // HPROF_TRACE
            int i, serial, threadSeq, frames;
            readUint(df->fin, &serial);             // serialNumber
            readUint(df->fin, &threadSeq);
            readUint(df->fin, &frames);
            if (debug)
            printf("trace serial %d %d %d\n", serial, threadSeq, frames);
            for (i = 0; i < frames; i++) {
                long long ident = readIdent(df);
                if (debug)
                    printf("\tident 0x%x\n", ident);
            }
            break;
        }
        case 0x0c : { // HPROF_HEAP_DUMP
            // printf("heap dump\n");
            readHeap(df, rlen);
            break;
        }
        default:
            if (debug) puts("");
            fseek(df->fin, rlen, SEEK_CUR);
            break;
        }
    }
    puts("");
}

char *
hideSpecials(char *in)
{
    char *out = in;
    while (*in) {
        if ('\n' == *in || '\r' == *in || 0x20 > *in)
            *in = '.';
        in++;
    }
    return out;
}

int
readUshort(FILE *fin, unsigned short *input)
{
    unsigned char sb[2];
    int rc;
    unsigned short val;

    if (2 != (rc = fread(sb, 1, 2, fin))) {
        if (0 != rc)
            fprintf(stderr, "readUint: short read %d of 2 bytes\n", rc);
        return -1;
    }

#if __BYTE_ORDER == __BIGENDIAN
    val = sb[1] << 8 | sb[0];
#else
    val = sb[0] << 8 | sb[1];
#endif
    *input = val;
    return 0;
}

int
readUint(FILE *fin, unsigned int *input)
{
    unsigned char sb[4];
    int rc;
    unsigned int val;

    if (4 != (rc = fread(sb, 1, 4, fin))) {
        if (0 != rc)
            fprintf(stderr, "readUint: short read %d of 4 bytes\n", rc);
        return -1;
    }

#if __BYTE_ORDER == __BIGENDIAN
    val = ((sb[3] << 8 | sb[2]) << 8 | sb[1]) << 8 | sb[0];
#else
    val = ((sb[0] << 8 | sb[1]) << 8 | sb[2]) << 8 | sb[3];
#endif
    *input = val;
    return 0;
}

int
readUlong(FILE *fin, unsigned long long *input)
{
    unsigned char sb[8];
    int rc;
    unsigned long long val;

    if (8 != (rc = fread(sb, 1, 8, fin))) {
        if (0 != rc)
            fprintf(stderr, "readUint: short read %d of 8 bytes\n", rc);
        return -1;
    }

#if __BYTE_ORDER == __BIGENDIAN
    val = ((sb[3] << 8 | sb[2]) << 8 | sb[1]) << 8 | sb[0];
    val = (val << 32) + ((sb[3] << 8 | sb[2]) << 8 | sb[1]) << 8 | sb[0];
#else
    val = ((sb[0] << 8 | sb[1]) << 8 | sb[2]) << 8 | sb[3];
    val = (val << 32) + ((sb[4] << 8 | sb[5]) << 8 | sb[6]) << 8 | sb[7];
#endif
    *input = val;
    return 0;
}

long long
readIdent(struct jdump *jf)
{
    unsigned long long lval;

    if (4 == jf->identsz) {
        unsigned int val;
        readUint(jf->fin, &val);
        return (long long) val;
    }
    readUlong(jf->fin, &lval);
    return (long long) lval;
}

hobject *
makeObj(trbt_tree_t *tab, int ht, long long iid, long long cid)
{
    hobject *ho = (hobject *) talloc(tab, hobject);

    ho->htype = ht;
    ho->classId = cid;
    ho->instId = iid;
    ho->fpos = 0;
    ho->resolved = ho->visit = 0;
    return ho;
}

int
readVersion(FILE *fin)
{
    char sb[40];
    int ch, len = 0;

    while (EOF != (ch = getc(fin))) {
        sb[len++] = ch;
        if (0 == ch)
            break;
        if (sizeof(sb) < len + 2) {
            sb[1+len] = 0;
            break;
        }
    }
    if (0 < len) {
        if (0 == strcmp(sb, " PROFILE 1.0.2"))
            return 2;
        if (0 == strcmp(sb, " PROFILE 1.0.1"))
            return 1;
        if (0 == strcmp(sb, " PROFILE 1.0"))
            return 0;
    }
    return -1;
}

unsigned char 
sigFromType(unsigned char otype)
{
    switch (otype) {
    case  2 /* T_CLASS */ :      return 'L';
    case  4 /* T_BOOLEAN */ :    return 'Z';
    case  5 /* T_CHAR */ :       return 'C';
    case  6 /* T_FLOAT */ :      return 'F';
    case  7 /* T_DOUBLE */ :     return 'D';
    case  8 /* T_BYTE */ :       return 'B';
    case  9 /* T_SHORT */ :      return 'S';
    case 10 /* T_INT */ :        return 'I';
    case 11 /* T_LONG */ :       return 'J';
    }
    return 'U';
}

int
readValue(struct jdump *jf, hobject *ho)
{
    ho->htype = getc(jf->fin);

    if (1 <= jf->fVersion)
        ho->htype = sigFromType(ho->htype);

    switch (ho->htype) {
    case '[' :
    case 'L' : {
        ho->hvalues[0].ident = readIdent(jf);
        return jf->identsz;
    }
    case 'Z' : {
        ho->hvalues[0].b = getc(jf->fin);
        if (0 != ho->hvalues[0].b && 1 != ho->hvalues[0].b) {
            fprintf(stderr, "readValue: illegal bool read 0x%x\n", ho->hvalues[0].b);
            return -1;
        }
        return 1;
    }
    case 'B' : {
        ho->hvalues[0].b = getc(jf->fin);
        return 1;
    }
    case 'S' : {
        readUshort(jf->fin, &(ho->hvalues[0].c));
        return 2;
    }
    case 'C' : {
        readUshort(jf->fin, &(ho->hvalues[0].c));
        return 2;
    }
    case 'I' : {
        readUint(jf->fin, &(ho->hvalues[0].i));
        return 4;
    }
    case 'J' : {
        readUlong(jf->fin, &(ho->hvalues[0].j));
        return 8;
    }
    case 'F' : {                // float
        ho->hvalues[0].f = 0.0;
        fseek(jf->fin, 4, SEEK_CUR);
        return 4;
    }
    case 'D' : {                // double
        ho->hvalues[0].d = 0.0;
        fseek(jf->fin, 8, SEEK_CUR);
        return 8;
    }
    default:
    fprintf(stderr, "readValue: bad type sig 0x%x\n", ho->htype);
    break;
    }
    return 0;
}

int
readClass(struct jdump *jf, unsigned int hsize)
{
    long long ident, res1, res2;
    unsigned int stackId, instsz;
    unsigned short cpool;
    cinfo *ci;
    int i;

    ident = readIdent(jf);
    ci = (cinfo *) trbt_lookup32(jf->cTable, (long) ident);

    readUint(jf->fin, &stackId);
    ci->superId = readIdent(jf);
    ci->loaderId = readIdent(jf);
    ci->signerId = readIdent(jf);
    ci->domainId = readIdent(jf);
    res1 = readIdent(jf);
    res2 = readIdent(jf);
    readUint(jf->fin, &instsz);
    hsize -= (7 * jf->identsz) + 8;

    readUshort(jf->fin, &cpool);             // const pool entries
    hsize -= 2;
    for (i = 0; i < cpool; i++) {
        hobject sho;
        union hvalue hv;
        unsigned short entry;

        sho.hvalues = &hv;
        readUshort(jf->fin, &entry);
        hsize -= 2;
        hsize -= readValue(jf, &sho);
    }

    readUshort(jf->fin, &(ci->cstats));             // statics
    hsize -= 2;
    if (0 < ci->cstats) {
        ci->statics = talloc_array(ci, hobject, ci->cstats);
        for (i = 0; i < ci->cstats; i++) {
            ci->statics[i].instId = readIdent(jf);
            ci->statics[i].resolved = 0;
            ci->statics[i].hvalues = talloc(ci, union hvalue);
            hsize -= jf->identsz;
            hsize -= 1 + readValue(jf, ci->statics + i);
        }
    }

    readUshort(jf->fin, &(ci->cfields));             // fields
    hsize -= 2;
    if (0 < ci->cfields) {
        ci->fields = talloc_array(ci, finfo, ci->cfields);
        for (i = 0; i < ci->cfields; i++) {
            ci->fields[i].ident = readIdent(jf);
            ci->fields[i].ftype = getc(jf->fin);
            ci->fields[i].resolved = 0;
            if (1 <= jf->fVersion)
                ci->fields[i].ftype = sigFromType(ci->fields[i].ftype);
            hsize -= 1 + jf->identsz;
        }
    }
    if (debug)
    printf("0x%x class %4d %4d %4d %s\n", ident, cpool, ci->cstats, ci->cfields, ci->name);
    // putchar('c');

    return hsize;
}

int
readArray(struct jdump *jf, unsigned hsize, int prim)
{
    long long ide, elemClassId;
    unsigned int stackId, isz;
    char primSig = 0x00;
    int elsz;
    hobject *ho;

    ide = readIdent(jf);
    readUint(jf->fin, &stackId);
    readUint(jf->fin, &isz);
    hsize -= 8 + jf->identsz;
    if (prim) {
        elemClassId = getc(jf->fin);
        hsize -= 1;
        if (debug)
        printf("0x%x primary array type %2d %s\n", ide, elemClassId, "");
    } else {
        elemClassId = readIdent(jf);
        hsize -= jf->identsz;
        if (debug) {
            cinfo *ci = (cinfo *) trbt_lookup32(jf->cTable, elemClassId);
            printf("0x%x object array type %x %s\n", ide, elemClassId, ci->name);
        }
    }
    if (prim /* || version < 1 */) {
        switch (elemClassId) {
        case  4 /* T_BOOLEAN */:  primSig = 'Z';  elsz = 1;  break;
        case  5 /* T_CHAR */:     primSig = 'C';  elsz = 2;  break;
        case  6 /* T_FLOAT */:    primSig = 'F';  elsz = 4;  break;
        case  7 /* T_DOUBLE */:   primSig = 'D';  elsz = 8;  break;
        case  8 /* T_BYTE */:     primSig = 'B';  elsz = 1;  break;
        case  9 /* T_SHORT */:    primSig = 'S';  elsz = 2;  break;
        case 10 /* T_INT */:      primSig = 'I';  elsz = 4;  break;
        case 11 /* T_LONG */:     primSig = 'J';  elsz = 8;  break;
        }
    }
    if (0x00 != primSig) {
        hsize -= elsz * isz;
        ho = makeObj(jf->hTable, H_VARRAY, ide, elemClassId);
        ho->fpos = ftello(jf->fin);
        ho->size = elsz;
        ho->count = isz;
        fseek(jf->fin, elsz * isz, SEEK_CUR);
    } else {
        hsize -= jf->identsz * isz;
        ho = makeObj(jf->hTable, H_OARRAY, ide, elemClassId);
        ho->fpos = ftello(jf->fin);
        ho->size = jf->identsz;
        ho->count = isz;
        fseek(jf->fin, jf->identsz * isz, SEEK_CUR);
    }
    trbt_insert32(jf->hTable, ide, ho);
    return hsize;
}

void
printClass_r(trbt_node_t *node)
{
    cinfo *ci;
    if (NULL == node)
        return;
    printClass_r(node->left);
    ci = (cinfo *) node->data;
    printf("0x%x instance 0x%x %d %s\n", ci->ident, ci->nident, ci->count, ci->name);
    printClass_r(node->right);
}

void
printClass(trbt_node_t *node)
{
    trbt_node_t **stack = (trbt_node_t **) malloc(sizeof(trbt_node_t *) * 20);
    int curr = 0, size = 20;

    while (node) {
        cinfo *ci = (cinfo *) node->data;
        printf("0x%x instance 0x%x %d %s\n", ci->ident, ci->nident, ci->count, ci->name);
        if (node->right) {
            if (curr == size) {
                stack = (trbt_node_t **) realloc(stack, sizeof(trbt_node_t *) * (size + 20));
                size += 20;
            }
            stack[curr++] = node->right;
        }
        if (node->left) {
            node = node->left;
            continue;
        }
        if (0 < curr)
            node = stack[--curr];
        else 
            node = NULL;
    }
}

unsigned long
hashKey(char *key)
{
    unsigned long ckey = crc32(0, Z_NULL, 0);
    return crc32(ckey, key, strlen(key));
}

cinfo *
findClass(struct jdump *jf, char *cname)
{
    return (cinfo *) trbt_lookup32(jf->rcTable, hashKey(cname));
}

int
resolveSClassSize(struct jdump *jf, cinfo *ci)
{
    cinfo *super;

    if (0 == ci->superId)
        return;

    super = (cinfo *) trbt_lookup32(jf->cTable, ci->superId);
    return ci->cfields + resolveSClassSize(jf, super);
}

void
resolveField(struct jdump *jf, hobject *fi)
{
    if (fi->resolved)
        return;
#ifdef notdef
    fi->name = (char *) trbt_lookup32(jf->sbTable, (long) fi->ident);
    switch (fi->ftype) {
    case 'L':
    case '[':
    }
#endif
    fi->resolved = 1;
}

cinfo *
getSuperClass(struct jdump *jf, cinfo *ci)
{
    return (cinfo *) trbt_lookup32(jf->cTable, ci->superId);
}

finfo *
copyField(cinfo *parent, finfo *src)
{
    finfo *dest = (finfo *) talloc(parent, finfo);
    memcpy(dest, src, sizeof(finfo));
    return dest;
}

void
resolveClassNode(struct jdump *jf, cinfo *ci)
{
    cinfo *super;
    int i;
    if (ci->resolved)
        return;
    ci->tfields = resolveSClassSize(jf, ci);

    // loader
    // signers
    // domain

    for (i = 0; i < ci->cstats; i++) 
        resolveField(jf, ci->statics + i);

    if (0 != (super = getSuperClass(jf, ci)))
        resolveClassNode(jf, super);

    if (0 < ci->tfields) {
        int target, index, fieldNo;
        cinfo *cc = ci;

        ci->values = talloc_array(ci, finfo *, ci->tfields);
        target = ci->tfields - cc->cfields;
        fieldNo = index = 0;
        for (i = 0; i < ci->tfields; i++, fieldNo++) {
            finfo *field, **value;
            while (fieldNo >= cc->cfields) {
                cc = getSuperClass(jf, cc);
                fieldNo = 0;
                target -= cc->cfields;
            }
            value = ci->values + target + fieldNo;
            if (cc == ci) {
                field = *value = cc->fields + fieldNo;
                field->name = (char *) trbt_lookup32(jf->sbTable, (long) field->ident);
            } else
                field = *value = copyField(ci, cc->fields + fieldNo);

            field->offset = index;
            switch (field->ftype) {
            case 'L': 
            case '[':  index += jf->identsz;  break;
            case 'Z': 
            case 'B':  index += 1; break;
            case 'S': 
            case 'C':  index += 2; break;
            case 'F':
            case 'I':  index += 4; break;
            case 'D':
            case 'J':  index += 8; break;
            }
        }
    }
    ci->resolved = 1;
}

void
resolveClass(struct jdump *jf, trbt_node_t *cnode)
{
    if (NULL == cnode)
        return;
    resolveClass(jf, cnode->left);
    resolveClassNode(jf, (cinfo *) cnode->data);
    resolveClass(jf, cnode->right);
}

void
resolveInstances(struct jdump *jf, trbt_node_t *cnode)
{
    if (NULL == cnode)
        return;
    resolveInstances(jf, cnode->left);
    resolveInstance(jf, (hobject *) cnode->data);
    resolveInstances(jf, cnode->right);
}

void
resolveClasses(struct jdump *jf)
{
    jf->javaLangClass = findClass(jf, "java/lang/Class");
    jf->javaLangClassLoader = findClass(jf, "java/lang/ClassLoader");

    resolveClass(jf, jf->cTable->root);
    resolveInstances(jf, jf->hTable->root);
}

void
resolveInstance(struct jdump *jf, hobject *ho)
{
    cinfo *ci;
    int i;

    if (ho->resolved)
        return;
    ho->resolved = 1;

    if (H_VARRAY == ho->htype) {
        ho->hvalues = talloc_array(ho, union hvalue, ho->count);
        if (0 == ho->count)
            return;
        fseeko(jf->fin, ho->fpos, SEEK_SET);
        switch (ho->classId) {
        case 4: case 8: { // BOOLEAN, BYTE
            char *b = (char *) ho->hvalues;
            fread(b, sizeof(char), ho->count, jf->fin);
            break;
        }
        case 5: case 9: {       // CHAR, SHORT
            unsigned short *c = (unsigned short *) ho->hvalues;
            fread(c, sizeof(unsigned short), ho->count, jf->fin);
            break;
        }
        case 6: case 7:         // FLOAT, DOUBLE
            break;
        case 10: {              // INT
            unsigned int *i = (unsigned int *) ho->hvalues;
            fread(i, sizeof(unsigned int), ho->count, jf->fin);
            break;
        }
        case 11: {              // LONG
            unsigned long long *j = (unsigned long long *) ho->hvalues;
            fread(j, sizeof(unsigned long long), ho->count, jf->fin);
            break;
        }
        }
        return;
    } else if (H_OARRAY == ho->htype) {
        if (0 == ho->count)
            return;
        fseeko(jf->fin, ho->fpos, SEEK_SET);
        ho->hvalues = talloc_array(ho, union hvalue, ho->count);
        for (i = 0; i < ho->count; i++) 
            (ho->hvalues + i)->ident = readIdent(jf);
        for (i = 0; i < ho->count; i++) {
            hobject *dref = (hobject *) trbt_lookup32(jf->hTable, (ho->hvalues + i)->ident);
            if (dref)
                resolveInstance(jf, dref);
        }
        return;
    }
    
    ci = (cinfo *) trbt_lookup32(jf->cTable, ho->classId);

    if (ci->tfields)
        ho->hvalues = talloc_array(ho, union hvalue, ci->tfields);
    for (i = 0; i < ci->tfields; i++) {
        finfo *info = *(ci->values + i);
        union hvalue *value = (ho->hvalues + i);
        fseeko(jf->fin, ho->fpos, SEEK_SET);
        fseek(jf->fin, info->offset, SEEK_CUR);
        switch (info->ftype) {
        case '[': 
        case 'L': {
            value->ident = readIdent(jf);
            hobject *dref = (hobject *) trbt_lookup32(jf->hTable, (long) value->ident);
            if (0 && !dref) 
                fprintf(stderr, "resolveInstance: cannot resolve Instance 0x%08x in 0x%08x of 0x%08x %s\n",
                    value->ident, ho->instId, ho->classId, ci->name);
            if (dref)
                resolveInstance(jf, dref);
            break;
        }
        case 'Z' : case 'B':
            value->b = getc(jf->fin); break;
        case 'S' :
        case 'C' : readUshort(jf->fin, &(value->c)); break;
        case 'I' : readUint(jf->fin, &(value->i)); break;
        case 'J' : readUlong(jf->fin, &(value->j)); break;
        }
    }
}

void
printInstance(struct jdump *jf, hobject *ho, int indent, int pshort)
{
    int i;
    cinfo *ci;

    if (ho->visit) {
        printf("[ recursive ] Instance 0x%08x of 0x%08x\n", ho->instId, ho->classId);
        return;
    }
    if (H_VARRAY == ho->htype) {
        printf("value array 0x%08x 0x%08x count %d\n", ho->instId, ho->classId, ho->count);
        if (ho->count) {
        if (indent) fputs("\t\t", stdout);
        for (i = 0; i < ho->count; i++) {
            switch (ho->classId) {
            case 4: case 8: {
                char *b = (char *) ho->hvalues;
                printf("%x ", *(b + i)); break; }
            case 5: {
                unsigned short *c = (unsigned short *) ho->hvalues;
                putchar(*(c + i) >> 8); break; }
            case 9: {
                unsigned short *c = (unsigned short *) ho->hvalues;
                printf("%x ", *(c + i)); break; }
            case 10: {
                unsigned int *pi = (unsigned int *) ho->hvalues;
                printf("%x ", *(pi + i)); break; }
            case 11: {
                unsigned long long *j = (unsigned long long *) ho->hvalues;
                printf("%lx ", *(j + i)); break; }
            }
        }
        puts("");
        }
        return;
    } else if (H_OARRAY == ho->htype) {
        printf("object array 0x%08x 0x%08x count %d\n", ho->instId, ho->classId, ho->count);
        if (ho->count) {
        for (i = 0; i < ho->count; i++) {
            if (indent) fputs("\t\t\t", stdout);
            if (5 < i) {
                puts("[....]");
                break;
            }
            unsigned long long *pid = (unsigned long long *) ho->hvalues;
            hobject *dref = (hobject *) trbt_lookup32(jf->hTable, (long) *(pid + i));
            if (dref)
                printInstance(jf, dref, 1 + indent, 1);
            else
                printf("Instance 0x%08x of 0x%08x %s\n", *(pid + i), 0, "unknown");
        }
        }
        return;
    }

    ci = (cinfo *) trbt_lookup32(jf->cTable, ho->classId);
    ho->visit = 1;
    // if (indent) fputs("\t\t", stdout);
    printf("Instance 0x%08x of 0x%08x %s\n", ho->instId, ho->classId, ci->name);
    if (pshort) {
        ho->visit = 0;
        return;
    }
    if (indent)
        printf("\t\t----> (%d)\n", indent);
    for (i = 0; i < ci->tfields; i++) {
        finfo *info = *(ci->values + i);
        union hvalue *value = ho->hvalues + i;
        
        if (indent) fputs("\t", stdout);
        printf("\t%3d (%3d): %c %-25s ", i, info->offset, info->ftype, info->name);
        switch (info->ftype) {
        case '[' :
        case 'L' : {
            hobject *dref = (hobject *) trbt_lookup32(jf->hTable, (long) value->ident);
            if (dref) {
                printInstance(jf, dref, 1 + indent, 1);
            } else
                printf("Instance 0x%08x of 0x%08x %s\n", value->ident, 0, "unknown");
            break;
        }
        case 'B' :
        case 'Z' :  printf(" %d  0x%x\n", value->b, value->b);  break;
        case 'C' :
        case 'S' :  printf(" %d  0x%x\n", value->c, value->c);  break;
        case 'I' :  printf(" %d  0x%x\n", value->i, value->i);  break;
        case 'J' :  printf(" %ld  0x%lx\n", value->j, value->j);  break;
        default: puts(" 0 0 ");
        }
    }
    if (indent)
        printf("\t\t<---- (%d)\n", indent);
    fflush(stdout);

    for (i = 0; i < ci->tfields; i++) {
        finfo *info = *(ci->values + i);
        union hvalue *value = ho->hvalues + i;
        switch (info->ftype) {
        case '[' :
        case 'L' : {
            hobject *dref = (hobject *) trbt_lookup32(jf->hTable, (long) value->ident);
            if (dref)
                printInstance(jf, dref, indent, 0);
            break;
            }
        default:  break;
        }
    }
        
    ho->visit = 0;
}

void
collectStats(struct jdump *jf, trbt_node_t *node, unsigned long ckey)
{
    hobject *ho;
    if (NULL == node)
        return;
    collectStats(jf, node->left, ckey);
    ho = (hobject *) node->data;
    if (ckey == ho->classId) {
        resolveInstance(jf, ho);
        printInstance(jf, ho, 0, 0);
    }
    collectStats(jf, node->right, ckey);
}

unsigned int 
countbytes(FILE *fin, unsigned int sz)
{
    unsigned int count = 0;
    int bb;

    while (sz > 0 && EOF != (bb = getc(fin))) {
        if (0 < bb) 
            count++;
        sz--;
    }
    return count;
}

void
readHeap(struct jdump *jf, unsigned int hsize)
{
    unsigned char rtype;
    long roott, rootg, rootl, frame, stack, sclass, tblock, monitor, cclass, inst, oarr, parr;


    roott = rootg = rootl = frame = stack = sclass = tblock = monitor = cclass = inst = oarr = parr = 0;

    jf->javaLangString = findClass(jf, "java/lang/String");

    while (0 < hsize && EOF != (rtype = getc(jf->fin))) {
        hsize--;
        switch (rtype) {
        case 0xff : {    // HPROF_GC_ROOT_UNKNOWN
            long long id = readIdent(jf);
            hsize -= jf->identsz;
            puts("\t heap root unknown");
            break;
        }
        case 0x08 : {   // HPROF_GC_ROOT_THREAD_OBJ
            int threadSeq, stackSeq;
            long long id = readIdent(jf);
            readUint(jf->fin, &threadSeq);
            readUint(jf->fin, &stackSeq);
            hsize -= jf->identsz + 8;
            if (debug)
            printf("0x%08lx root thread obj 0x08x 0x08x\n", id, threadSeq, stackSeq);
            // putchar('r');
            roott++;
            break;
        }
        case 0x01 : {   // HPROF_GC_ROOT_JNI_GLOBAL
            long long id, gid;
            id = readIdent(jf);
            gid = readIdent(jf);
            hsize -= jf->identsz + jf->identsz;
            if (debug)
            printf("0x%08lx root native static \n", id);
            // puts("\t heap root native global");
            // putchar('G');
            rootg++;
            break;
        }
        case 0x02 : {    // HPROF_GC_ROOT_JNI_LOCAL
            long long id;
            int threadSeq, depth;
            id = readIdent(jf);
            readUint(jf->fin, &threadSeq);
            readUint(jf->fin, &depth);
            hsize -= jf->identsz + 8;
            if (debug)
            printf("0x%08lx root native local \n", id);
            // puts("\t heap root native local");
            // putchar('L');
            rootl++;
            break;
        }
        case 0x03 : {    // HPROF_GC_ROOT_JAVA_FRAME
            long long id;
            int threadSeq, depth;
            id = readIdent(jf);
            readUint(jf->fin, &threadSeq);
            readUint(jf->fin, &depth);
            hsize -= jf->identsz + 8;
            if (debug)
            printf("0x%08lx root java local \n", id);
            // puts("\t heap root java frame");
            // putchar('F');
            frame++;
            break;
        }
        case 0x04 : {    // HPROF_GC_ROOT_NATIVE_STACK
            long long id;
            int threadSeq;
            id = readIdent(jf);
            readUint(jf->fin, &threadSeq);
            hsize -= jf->identsz + 4;
            if (debug)
            printf("0x%08lx root native stack \n", id);
            // puts("\t heap root native stack");
            // putchar('S');
            stack++;
            break;
        }
        case 0x05 : {    // HPROF_GC_ROOT_STICKY_CLASS
            long long id = readIdent(jf);
            hsize -= jf->identsz;
            if (debug)
            printf("0x%08lx root system class\n", id);
            // puts("\t heap root system class");
            // putchar('C');
            sclass++;
            break;
        }
        case 0x06 : {    // HPROF_GC_ROOT_THREAD_BLOCK
            long long id;
            int threadSeq;
            id = readIdent(jf);
            readUint(jf->fin, &threadSeq);
            hsize -= jf->identsz + 4;
            if (debug)
            printf("0x%08lx root thread block\n", id);
            // puts("\t heap root thread block");
            // putchar('T');
            tblock++;
            break;
        }
        case 0x07 : {    // HPROF_GC_ROOT_MONITOR_USED
            long long id = readIdent(jf);
            hsize -= jf->identsz;
            if (debug)
            printf("0x%08lx root busy monitor\n", id);
            // puts("\t heap root monitor ");
            // putchar('M');
            monitor++;
            break;
        }
        case 0x20 : {    // HPROF_GC_CLASS_DUMP
            hsize = readClass(jf, hsize);
            cclass++;
            break;
        }
        case 0x21 : {    // HPROF_GC_INSTANCE_DUMP
            long long ide, classId;
            unsigned int stackId, isz, cnt, fpos;
            hobject *ho;
            cinfo *ci;

            ide = readIdent(jf);
            readUint(jf->fin, &stackId);
            classId = readIdent(jf);
            readUint(jf->fin, &isz);
            hsize -= isz + 8 + jf->identsz + jf->identsz;
            ci = (cinfo *) trbt_lookup32(jf->cTable, classId);
            ci->count++;
            if (debug)
                printf("0x%x instance 0x%x %s\n", ide, classId, ci->name);
            // fpos = ftell(jf->fin);
            // cnt = countbytes(jf->fin, isz);
            if (50000 > ci->count || classId == jf->javaLangString->ident) {
                ho = makeObj(jf->hTable, H_INSTANCE, ide, classId);
                ho->fpos = ftello(jf->fin);
                trbt_insert32(jf->hTable, ide, ho);
            }
            // putchar('i');
            fseek(jf->fin, isz, SEEK_CUR);
            inst++;
            break;
        }
        case 0x22 : {    // HPROF_GC_OBJ_ARRAY_DUMP
            // puts("\t heap object array");
            // putchar('o');
            hsize = readArray(jf, hsize, 0);
            oarr++;
            break;
        }
        case 0x23 : {    // HPROF_GC_PRIM_ARRAY_DUMP
            // puts("\t heap prim array");
            // putchar('p');
            hsize = readArray(jf, hsize, 1);
            parr++;
            break;
        }
        default:
            fprintf(stderr, "readHeap: unknown heap type 0x%x\n", rtype);
            fseek(jf->fin, hsize, SEEK_CUR);
            hsize = 0;
            break;
    }
    }
    puts("\nHeap Summary");
    printf("\t%15s : %8d \n", "root thread", roott);
    printf("\t%15s : %8d \n", "root global", rootg);
    printf("\t%15s : %8d \n", "root local", rootl);
    printf("\t%15s : %8d \n", "root frame", frame);
    printf("\t%15s : %8d \n", "stack", stack);
    printf("\t%15s : %8d \n", "system class", sclass);
    printf("\t%15s : %8d \n", "thread block", tblock);
    printf("\t%15s : %8d \n", "monitor", monitor);
    printf("\t%15s : %8d \n", "class", cclass);
    printf("\t%15s : %8d \n", "instance", inst);
    printf("\t%15s : %8d \n", "object array", oarr);
    printf("\t%15s : %8d \n", "primary array", parr);

    resolveClasses(jf);

    puts("Class Summary");
    printClass_r(jf->cTable->root);

    if (jf->fclass) {
        // cinfo *cdata = findClass(jf, "com/teramedica/web/actions/notification/TMNotificationListAction");
        // cinfo *cdata = findClass(jf, "java/util/concurrent/ConcurrentHashMap$Segment");
        cinfo *cdata = findClass(jf, jf->fclass);
        if (cdata)
            collectStats(jf, jf->hTable->root, (long) cdata->ident);
        else 
            printf("findclass: \'%s\' not found\n");
    }
}
