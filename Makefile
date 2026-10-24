CC ?= cc
CFLAGS ?= -Os -std=c11 -Wall -Wextra -Werror
SOURCES = src/main.c src/sms.c
all: build/ec20-events
build/ec20-events: $(SOURCES) src/sms.h
	mkdir -p build
	$(CC) $(CFLAGS) $(SOURCES) $(LDFLAGS) -o $@
arm:
	mkdir -p build
	$(ZIG) cc -target arm-linux-musleabi -mcpu=cortex_a7 -Os -std=c11 -Wall -Wextra -Werror -static -s $(SOURCES) -o build/ec20-events-arm
clean:
	rm -f build/ec20-events build/ec20-events-arm
.PHONY: all arm clean
