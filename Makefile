CC=gcc
GTK_CFLAGS := $(shell pkg-config --cflags gtk4)
GTK_LIBS := $(shell pkg-config --libs gtk4)
CFLAGS=-O2 -Wall -Wextra -pedantic -std=c11 $(GTK_CFLAGS)
LDFLAGS=
LDLIBS=-lcurl -lpthread -lncurses $(GTK_LIBS)

OBJS=main.o downloader.o job.o job_manager.o scheduler.o progress.o gui.o util.o

all: weighted_downloader

weighted_downloader: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) weighted_downloader

.PHONY: all clean
