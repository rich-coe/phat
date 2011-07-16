/* parse java hprof results file */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <endian.h>
#include <time.h>
#include <unistd.h>

#include "hash.h"

#define MAGIC_HEADER 0x4a415641   // JAVA

struct jdump {          // java dump
    FILE *fin;
    int fVersion;
    unsigned int identsz;
    pHashTable *sbTable, *cTable, *hTable;
};

struct hobj {           // heap object
#define H_INSTANCE 0x01
#define H_VARRAY   0x02
#define H_OARRAY   0x03
    int htype;
    long long heapId;
};

int readVersion(FILE *fin);
int readUint(FILE *fin, unsigned int *input);
int readUlong(FILE *fin, unsigned long long *input);
long long readIdent(struct jdump *);
struct jdump *readDump(char *dumpfile);
void readHeap(struct jdump *, unsigned int hsize);
char *hideSpecials(char *);

int debug = 0;

int
iKeyEqual(void *a, void *b)
{
    return ((long) a) == ((long ) b);
}

int
main(int argc, char **argv)
{
    int opt;
    char *baseline = NULL;
    struct jdump *df, *bf;

    while (-1 != (opt = getopt(argc, argv, "b:d"))) {
    switch (opt) {
    case 'b': baseline = strdup(optarg); break;
    case 'd': debug++; break;
    }
    }

    df = readDump(argv[optind]);
    if (baseline) {
        bf = readDump(baseline);
    }

    exit(0);
}

struct jdump *
readDump(char *dumpfile) 
{
    struct jdump *df;
    int rc;
    unsigned int magic;
    unsigned long long date;
    time_t tdate;
    struct tm *ltime;
    unsigned char rtype;

    df = (struct jdump *) malloc(sizeof(struct jdump));

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

    df->sbTable = pHashNew(NULL, iKeyEqual);    // table of strings
    df->cTable = pHashNew(NULL, iKeyEqual);     // table of classes
    df->hTable = pHashNew(NULL, iKeyEqual);     // table of heap objs

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

        switch (rtype) {
        case 0x01 : { // HPROF_UTF8
            long key;
            long long ident = readIdent(df);
            int  slen = rlen - df->identsz;
            char *usb = calloc(1 + rlen - df->identsz, 1);
            rc = fread(usb, 1, slen, df->fin);
            usb = hideSpecials(usb);
            if (debug)
            printf("0x%8x %s\n", ident, usb);
            key = (long) ident;
            pHashInsert(df->sbTable, (hptr) key, usb);
            break;
        }
        case 0x02 : { // HPROF_LOAD_CLASS
            long key, nkey;
            char *value;
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
            value = (char *) pHashLookup(df->sbTable, (hptr) nkey);
            pHashInsert(df->cTable, (hptr) key, value);
            if (debug) {
            if (4 < df->identsz)
                printf("0x%016lx classId 0x%08x 0x%016lx %s\n", classId, serial, classNameId, value);
            else
                printf("0x%08lx classId 0x%08x 0x%08lx %s\n", classId, serial, classNameId, value);
            }
            break;
        }
        case 0x05 : { // HPROF_TRACE
            int serial, threadSeq, frames;
            readUint(df->fin, &serial);             // serialNumber
            readUint(df->fin, &threadSeq);
            readUint(df->fin, &frames);
            if (debug)
            printf("trace serial %d %d %d\n", serial, threadSeq, frames);
            break;
        }
        case 0x0c : { // HPROF_HEAP_DUMP
            // printf("heap dump\n");
            readHeap(df, rlen);
            break;
        }
        default:
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

struct hobj *
makeObj(int ht, long long cid)
{
    struct hobj *ho = (struct hobj *) malloc(sizeof(struct hobj));

    ho->htype = ht;
    ho->heapId = cid;
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
readValue(struct jdump *jf)
{
    unsigned char rtype = getc(jf->fin);

    if (1 <= jf->fVersion)
        rtype = sigFromType(rtype);

    switch (rtype) {
    case '[' :
    case 'L' : {
        long long ident = readIdent(jf);
        return jf->identsz;
    }
    case 'Z' : {
        unsigned char b = getc(jf->fin);
        if (0 != b && 1 != b) {
            fprintf(stderr, "readValue: illegal bool read 0x%x\n", b);
            return -1;
        }
        return 1;
    }
    case 'B' : {
        unsigned char b = getc(jf->fin);
        return 1;
    }
    case 'S' : {
        unsigned short s;
        readUshort(jf->fin, &s);
        return 2;
    }
    case 'C' : {
        unsigned short c;
        readUshort(jf->fin, &c);
        return 2;
    }
    case 'I' : {
        unsigned int i;
        readUint(jf->fin, &i);
        return 4;
    }
    case 'J' : {
        unsigned long long j;
        readUlong(jf->fin, &j);
        return 8;
    }
    case 'F' : {                // float
        fseek(jf->fin, 4, SEEK_CUR);
        return 4;
    }
    case 'D' : {                // double
        fseek(jf->fin, 8, SEEK_CUR);
        return 8;
    }
    default:
    fprintf(stderr, "readValue: bad type sig 0x%x\n", rtype);
    break;
    }
}

int
readClass(struct jdump *jf, unsigned int hsize)
{
    long long ident, superId, classId, signerId, domainId, res1, res2;
    unsigned int stackId, instsz;
    unsigned short cpool, cstats, cfields;
    char *name;
    long key;
    int i;

    ident = readIdent(jf);
    readUint(jf->fin, &stackId);
    superId = readIdent(jf);
    classId = readIdent(jf);
    signerId = readIdent(jf);
    domainId = readIdent(jf);
    res1 = readIdent(jf);
    res2 = readIdent(jf);
    readUint(jf->fin, &instsz);
    hsize -= (7 * jf->identsz) + 8;

    readUshort(jf->fin, &cpool);             // const pool entries
    hsize -= 2;
    for (i = 0; i < cpool; i++) {
        unsigned short entry;
        readUshort(jf->fin, &entry);
        hsize -= 2;
        hsize -= readValue(jf);
    }

    readUshort(jf->fin, &cstats);             // statics
    hsize -= 2;
    for (i = 0; i < cstats; i++) {
        long long nameId = readIdent(jf);
        hsize -= jf->identsz;
        hsize -= 1 + readValue(jf);
    }

    readUshort(jf->fin, &cfields);             // fields
    hsize -= 2;
    for (i = 0; i < cfields; i++) {
        unsigned char rtype;
        long long nameId = readIdent(jf);
        rtype = getc(jf->fin);
        hsize -= 1 + jf->identsz;
    }
    key = (long) ident;
    name = (char *) pHashLookup(jf->cTable, (hptr) key);
    if (debug)
    printf("0x%x class %4d %4d %4d %s\n", ident, cpool, cstats, cfields, name);
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
    struct hobj *ho;

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
        if (debug)
        printf("0x%x object array type %x %s\n", ide, elemClassId, (char *) pHashLookup(jf->cTable, (hptr) elemClassId));
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
        fseek(jf->fin, elsz * isz, SEEK_CUR);
        hsize -= elsz * isz;
        ho = makeObj(H_VARRAY, elemClassId);
    } else {
        fseek(jf->fin, jf->identsz * isz, SEEK_CUR);
        hsize -= jf->identsz * isz;
        ho = makeObj(H_OARRAY, elemClassId);
    }
    // pHashInsert(jf->hTable, (hptr) ide, ho);
    return hsize;
}

void
readHeap(struct jdump *jf, unsigned int hsize)
{
    unsigned char rtype;
    long roott, rootg, rootl, frame, stack, sclass, tblock, monitor, cclass, inst, oarr, parr;


    roott = rootg = rootl = frame = stack = sclass = tblock = monitor = cclass = inst = oarr = parr = 0;

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
            unsigned int stackId, isz;
            struct hobj *ho;

            ide = readIdent(jf);
            readUint(jf->fin, &stackId);
            classId = readIdent(jf);
            readUint(jf->fin, &isz);
            hsize -= isz + 8 + jf->identsz + jf->identsz;
            if (debug)
            printf("0x%x instance 0x%x %s\n", ide, classId, (char *) pHashLookup(jf->cTable, (hptr) classId));
            ho = makeObj(H_INSTANCE, classId);
            // pHashInsert(jf->hTable, (hptr) ide, ho);
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
}
