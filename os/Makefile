CC = gcc -m32
OS=linux
KHOME = /cs/faculty/rich/cs170
CFLAGS = -m32 -g -I$(KHOME)/include -I.
#LIB = $(KHOME)/lib/libsim.a $(KHOME)/lib/libkt.a $(KHOME)/lib/libfdr.a
LIB = $(KHOME)/lib/libsim.a $(KHOME)/lib/libkt.a $(KHOME)/lib/libfdr.a $(KHOME)/lib/libkos.a
MAIN = $(KHOME)/lib/main_lab2.o $(KHOME)/lib/rb.o

EXECUTABLES = kos

all: $(EXECUTABLES)

INCL = scheduler.h console_buf.h syscall.h memory.h kos.h
CFILES = scheduler.c console_buf.c syscall.c memory.c kos.c exception.c

USER    = exception.o kos.o scheduler.o console_buf.o syscall.o memory.o

kos: $(USER) $(MAIN) $(LIB) $(INCL)
	$(CC) $(CFLAGS) -o kos $(MAIN) $(USER) $(LIB) 

.c.o: $(INCL) $(CFILES)
	$(CC) -c $(CFLAGS) $*.c

clean:
	/bin/rm -f *.o core $(EXECUTABLES)



