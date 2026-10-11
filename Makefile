PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
CC = /opt/ps5-payload-sdk/bin/prospero-clang
CFLAGS   = -O2 -Wall -Wextra -fno-stack-protector -fPIC
LDFLAGS  = -Wl,--gc-sections

TARGET   = bd-mute.elf
SRC      = main.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
