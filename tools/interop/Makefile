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
