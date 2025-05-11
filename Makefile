CC      := cc
CFLAGS  := -std=c17 -Wall -Wextra -O2
LDFLAGS :=
TARGET  := vfs
SRC     := virtual_fs.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^


clean:
	rm -f $(TARGET) *.o

.PHONY: all debug clean
