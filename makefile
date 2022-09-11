bin:=$(notdir $(shell pwd))
src:=$(shell ls *.c)
obj:=$(src:.c=.o)

CFLAGS+=-D_GNU_SOURCE

all: $(bin)

$(bin): $(obj)
	$(CC) $^ $(LDFLAGS) -o $@

%.o: %.c *.h
	$(CC) -c $< $(CFLAGS) -o $@

clean:
	-rm $(bin) $(obj)

.PHONY: all clean
