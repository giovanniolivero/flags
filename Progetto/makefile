CFLAGS= -std=c89 -pedantic 

main: non-clean clean run

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
