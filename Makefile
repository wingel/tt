CC=gcc
CFLAGS=-Wall -O2

TARGETS=tt

all: $(TARGETS)

install:
	cp -f $(TARGETS) /usr/local/bin

clean:
	rm -f *.o *~ core
	rm -f $(TARGETS)

