PKGS := wayland-client
CFLAGS += -std=c11 -Wall -Wextra -O2 $(shell pkg-config --cflags $(PKGS))
LDLIBS += $(shell pkg-config --libs $(PKGS))

WAYLAND_SCANNER := wayland-scanner
PROTO_DIR := protocols
GEN_DIR := generated

PROTOCOLS := wlr-layer-shell-unstable-v1 single-pixel-buffer-v1 viewporter xdg-shell

GEN_HEADERS := $(addprefix $(GEN_DIR)/,$(addsuffix -client-protocol.h,$(PROTOCOLS)))
GEN_SOURCES := $(addprefix $(GEN_DIR)/,$(addsuffix -protocol.c,$(PROTOCOLS)))
GEN_OBJECTS := $(GEN_SOURCES:.c=.o)

BIN := omarchy-soft-brightnessd

.PHONY: all clean install
.SECONDARY: $(GEN_HEADERS) $(GEN_SOURCES)

all: $(BIN)

$(GEN_DIR):
	mkdir -p $(GEN_DIR)

$(GEN_DIR)/%-client-protocol.h: $(PROTO_DIR)/%.xml | $(GEN_DIR)
	$(WAYLAND_SCANNER) client-header $< $@

$(GEN_DIR)/%-protocol.c: $(PROTO_DIR)/%.xml | $(GEN_DIR)
	$(WAYLAND_SCANNER) private-code $< $@

$(GEN_DIR)/%.o: $(GEN_DIR)/%.c $(GEN_HEADERS)
	$(CC) $(CFLAGS) -I$(GEN_DIR) -c $< -o $@

src/main.o: src/main.c $(GEN_HEADERS)
	$(CC) $(CFLAGS) -I$(GEN_DIR) -c $< -o $@

$(BIN): src/main.o $(GEN_OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

install: $(BIN)
	install -Dm755 $(BIN) /usr/local/bin/$(BIN)
	install -Dm755 omarchy-soft-brightness /usr/local/bin/omarchy-soft-brightness

clean:
	rm -rf $(GEN_DIR) src/main.o $(BIN)
