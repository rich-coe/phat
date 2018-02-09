
SRC= phat.c rbtree.c talloc.c
OBJ= phat.o rbtree.o talloc.o

CFLAGS= -g 
# LIBF= -Wl,-rpath,${TOP}/lib -L${TOP}/lib -lheader

# .c.o:
#	c++ -c ${CFLAGS} $*.c

phat: ${OBJ}
	${CC} ${CFLAGS} -o phat ${OBJ} -lz

clean:
	rm -f ${OBJ} phat
