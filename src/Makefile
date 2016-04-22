all: pty.so
.PHONY: all

pty.o: pty.c
	gcc -Wall -O2 -fpic -I/usr/local/include/lua -c pty.c

pty.so: pty.o
	gcc -O -fpic -shared -o pty.so pty.o

clean:
	rm -f *.o *.so
