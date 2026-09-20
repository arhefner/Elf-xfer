PROJECT = mem-xfr
CC=gcc -g3
CFLAGS=-I. -Wall
DEPS = intl.h ihex.h
OBJ = mem-xfr.o ihex.o

%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

$(PROJECT): $(OBJ)
	$(CC) -o $@ $^ $(CFLAGS)

# Protocol tests against Python mocks of loadbin/savebin. No 1802 and no
# serial hardware needed -- they run over a pty.
test: $(PROJECT)
	python3 test-mem-xfr.py

clean:
	-rm *.o
	-rm $(PROJECT)

.PHONY: test clean
