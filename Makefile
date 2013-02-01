CC = gcc
CFLAGS = -Wall -ggdb $(shell pkg-config gtk+-2.0 --cflags)
LIBS = $(shell pkg-config gtk+-2.0 --libs)

all:	fm1216me

%.o:	%.c
	$(CC) $(CFLAGS) -c $^ -o $@

fm1216me:	fm1216me.o
	$(CC) $^ $(LIBS) -o $@

clean:
	rm -f fm1216me *.o *~ core

.PHONY:	all clean
