# ESP32 firmware shortcuts. Run from esp32/ or as `make -C esp32 TARGET`.
# Uses ESP-IDF from IDF_PATH (default ~/esp/esp-idf-v5.5.3) and the board on
# PORT (default /dev/ttyACM0). A regular flash keeps NVS: pairing, identity,
# settings and call-button numbers survive. Never use erase-flash routinely.

SHELL := /bin/bash
IDF_PATH ?= $(HOME)/esp/esp-idf-v5.5.3
PORT ?= /dev/ttyACM0
IDF := source "$(IDF_PATH)/export.sh" >/dev/null && idf.py
# Inside the Flatpak VS Code sandbox, host processes are only visible via
# flatpak-spawn. The [e]sp pattern keeps pkill from matching its own shell.
HOST := $(if $(shell command -v flatpak-spawn 2>/dev/null),flatpak-spawn --host,)

.PHONY: help build flash monitor deploy fresh kill-monitors test test-quick menuconfig size clean

help:
	@echo "make build       compile firmware"
	@echo "make flash       build if needed, then flash to PORT=$(PORT)"
	@echo "make monitor     serial console (exit with Ctrl+])"
	@echo "make deploy      build, flash and open the serial console"
	@echo "make fresh       new board: build, erase it completely, flash (asks first)"
	@echo "make kill-monitors  stop every serial monitor and free PORT"
	@echo "                 (flash, monitor and deploy do this first)"
	@echo "make test        all host tests, normal and sanitized"
	@echo "make test-quick  host tests without sanitizers"
	@echo "make menuconfig  Wi-Fi, server URL, phone controls and other options"
	@echo "make size        static memory footprint"

build:
	@$(IDF) build

flash: kill-monitors
	@$(IDF) -p $(PORT) flash

monitor: kill-monitors
	@$(IDF) -p $(PORT) monitor

deploy: kill-monitors
	@$(IDF) -p $(PORT) flash monitor

# A new board: erase once (Waveshare's demo leaves data behind), then flash.
# Erasing deletes pairing, number and settings, so it shows the board's MAC
# and asks first. Building comes first: a build error erases nothing.
fresh: build kill-monitors
	@echo "Board on $(PORT):"
	@source "$(IDF_PATH)/export.sh" >/dev/null 2>&1 && esptool.py --port $(PORT) read_mac 2>/dev/null | grep -m1 "MAC:" \
		|| { echo "No board answers on $(PORT)"; exit 1; }
	@echo "This erases EVERYTHING on that board: pairing, number, settings, saved Wi-Fi."
	@read -r -p "Type ERASE to continue: " answer; [ "$$answer" = ERASE ] || { echo "Cancelled, nothing erased"; exit 1; }
	@$(IDF) -p $(PORT) erase-flash
	@$(IDF) -p $(PORT) flash
	@echo "Done. Join the Digits-Setup-XXXX network with the setup password to start the wizard."

# Hidden VS Code task terminals can keep an old monitor holding the port.
kill-monitors:
	@$(HOST) sh -c 'pkill -f "[e]sp_idf_monitor|[i]df_monitor\.py" || true; \
		fuser -s -k -TERM $(PORT) 2>/dev/null || true; sleep 0.5; \
		if fuser -s $(PORT) 2>/dev/null; then echo "$(PORT) still busy:"; fuser -v $(PORT); exit 1; \
		else echo "$(PORT) is free"; fi'

# Some harnesses compile against managed components installed by a build.
test: build
	@IDF_PATH="$(IDF_PATH)" tests/run_host_tests.sh

test-quick: build
	@IDF_PATH="$(IDF_PATH)" tests/run_host_tests.sh --quick

menuconfig:
	@$(IDF) menuconfig

size:
	@$(IDF) size

clean:
	@$(IDF) fullclean
