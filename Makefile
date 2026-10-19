CC     = gcc
CFLAGS = -Wall -Wextra -std=gnu11

all: kernelsim intercontroller aplicacao

kernelsim: kernelsim.c comum.h
	$(CC) $(CFLAGS) -o kernelsim kernelsim.c

intercontroller: intercontroller.c comum.h
	$(CC) $(CFLAGS) -o intercontroller intercontroller.c

aplicacao: aplicacao.c comum.h
	$(CC) $(CFLAGS) -o aplicacao aplicacao.c

run: all
	./kernelsim 4

clean:
	rm -f kernelsim intercontroller aplicacao
