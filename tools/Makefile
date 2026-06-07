# Host build of the chip-independent codec self-test. No Pico SDK required.
CC ?= cc
CFLAGS ?= -Wall -Wextra -O2 -I../core

st: frame_selftest.c ../core/bus_crc8.c ../core/bus_frame.c
	$(CC) $(CFLAGS) $^ -o $@

.PHONY: test clean
test: st
	./st

clean:
	rm -f st
