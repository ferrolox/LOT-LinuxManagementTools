CC = gcc
CFLAGS = -Wall -Wextra -Wpedantic -std=c11 -O2

TARGET = TWMailerClient
SOURCE = main.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) $(SOURCE) -o $(TARGET)

clean:
	rm -f $(TARGET)