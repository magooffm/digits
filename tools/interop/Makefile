.PHONY: build test race vet

# The upstream Opus wrapper offers this tag to omit Ogg/file decoding. Raw
# Opus encode/decode is unchanged; this tool therefore needs only libopus.
GO ?= go
GOFLAGS_INTEROP = -tags nolibopusfile

build:
	$(GO) build $(GOFLAGS_INTEROP) -o bin/digits-pion .

test:
	$(GO) test $(GOFLAGS_INTEROP) ./...

race:
	$(GO) test $(GOFLAGS_INTEROP) -race ./...

vet:
	$(GO) vet $(GOFLAGS_INTEROP) ./...

# Container build: works without local Go/opus-devel, including from a Flatpak
# VS Code terminal (podman is reached through flatpak-spawn --host).
IMAGE ?= localhost/digits-interop-test:fedora44
REPO_ROOT := $(abspath ../..)
PODMAN := $(if $(shell command -v podman 2>/dev/null),podman,flatpak-spawn --host podman)
CONTAINER_RUN = $(PODMAN) run --rm --network host -v "$(REPO_ROOT):/work:z" \
	-w /work/tools/interop -e GOMODCACHE=/work/tools/interop/.cache/mod \
	-e GOCACHE=/work/tools/interop/.cache/build $(IMAGE)

.PHONY: container-image container-build container-test stop-live live live-record mix

container-image:
	@$(PODMAN) image exists $(IMAGE) || $(PODMAN) build -t $(IMAGE) -f Containerfile .

container-build: container-image
	$(CONTAINER_RUN) make build

container-test: container-image
	$(CONTAINER_RUN) sh -c 'make vet && make test && make race'

# Live two-way call peer: PC microphone and speaker via parec/pacat. The
# server defaults to the ESP32 firmware's configured URL. Use a headset.
SERVER ?= $(or $(DIGITS_SERVER_URL),$(shell sed -n 's/^CONFIG_DIGITS_SERVER_URL="\(.*\)"$$/\1/p' ../../esp32/sdkconfig 2>/dev/null))
STATE ?= $(HOME)/.config/digits-interop/device.json
# Set to record each call: receive-*.wav (the ESP32) and send-*.wav (your mic).
RECORD_DIR ?=

# One identity allows one running client. Stop an older live peer for the same
# state file first (SIGTERM: it hangs up and finalizes recordings cleanly).
HOST := $(if $(shell command -v flatpak-spawn 2>/dev/null),flatpak-spawn --host,)
stop-live:
	@$(HOST) sh -c 'pids=$$(pgrep -f "bin/[d]igits-pion .*--state $(STATE)"); \
		[ -z "$$pids" ] && exit 0; echo "Stopping running helper $$pids"; kill -TERM $$pids; \
		for i in 1 2 3 4 5 6 7 8 9 10; do pgrep -f "bin/[d]igits-pion .*--state $(STATE)" >/dev/null || exit 0; sleep 0.5; done; \
		echo "Helper did not stop"; exit 1'

live: stop-live
	@test -x bin/digits-pion || $(MAKE) container-build
	./bin/digits-pion --server "$(SERVER)" --state "$(STATE)" --host-only --audio live \
		$(if $(RECORD_DIR),--record-dir "$(RECORD_DIR)")

live-record:
	@$(MAKE) live RECORD_DIR="$(HOME)/.config/digits-interop/audio"

# Combine the newest receive/send pair into call-*.wav: left = ESP32, right = PC.
AUDIO_DIR ?= $(HOME)/.config/digits-interop/audio
mix:
	@r=$$(ls -t "$(AUDIO_DIR)"/receive-*.wav 2>/dev/null | head -1); \
	s=$$(ls -t "$(AUDIO_DIR)"/send-*.wav 2>/dev/null | head -1); \
	if [ -z "$$r" ] || [ -z "$$s" ]; then echo "No receive/send pair in $(AUDIO_DIR); record with: make live-record"; exit 1; fi; \
	out="$${r%/*}/call-$${r##*/receive-}"; \
	ffmpeg -loglevel error -y -i "$$r" -i "$$s" \
		-filter_complex "[0:a][1:a]join=inputs=2:channel_layout=stereo[a]" -map "[a]" "$$out" && \
	chmod 600 "$$out" && echo "Wrote $$out (left: ESP32, right: PC microphone)"
