CFLAGS= -std=c89 -pedantic -Wall -Wextra

SHELL := /bin/bash

main: rule non-clean clean run

rule:
	source .bash_profile

non-clean: master.o player.o pawn.o
	gcc master.o -o master
	gcc player.o -o player
	gcc pawn.o -o pawn

master.o:
	gcc $(CFLAGS) -c master.c

player.o:
	gcc $(CFLAGS) -c player.c

pawn.o:
	gcc $(CFLAGS) -c pawn.c

clean:
	rm *.o

run:
	./master
