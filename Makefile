CC=gcc
CFLAGS=-Wall -Os -g

home-container: home-container.c
	$(CC) -std=c99 $(CFLAGS) home-container.c -o home-container

clean:
	rm -f home-container
