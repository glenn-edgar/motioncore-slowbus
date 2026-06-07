# Host builds of the chip-independent self-tests. No Pico SDK required.
CC ?= cc
CFLAGS ?= -Wall -Wextra -O2 -I../core
HLFLAGS ?= -Wall -Wextra -O2 -I../core -I../port/rp2040/vendor/libcomm

st: frame_selftest.c ../core/bus_crc8.c ../core/bus_frame.c
	$(CC) $(CFLAGS) $^ -o $@

# Northbound dongle-protocol self-test (host_link + vendored libcomm codec).
hl: host_link_selftest.c ../core/host_link.c ../port/rp2040/vendor/libcomm/frame.c
	$(CC) $(HLFLAGS) $^ -o $@

.PHONY: test hltest clean
test: st
	./st

hltest: hl
	./hl

clean:
	rm -f st hl
