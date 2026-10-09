package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
	"testing"
)

const testHardwareID = "74d0527f-d18b-4f67-938c-6f26a307cb38"

func testStatePath(t *testing.T) string {
	t.Helper()
	return filepath.Join(t.TempDir(), "private", "device.json")
}

func openTestState(t *testing.T, path, hardware, number, token string) *stateStore {
	t.Helper()
	s, err := openState(path, hardware, number, token)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := s.Close(); err != nil {
			t.Error(err)
		}
	})
	return s
}

func TestStateFirstBootPersistsIndependentUUID(t *testing.T) {
	path := testStatePath(t)
	s := openTestState(t, path, "", "", "")
	first := s.Snapshot()
	if first.Version != 1 || !validHardwareID(first.HardwareID) || first.Number != "unpaired" || first.DeviceToken != "" {
		t.Fatalf("unexpected first boot state: %+v", first)
	}
	for path, mode := range map[string]os.FileMode{filepath.Dir(path): 0700, path: 0600, path + ".lock": 0600} {
		info, err := os.Stat(path)
		if err != nil || info.Mode().Perm() != mode {
			t.Fatalf("permissions for %s: info=%v err=%v", path, info, err)
		}
	}
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	second := openTestState(t, path, "", "", "").Snapshot()
	if second != first {
		t.Fatal("reopening changed persistent identity")
	}
	other := openTestState(t, testStatePath(t), "", "", "").Snapshot()
	if first.HardwareID == other.HardwareID {
		t.Fatal("independent state files reused a hardware UUID")
	}
}

func TestStatePairingAndRenumberPreserveOriginalToken(t *testing.T) {
	path := testStatePath(t)
	s := openTestState(t, path, "", "", "")
	hardware := s.Snapshot().HardwareID
	token := strings.Repeat("aB", 32)
	if err := s.SavePairing("00123", token); err != nil {
		t.Fatal(err)
	}
	if err := s.SaveNumber("00456"); err != nil {
		t.Fatal(err)
	}
	want := deviceState{Version: 1, HardwareID: hardware, Number: "00456", DeviceToken: token}
	if s.Snapshot() != want {
		t.Fatal("renumber changed original credentials")
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	var onDisk deviceState
	if err := json.Unmarshal(data, &onDisk); err != nil || onDisk != want {
		t.Fatal("pairing/renumber did not commit the original token and number")
	}
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	if got := openTestState(t, path, "", "", "").Snapshot(); got != want {
		t.Fatal("paired state changed on subsequent boot")
	}
}

func TestStateCredentialImportAndConflictingFlags(t *testing.T) {
	path := testStatePath(t)
	token := strings.Repeat("42", 32)
	s := openTestState(t, path, testHardwareID, "987", token)
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	s = openTestState(t, path, testHardwareID, "987", token)
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	before, _ := os.ReadFile(path)
	for _, args := range [][3]string{
		{"e365caaf-ab84-4f6c-bc9a-47b6b7a385a8", "", ""},
		{"", "988", ""},
		{"", "", strings.Repeat("13", 32)},
	} {
		_, err := openState(path, args[0], args[1], args[2])
		if err == nil || !strings.Contains(err.Error(), "different --state") {
			t.Fatalf("conflicting flag was accepted: %v", err)
		}
		if strings.Contains(err.Error(), token) || strings.Contains(err.Error(), args[2]) && args[2] != "" {
			t.Fatal("credential error exposed a token")
		}
	}
	after, _ := os.ReadFile(path)
	if string(before) != string(after) {
		t.Fatal("conflicting flags overwrote persistent identity")
	}
	openTestState(t, path, "", "", "") // Failure paths must release the lock.
}

func TestStateRejectsPartialAndMalformedImports(t *testing.T) {
	for name, args := range map[string][3]string{
		"token without identity": {"", "123", strings.Repeat("ab", 32)},
		"number without token":   {testHardwareID, "123", ""},
		"token without number":   {testHardwareID, "", strings.Repeat("ab", 32)},
		"invalid hardware":       {"legacy-id", "", ""},
		"v1 hardware":            {"74d0527f-d18b-1f67-938c-6f26a307cb38", "", ""},
		"short token":            {testHardwareID, "123", "abcd"},
		"invalid token":          {testHardwareID, "123", strings.Repeat("zz", 32)},
		"oversized number":       {testHardwareID, strings.Repeat("1", 32), strings.Repeat("ab", 32)},
		"nondigit number":        {testHardwareID, "1-2", strings.Repeat("ab", 32)},
	} {
		t.Run(name, func(t *testing.T) {
			path := testStatePath(t)
			if _, err := openState(path, args[0], args[1], args[2]); err == nil {
				t.Fatal("malformed/partial identity was accepted")
			}
			if _, err := os.Stat(path); !os.IsNotExist(err) {
				t.Fatal("rejected identity left a state file")
			}
			openTestState(t, path, "", "", "")
		})
	}
}

func TestStateRejectsCorruptionWithoutRegeneratingIdentity(t *testing.T) {
	valid := `{"version":1,"hardware_id":"` + testHardwareID + `","number":"unpaired","device_token":""}`
	for name, data := range map[string]string{
		"truncated":            valid[:len(valid)-3],
		"unknown field":        strings.TrimSuffix(valid, "}") + `,"unexpected":true}`,
		"trailing JSON":        valid + `{}`,
		"trailing junk":        valid + "garbage",
		"unsupported version":  strings.Replace(valid, `"version":1`, `"version":2`, 1),
		"missing hardware":     `{"version":1,"number":"unpaired"}`,
		"missing number":       `{"version":1,"hardware_id":"` + testHardwareID + `"}`,
		"missing paired token": strings.Replace(valid, `"unpaired"`, `"123"`, 1),
		"null":                 `null`,
		"too large":            valid + strings.Repeat(" ", maxStateBytes),
	} {
		t.Run(name, func(t *testing.T) {
			path := testStatePath(t)
			if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
				t.Fatal(err)
			}
			if err := os.WriteFile(path, []byte(data), 0600); err != nil {
				t.Fatal(err)
			}
			if _, err := openState(path, "", "", ""); err == nil {
				t.Fatal("corrupt state was accepted")
			}
			after, err := os.ReadFile(path)
			if err != nil || string(after) != data {
				t.Fatal("corrupt state was replaced instead of reported")
			}
			if err := os.WriteFile(path, []byte(valid), 0600); err != nil {
				t.Fatal(err)
			}
			openTestState(t, path, "", "", "")
		})
	}
}

func TestStateExclusiveLockAndClose(t *testing.T) {
	path := testStatePath(t)
	first := openTestState(t, path, "", "", "")
	if _, err := openState(path, "", "", ""); err == nil || !strings.Contains(err.Error(), "already in use") {
		t.Fatalf("concurrent reuse did not fail: %v", err)
	}
	if err := first.Close(); err != nil {
		t.Fatal(err)
	}
	if err := first.Close(); err != nil {
		t.Fatal("Close must be idempotent")
	}
	if err := first.SavePairing("123", strings.Repeat("ab", 32)); err == nil {
		t.Fatal("closed store accepted pairing")
	}
	second := openTestState(t, path, "", "", "")
	if second.Snapshot() != first.Snapshot() {
		t.Fatal("unlock/relock changed state")
	}
	if _, err := os.Stat(path + ".lock"); err != nil {
		t.Fatal("lockfile must remain to prevent an inode race")
	}
}

func TestStateRejectsInsecurePermissionsAndSymlinks(t *testing.T) {
	for _, kind := range []string{"directory", "state", "lock", "state-symlink", "lock-symlink", "directory-symlink", "state-fifo"} {
		t.Run(kind, func(t *testing.T) {
			path := testStatePath(t)
			original := openTestState(t, path, "", "", "")
			if err := original.Close(); err != nil {
				t.Fatal(err)
			}
			switch kind {
			case "directory":
				if err := os.Chmod(filepath.Dir(path), 0755); err != nil {
					t.Fatal(err)
				}
			case "state":
				if err := os.Chmod(path, 0644); err != nil {
					t.Fatal(err)
				}
			case "lock":
				if err := os.Chmod(path+".lock", 0644); err != nil {
					t.Fatal(err)
				}
			case "state-symlink", "lock-symlink":
				target := path
				if kind == "lock-symlink" {
					target += ".lock"
				}
				if err := os.Rename(target, target+".original"); err != nil {
					t.Fatal(err)
				}
				if err := os.Symlink(target+".original", target); err != nil {
					t.Fatal(err)
				}
			case "directory-symlink":
				dir := filepath.Dir(path)
				if err := os.Rename(dir, dir+".original"); err != nil {
					t.Fatal(err)
				}
				if err := os.Symlink(dir+".original", dir); err != nil {
					t.Fatal(err)
				}
			case "state-fifo":
				if err := os.Remove(path); err != nil {
					t.Fatal(err)
				}
				if err := syscall.Mkfifo(path, 0600); err != nil {
					t.Fatal(err)
				}
			}
			if _, err := openState(path, "", "", ""); err == nil {
				t.Fatal("unsafe state path was accepted")
			}
		})
	}
}

func TestStateFailedCommitDoesNotChangeMemoryAndCleansTemporaryFile(t *testing.T) {
	path := testStatePath(t)
	s := openTestState(t, path, "", "", "")
	want := s.Snapshot()
	destinationDir := filepath.Join(filepath.Dir(path), "cannot-replace-directory")
	if err := os.Mkdir(destinationDir, 0700); err != nil {
		t.Fatal(err)
	}
	s.path = destinationDir
	if err := s.SavePairing("123", strings.Repeat("ab", 32)); err == nil {
		t.Fatal("rename failure was ignored")
	}
	if s.Snapshot() != want {
		t.Fatal("failed commit changed in-memory credentials")
	}
	matches, err := filepath.Glob(filepath.Join(filepath.Dir(path), ".device-state-*"))
	if err != nil || len(matches) != 0 {
		t.Fatal("failed commit left a temporary credential file")
	}
	s.path = path
	if err := s.SavePairing("123", strings.Repeat("ab", 32)); err != nil {
		t.Fatal("failed commit poisoned store")
	}
}

func TestStateInvalidServerCredentialsDoNotChangeSnapshot(t *testing.T) {
	s := openTestState(t, testStatePath(t), "", "", "")
	before := s.Snapshot()
	if err := s.SaveNumber("123"); err == nil {
		t.Fatal("unpaired renumber was accepted")
	}
	for _, args := range [][2]string{{"123", ""}, {"unpaired", strings.Repeat("ab", 32)}, {"1-2", strings.Repeat("ab", 32)}, {"123", "not-a-token"}} {
		if err := s.SavePairing(args[0], args[1]); err == nil {
			t.Fatal("invalid server pairing was accepted")
		}
		if s.Snapshot() != before {
			t.Fatal("invalid server pairing changed snapshot")
		}
	}
}

func TestStateSnapshotConcurrentWithRenumber(t *testing.T) {
	s := openTestState(t, testStatePath(t), testHardwareID, "123", strings.Repeat("ab", 32))
	var readers sync.WaitGroup
	for i := 0; i < 4; i++ {
		readers.Add(1)
		go func() {
			defer readers.Done()
			for j := 0; j < 100; j++ {
				if err := validateDeviceState(s.Snapshot()); err != nil {
					t.Error("reader observed partial snapshot:", err)
				}
			}
		}()
	}
	for i := 0; i < 4; i++ {
		if err := s.SaveNumber("456"); err != nil {
			t.Fatal(err)
		}
	}
	readers.Wait()
}
