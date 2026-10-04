CC = cc
CFLAGS = -Wall -Wextra -Wpedantic -std=c17

TARGET = myFind
SOURCE = main.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) $(SOURCE) -o $(TARGET)

clean:
	rm -f $(TARGET)