PROJECT = max-xfr
CC=gcc -g3
CFLAGS=-I.
DEPS = ihex.h intl.h
OBJ = max-xfr.o ihex.o 

%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

$(PROJECT): $(OBJ)
	$(CC) -o $@ $^ $(CFLAGS)

clean:
	-rm *.o
	-rm $(PROJECT)