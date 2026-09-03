PROJECT = max-xfr
CC=gcc -g3
CFLAGS=-I. -Wall
DEPS = intl.h
OBJ = max-xfr.o

%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

$(PROJECT): $(OBJ)
	$(CC) -o $@ $^ $(CFLAGS)

clean:
	-rm *.o
	-rm $(PROJECT)