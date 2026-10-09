package main

import (
	"bytes"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sync"
	"syscall"
)

const (
	stateVersion  = 1
	maxStateBytes = 8 * 1024
)

// deviceState contains the original server-issued token, never its hash. A
// private snapshot is persisted before the signaling connection is restarted.
type deviceState struct {
	Version     int    `json:"version"`
	HardwareID  string `json:"hardware_id"`
	Number      string `json:"number"`
	DeviceToken string `json:"device_token"`
}

type stateStore struct {
	State  deviceState
	mu     sync.Mutex
	path   string
	lock   *os.File
	closed bool
}

// openState holds an advisory lock until Close. Overrides can import an existing
// identity into a new file; an existing identity must never be overwritten by
// flags that accidentally refer to a different handset.
func openState(path, hardwareID, number, token string) (*stateStore, error) {
	if path == "" {
		return nil, errors.New("state path is required")
	}
	absPath, err := filepath.Abs(path)
	if err != nil {
		return nil, fmt.Errorf("resolve state path: %w", err)
	}
	dir := filepath.Dir(absPath)
	if err := os.MkdirAll(dir, 0700); err != nil {
		return nil, fmt.Errorf("create private state directory: %w", err)
	}
	if err := checkPrivatePath(dir, true); err != nil {
		return nil, err
	}
	lockPath := absPath + ".lock"
	lock, err := os.OpenFile(lockPath, os.O_CREATE|os.O_RDWR|syscall.O_NOFOLLOW|syscall.O_NONBLOCK, 0600)
	if err != nil {
		return nil, fmt.Errorf("open state lock: %w", err)
	}
	if err := checkPrivateFile(lock, lockPath); err != nil {
		_ = lock.Close()
		return nil, err
	}
	if err := syscall.Flock(int(lock.Fd()), syscall.LOCK_EX|syscall.LOCK_NB); err != nil {
		_ = lock.Close()
		if errors.Is(err, syscall.EWOULDBLOCK) || errors.Is(err, syscall.EAGAIN) {
			return nil, errors.New("state is already in use by another client; use a different --state path for an independent device")
		}
		return nil, fmt.Errorf("lock device state: %w", err)
	}
	s := &stateStore{path: absPath, lock: lock}
	success := false
	defer func() {
		if !success {
			_ = s.Close()
		}
	}()

	stateFile, err := os.OpenFile(absPath, os.O_RDONLY|syscall.O_NOFOLLOW|syscall.O_NONBLOCK, 0)
	if err == nil {
		defer stateFile.Close()
		if err := checkPrivateFile(stateFile, absPath); err != nil {
			return nil, err
		}
		contents, err := io.ReadAll(io.LimitReader(stateFile, maxStateBytes+1))
		if err != nil {
			return nil, fmt.Errorf("read device state: %w", err)
		}
		if len(contents) > maxStateBytes {
			return nil, errors.New("device state exceeds 8 KiB; refusing to replace the identity")
		}
		dec := json.NewDecoder(bytes.NewReader(contents))
		dec.DisallowUnknownFields()
		if err := dec.Decode(&s.State); err != nil {
			return nil, fmt.Errorf("decode device state (not replaced): %w", err)
		}
		var extra any
		if err := dec.Decode(&extra); !errors.Is(err, io.EOF) {
			return nil, errors.New("device state contains trailing JSON or data; refusing to replace the identity")
		}
		if err := validateDeviceState(s.State); err != nil {
			return nil, fmt.Errorf("invalid saved device state (not replaced): %w", err)
		}
		if (hardwareID != "" && hardwareID != s.State.HardwareID) ||
			(number != "" && number != s.State.Number) ||
			(token != "" && token != s.State.DeviceToken) {
			return nil, errors.New("credential flags do not match saved device identity; use a different --state path to import another device")
		}
	} else if errors.Is(err, os.ErrNotExist) {
		if token != "" && hardwareID == "" {
			return nil, errors.New("credential import requires --hardware-id, --number and --device-token together")
		}
		if hardwareID == "" {
			hardwareID, err = generateStateHardwareID()
			if err != nil {
				return nil, fmt.Errorf("generate hardware UUID: %w", err)
			}
		}
		if token == "" {
			if number != "" && number != "unpaired" {
				return nil, errors.New("numeric --number requires --device-token; omit both to pair a new device")
			}
			number = "unpaired"
		}
		s.State = deviceState{Version: stateVersion, HardwareID: hardwareID, Number: number, DeviceToken: token}
		if err := validateDeviceState(s.State); err != nil {
			return nil, fmt.Errorf("invalid initial credentials: %w", err)
		}
		if err := s.persist(s.State); err != nil {
			return nil, err
		}
	} else {
		return nil, fmt.Errorf("open device state (not replaced): %w", err)
	}
	success = true
	return s, nil
}

// Snapshot is safe for concurrent signaling/status readers. The public State
// field is only for callers that already serialize access to this store.
func (s *stateStore) Snapshot() deviceState {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.State
}

func (s *stateStore) SavePairing(number, token string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed {
		return errors.New("device state is closed")
	}
	next := s.State
	next.Number, next.DeviceToken = number, token
	if token == "" {
		return errors.New("paired message must include the original device token")
	}
	if err := validateDeviceState(next); err != nil {
		return fmt.Errorf("invalid pairing credentials: %w", err)
	}
	if err := s.persist(next); err != nil {
		return err
	}
	s.State = next
	return nil
}

func (s *stateStore) SaveNumber(number string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed {
		return errors.New("device state is closed")
	}
	if s.State.DeviceToken == "" {
		return errors.New("cannot renumber an unpaired device")
	}
	next := s.State
	next.Number = number
	if err := validateDeviceState(next); err != nil {
		return fmt.Errorf("invalid corrected number: %w", err)
	}
	if err := s.persist(next); err != nil {
		return err
	}
	s.State = next
	return nil
}

func (s *stateStore) Close() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed {
		return nil
	}
	s.closed = true
	if s.lock == nil {
		return nil
	}
	unlockErr := syscall.Flock(int(s.lock.Fd()), syscall.LOCK_UN)
	closeErr := s.lock.Close()
	s.lock = nil
	// Do not unlink: another process may already be holding this same inode.
	return errors.Join(unlockErr, closeErr)
}

func (s *stateStore) persist(next deviceState) error {
	data, err := json.MarshalIndent(next, "", "  ")
	if err != nil {
		return fmt.Errorf("encode device state: %w", err)
	}
	data = append(data, '\n')
	dir := filepath.Dir(s.path)
	if err := checkPrivatePath(dir, true); err != nil {
		return err
	}
	// Open the directory before changing anything, then sync its rename below.
	directory, err := os.Open(dir)
	if err != nil {
		return fmt.Errorf("open state directory: %w", err)
	}
	defer directory.Close()
	tmp, err := os.CreateTemp(dir, ".device-state-*")
	if err != nil {
		return fmt.Errorf("create temporary device state: %w", err)
	}
	name := tmp.Name()
	defer os.Remove(name)
	defer tmp.Close()
	if err := tmp.Chmod(0600); err != nil {
		return fmt.Errorf("set device state permissions: %w", err)
	}
	if _, err := tmp.Write(data); err != nil {
		return fmt.Errorf("write device state: %w", err)
	}
	if err := tmp.Sync(); err != nil {
		return fmt.Errorf("sync device state: %w", err)
	}
	if err := tmp.Close(); err != nil {
		return fmt.Errorf("close device state: %w", err)
	}
	if err := os.Rename(name, s.path); err != nil {
		return fmt.Errorf("commit device state: %w", err)
	}
	if err := directory.Sync(); err != nil {
		return fmt.Errorf("sync committed state directory; stop client and inspect saved state before reconnecting: %w", err)
	}
	return nil
}

func checkPrivatePath(path string, directory bool) error {
	info, err := os.Lstat(path)
	if err != nil {
		return fmt.Errorf("inspect private state path: %w", err)
	}
	if directory && (!info.IsDir() || info.Mode()&os.ModeSymlink != 0) {
		return errors.New("state directory must be a real directory, not a symlink")
	}
	if info.Mode().Perm() != 0700 {
		return errors.New("state directory must have permissions 0700; choose a private directory or run chmod 700 on it")
	}
	if stat, ok := info.Sys().(*syscall.Stat_t); ok && stat.Uid != uint32(os.Geteuid()) {
		return errors.New("state directory must be owned by the current user")
	}
	return nil
}

func checkPrivateFile(file *os.File, name string) error {
	info, err := file.Stat()
	if err != nil {
		return fmt.Errorf("inspect device state file: %w", err)
	}
	if !info.Mode().IsRegular() {
		return fmt.Errorf("%s must be a regular private file", name)
	}
	if info.Mode().Perm() != 0600 {
		return fmt.Errorf("%s must have permissions 0600; run chmod 600 on the file", name)
	}
	if stat, ok := info.Sys().(*syscall.Stat_t); ok && stat.Uid != uint32(os.Geteuid()) {
		return fmt.Errorf("%s must be owned by the current user", name)
	}
	return nil
}

func validateDeviceState(s deviceState) error {
	if s.Version != stateVersion {
		return errors.New("unsupported or missing state version (expected 1)")
	}
	if !validHardwareID(s.HardwareID) {
		return errors.New("hardware_id must be a UUID v4")
	}
	if s.DeviceToken == "" {
		if s.Number != "unpaired" {
			return errors.New("missing device token for paired number; identity was not replaced")
		}
		return nil
	}
	if !validDeviceNumber(s.Number) {
		return errors.New("paired number must contain 1 to 31 decimal digits")
	}
	if len(s.DeviceToken) != 64 {
		return errors.New("device_token must be the original 64-character hexadecimal token")
	}
	if _, err := hex.DecodeString(s.DeviceToken); err != nil {
		return errors.New("device_token must be the original 64-character hexadecimal token")
	}
	return nil
}

func validDeviceNumber(number string) bool {
	if len(number) == 0 || len(number) > 31 {
		return false
	}
	for _, c := range number {
		if c < '0' || c > '9' {
			return false
		}
	}
	return true
}

func validHardwareID(id string) bool {
	if len(id) != 36 || id[8] != '-' || id[13] != '-' || id[18] != '-' || id[23] != '-' || id[14] != '4' {
		return false
	}
	if id[19] != '8' && id[19] != '9' && id[19] != 'a' && id[19] != 'b' && id[19] != 'A' && id[19] != 'B' {
		return false
	}
	for i, c := range id {
		if i == 8 || i == 13 || i == 18 || i == 23 {
			continue
		}
		if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f' || c >= 'A' && c <= 'F') {
			return false
		}
	}
	return true
}

func generateStateHardwareID() (string, error) {
	var uuid [16]byte
	if _, err := rand.Read(uuid[:]); err != nil {
		return "", err
	}
	uuid[6] = uuid[6]&0x0f | 0x40
	uuid[8] = uuid[8]&0x3f | 0x80
	return fmt.Sprintf("%x-%x-%x-%x-%x", uuid[0:4], uuid[4:6], uuid[6:8], uuid[8:10], uuid[10:16]), nil
}
