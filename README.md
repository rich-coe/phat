# PHAT  parse java's hprof (dump) files, like jhat

## Motivation
I wrote this to successfully parse a java heap dumps that jhat was incapable of.
The problem I kept hitting was that I couldn't make the heap large enough for jhat
to successfully parse the dump files that were being generated.  The root cause for jhat
was that it used java.util.HashMap to store classes and instances, and it exhausted all of memory
parsing the file, even with a 64-bit JVM.

Unlike jhat, phat parses the dump file, storing classes and instances in a red/black tree,
similar to java.util.TreeMap.  Since it's written in C, you don't have to guess the heap
allocation. 

## Output

- Dump file created [... date ...]
- trace back frames 
- Heap Summary 
- Class Summary
- Instance Summary

### Dump file date  

The heap dump creation date.

### trace back frames

tracebacks for each thread

### Heap Summary

            root thread :       79 
            root global :       35 
             root local :       33 
             root frame :      712 
                  stack :        0 
           system class :     3335 
           thread block :        0 
                monitor :        8 
                  class :    11829 
               instance :  5767989 
           object array :   795709 
          primary array :   979154 


### Class Summary

    - Class Identifier
    - Name Identifier 
    - count of instances
    - symbol class name

        0x2678c7a8 class 0x530fc950 3 java/util/concurrent/atomic/AtomicLongFieldUpdater$CASUpdater
        0x2678c808 class 0x54196eb0 0 java/util/concurrent/ExecutionException
        0x2678c928 class 0x5634e890 0 java/util/UUID$Holder
        0x2678ca48 class 0x541c2860 0 java/util/JumboEnumSet
        0x2678d8e8 class 0x53129728 2 java/util/Collections$SynchronizedSortedMap
        0x2678df48 class 0x54d8b8e8 3 java/util/TaskQueue
        0x2678dfa8 class 0x54d86258 0 java/util/TooManyListenersException
        0x26993e50 class 0x55583a20 3 java/util/Timer


### Instance Summary

    Instance 0x28405f00 of 0x26993e50 java/util/Timer self 12 self+children -1293452014923129306
          0 (  0): L threadReaper              Instance 0x28614b10 of 0x295e0660 java/util/Timer$1 self 4 self+children 4
                ----> (1)
                  0 (  0): L this$0                    [ recursive ] Instance 0x28405f00 of 0x26993e50 self 12 self+children -729915866
                <---- (1)
          1 (  4): L thread                    Instance 0x267a7300 of 0x295e06b8 java/util/TimerThread self 112 self+children -1293452014923129842
                ----> (1)
                  0 (  5): I threadLocalRandomSecondarySeed  0  0x0
                  1 (  9): I threadLocalRandomProbe     0  0x0
                  2 ( 13): J threadLocalRandomSeed      0  0x0
                  3 ( 21): L uncaughtExceptionHandler  [null]
                  4 ( 25): L blockerLock                          5 ( 29): L blocker                   [null]
                  6 ( 33): L parkBlocker               [null]
                  7 ( 37): I threadStatus               401  0x191
                  8 ( 41): J tid                        50  0x32
                  9 ( 49): J nativeParkEventPointer     0  0x0
                 10 ( 57): J stackSize                  0  0x0
                 11 ( 65): L inheritableThreadLocals             12 ( 69): L threadLocals              [null]
                 13 ( 73): L inheritedAccessControlContext               14 ( 77): L contextClassLoader                  15 ( 81): L group                               16 ( 85): L target                    [null]
                 17 ( 89): Z stillborn                  0  0x0
                 18 ( 90): Z daemon                     1  0x1
                 19 ( 91): Z single_step                0  0x0
                 20 ( 92): J eetop                      1424803840  0x54ecc800
                 21 (100): L threadQ                   [null]
                 22 (104): I priority                   5  0x5
                 23 (108): L name                                24 (  0): L queue                               25 (  4): Z newTasksMayBeScheduled     1  0x1
                <---- (1)
          2 (  8): L queue                     Instance 0x29151e00 of 0x2678df48 java/util/TaskQueue self 8 self+children 520
                ----> (1)
                  0 (  0): I size                       0  0x0
                  1 (  4): L queue                              <---- (1)


## Usage 

    - Generate a class instance list with counts

    phat  heapdump.heap  > heapdump.out
    egrep ' class ' heapdump.out | sort -n -k 4 > heapdump.out.s


    - Dump details on a specific class

    phat  -C java/util/TaskQueue  heapdump.heap  > heapinfo


## Options

- '-C' dump details on specific class
- '-d' print diagnostic debugging for development
- '-l' limit class dump depth

## Limitations

- recursive dump of classes needs some pretty-printing work
- consider sorting of classes within the app
- limit of the nubmer elements in array object to print should be configurable (100)
