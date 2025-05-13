
CC = gcc
CFLAGS = -Wall -Wextra
TARGET = vfs
SRC = virtual_fs.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET)
