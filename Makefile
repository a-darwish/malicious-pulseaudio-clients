
PULSE_CFLAGS = $(shell pkg-config --cflags libpulse)
PULSE_LDFLAGS = $(shell pkg-config --libs libpulse)

CFLAGS += -Wall -g -I include/ $(PULSE_CFLAGS)
LDFLAGS += $(PULSE_LDFLAGS)

all: exhaust-open-streams kill-server-quickly-open-write-streams

#
# TODO: Re-factor makefile
#

exhaust-open-streams: exhaust_open_streams.o
	$(CC) $^ -o $@ $(LDFLAGS)

kill-server-quickly-open-write-streams: kill_server_quickly_open_write_streams.o audio_file.o
	$(CC) $^ -o $@ $(LDFLAGS)

.PHONY: clean
clean:
	rm -vf *~
	rm -vf exhaust-open-streams exhaust_open_streams.o
	rm -vf kill-server-quickly-open-write-streams kill_server_quickly_open_write_streams.o audio_file.o
