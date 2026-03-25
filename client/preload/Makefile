CC = gcc
CFLAGS = -Wall -Wextra -Wno-nonnull-compare -O2 -fPIC -D_GNU_SOURCE
LDFLAGS = -shared -ldl -lpthread

LIBCFS_H = ../libsdk/libcfs.h
TARGET = libcfs_preload.so
SRC = cfs_preload.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC) $(LIBCFS_H)
	$(CC) $(CFLAGS) -I../libsdk -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)
