CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -g -std=gnu11
LDFLAGS = -lrt

TARGET  = demo
SRCS    = kthread.c demo.c
OBJS    = $(SRCS:.c=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c kthread.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)
