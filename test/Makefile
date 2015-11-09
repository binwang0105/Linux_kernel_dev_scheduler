CC := arm-none-linux-gnueabi-gcc
LD := arm-none-linux-gnueabi-gcc
CFLAGS := -g -Wall -Werror -static
LDFLAGS := -static

OBJECTS := test.o

all: test

: $(OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJECTS)

%.o: %.c
	$(CC) $(CFLAGS) -c $^

clean:
	rm -f test test.o
.PHONY: clean
