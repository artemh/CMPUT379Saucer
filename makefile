all: saucer

saucer: saucer.c saucer.h
	cc -o saucer saucer.c -lpthread -lcurses -lbsd

clean:
	rm -rf saucer