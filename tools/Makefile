# Host builds of the chip-independent self-tests. No Pico SDK required.
CC ?= cc
CFLAGS ?= -Wall -Wextra -O2 -I../core
HLFLAGS ?= -Wall -Wextra -O2 -I../core -I../port/rp2040/vendor/libcomm
CBORFLAGS ?= -Wall -Wextra -O2 -I../node

st: frame_selftest.c ../core/bus_crc8.c ../core/bus_frame.c
	$(CC) $(CFLAGS) $^ -o $@

# Northbound dongle-protocol self-test (host_link + vendored libcomm codec).
hl: host_link_selftest.c ../core/host_link.c ../port/rp2040/vendor/libcomm/frame.c
	$(CC) $(HLFLAGS) $^ -o $@

# Boot-config CBOR decoder self-test (header-only cbor_min.h -> list it as a
# prerequisite so editing the header retriggers the build).
cbor: cbor_selftest.c ../node/cbor_min.h
	$(CC) $(CBORFLAGS) cbor_selftest.c -o $@

.PHONY: test hltest cbortest clean
test: st cbor
	./st
	./cbor

hltest: hl
	./hl

cbortest: cbor
	./cbor

clean:
	rm -f st hl cbor
