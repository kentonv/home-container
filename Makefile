CC=gcc
CFLAGS=-Wall -O2

chrome-container: chrome-container.nosuid
	@echo "*** We need to make the binary suid-root to set up the sandbox."
	sudo cp chrome-container.nosuid chrome-container
	sudo chmod u+s chrome-container

chrome-container.nosuid: chrome-container.c
	$(CC) -std=c99 $(CFLAGS) chrome-container.c -o chrome-container.nosuid

clean:
	rm -f chrome-container chrome-container.nosuid