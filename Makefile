
PULSE_CFLAGS = $(shell pkg-config --cflags libpulse)
PULSE_LDFLAGS = $(shell pkg-config --libs libpulse)

CFLAGS += -Wall -g -I include/ $(PULSE_CFLAGS)
LDFLAGS += $(PULSE_LDFLAGS)

exhaust-open-streams: exhaust_open_streams.o
	$(CC) $^ -o $@ $(LDFLAGS)

all: exhaust-open-streams


.PHONY: clean
clean:
	rm -vf *~
	rm -vf exhaust-open-streams exhaust_open_streams.o
