CC=gcc
CFLAGS=-Wall -Os -g

home-container: home-container.nosuid
	# We need to make the binary suid-root to set up the sandbox.
	sudo cp home-container.nosuid home-container
	sudo chmod u+s home-container

home-container.nosuid: home-container.c
	$(CC) -std=c99 $(CFLAGS) home-container.c -o home-container.nosuid

clean:
	rm -f home-container home-container.nosuid
