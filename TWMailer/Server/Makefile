CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -O2

.PHONY: all clean
all: twmailer-server

twmailer-server: twmailer-server.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f twmailer-server
