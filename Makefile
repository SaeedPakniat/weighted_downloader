CC=gcc
CFLAGS=-O2 -Wall -Wextra -pedantic -std=c11
LDFLAGS=
LDLIBS=-lcurl -lpthread -lncurses

OBJS=main.o downloader.o scheduler.o progress.o util.o

all: weighted_downloader

weighted_downloader: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) weighted_downloader

.PHONY: all clean
