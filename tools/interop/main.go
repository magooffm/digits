package main

import (
	"bufio"
	"context"
	"flag"
	"fmt"
	"log/slog"
	"net/url"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"
)

const upstreamRevision = "a4edaaa557c06b3b687be9e92a869fb0cbcf68fc"

type options struct {
	server, audio, traceDir string
	hostOnly                bool
}

func validateServer(raw string) error {
	u, err := url.Parse(raw)
	if err != nil || u == nil || (u.Scheme != "ws" && u.Scheme != "wss") ||
		u.Hostname() == "" || u.Path != "/ws" || u.User != nil || u.RawQuery != "" || u.Fragment != "" {
		return fmt.Errorf("server must be ws://host[:port]/ws or wss://host[:port]/ws")
	}
	return nil
}

func main() {
	if err := execute(); err != nil {
		slog.Error("Client stopped", "error", err)
		os.Exit(1)
	}
}

func execute() error {
	configDir, err := os.UserConfigDir()
	if err != nil {
		return err
	}
	server := flag.String("server", os.Getenv("DIGITS_SERVER_URL"), "Digits WebSocket URL (also DIGITS_SERVER_URL)")
	statePath := flag.String("state", filepath.Join(configDir, "digits-interop", "device.json"), "persistent independent device state")
	hardware := flag.String("hardware-id", "", "import hardware UUID into a new state file")
	number := flag.String("number", "", "import paired number into a new state file")
	// Keep the bearer token out of flag defaults: --help prints defaults.
	token := flag.String("device-token", "", "import original device token (also DIGITS_DEVICE_TOKEN)")
	audio := flag.String("audio", "tone", "generated audio: tone or silence")
	hostOnly := flag.Bool("host-only", false, "ignore server STUN/TURN settings for a same-LAN host-candidate test")
	traceDir := flag.String("trace-dir", "", "optional private directory for per-call SDP and ICE traces")
	flag.Parse()
	if *token == "" {
		*token = os.Getenv("DIGITS_DEVICE_TOKEN")
	}
	if flag.NArg() != 0 {
		return fmt.Errorf("unexpected positional arguments; use --help")
	}
	if err := validateServer(*server); err != nil {
		return err
	}
	if *audio != "tone" && *audio != "silence" {
		return fmt.Errorf("audio must be tone or silence")
	}
	// Preserve user-selected Pion verbosity; make upstream ICE/DTLS warnings
	// visible by default so a generic failed state is not the only evidence.
	if os.Getenv("PION_LOG_ERROR") == "" && os.Getenv("PION_LOG_WARN") == "" &&
		os.Getenv("PION_LOG_INFO") == "" && os.Getenv("PION_LOG_DEBUG") == "" && os.Getenv("PION_LOG_TRACE") == "" {
		if err := os.Setenv("PION_LOG_WARN", "all"); err != nil {
			return err
		}
	}
	if err := prepareTraceDir(*traceDir); err != nil {
		return err
	}
	store, err := openState(*statePath, *hardware, *number, *token)
	if err != nil {
		return err
	}
	defer store.Close()
	slog.Info("Headless Digits/Pion client", "upstream", upstreamRevision, "pion", "4.2.20",
		"hardware_id", store.State.HardwareID, "number", store.State.Number, "state", *statePath,
		"audio", *audio, "host_only", *hostOnly)
	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer cancel()
	commands := make(chan string, 16)
	go readCommands(ctx, commands)
	printHelp()
	return newApplication(store, options{server: *server, audio: *audio, hostOnly: *hostOnly, traceDir: *traceDir}).run(ctx, commands)
}

func printHelp() {
	fmt.Fprintln(os.Stderr, "Commands: call NUMBER | answer | hangup | status | reconnect | help | quit")
}

func readCommands(ctx context.Context, output chan<- string) {
	defer close(output)
	scanner := bufio.NewScanner(os.Stdin)
	scanner.Buffer(make([]byte, 512), 512)
	for {
		fmt.Fprint(os.Stderr, "digits-pion> ")
		if !scanner.Scan() {
			break
		}
		select {
		case output <- strings.TrimSpace(scanner.Text()):
		case <-ctx.Done():
			return
		}
	}
	if err := scanner.Err(); err != nil {
		slog.Error("CLI input failed", "error", err)
	}
}
