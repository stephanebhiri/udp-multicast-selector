CC = gcc
CFLAGS = -O2 -Wall -Wextra
TARGET = udp-selector
SRC = udp-selector.c

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/
