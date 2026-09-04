PROJECT = sertest
CC=gcc -g3
CFLAGS=-Wall

$(PROJECT): sertest.c
	$(CC) -o $@ $< $(CFLAGS)

clean:
	-rm -f $(PROJECT) $(PROJECT).exe
