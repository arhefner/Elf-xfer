PROJECT = mem-xfr
CC=gcc -g3
CFLAGS=-I. -Wall
DEPS = intl.h ihex.h
OBJ = mem-xfr.o ihex.o

%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

$(PROJECT): $(OBJ)
	$(CC) -o $@ $^ $(CFLAGS)

clean:
	-rm *.o
	-rm $(PROJECT)
