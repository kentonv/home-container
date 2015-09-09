CC=gcc
CFLAGS=-Wall -O2

browser-cordon: browser-cordon.nosuid
	# We need to make the binary suid-root to set up the sandbox.
	sudo cp browser-cordon.nosuid browser-cordon
	sudo chmod u+s browser-cordon

browser-cordon.nosuid: browser-cordon.c
	$(CC) -std=c99 $(CFLAGS) browser-cordon.c -o browser-cordon.nosuid

clean:
	rm -f browser-cordon browser-cordon.nosuid
