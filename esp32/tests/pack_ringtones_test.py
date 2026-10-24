"""Run tools/pack_ringtones.py on generated audio (needs ffmpeg) and check the
image layout read by main/ringtone_pack.c: header, sorted entries, trimming,
the 30 s cut with fade, peak normalization and the error cases."""
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

repo = Path(__file__).resolve().parents[2]
packer = repo / "esp32/tools/pack_ringtones.py"
if not shutil.which("ffmpeg"):
    print("pack_ringtones_test: skipped, ffmpeg not installed")
    sys.exit(0)


def tone(path: Path, seconds: float, hz: int, pad: float = 0.0) -> None:
    # Sine with optional leading/trailing silence.
    filters = f"sine=frequency={hz}:duration={seconds}:sample_rate=44100"
    command = ["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", filters]
    if pad:
        command += ["-af", f"adelay={int(pad * 1000)},apad=pad_dur={pad}"]
    subprocess.run(command + [str(path)], check=True)


def pack(directory: Path, out: Path, size: str = "0x800000") -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(packer), "--dir", str(directory), "--out", str(out),
                           "--partition-size", size], capture_output=True, text=True)


with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    clips, out = root / "ringtones", root / "image.bin"
    clips.mkdir()

    # Empty folder (only README): valid empty image, no ffmpeg use.
    (clips / "README.md").write_text("docs")
    assert pack(clips, out).returncode == 0
    magic, version, count, rate, entries = struct.unpack("<4sHHII", out.read_bytes()[:16])
    assert (magic, version, count, rate, entries) == (b"DRTP", 1, 0, 24000, 16)

    tone(clips / "20-Long tone.wav", 40, 880)
    tone(clips / "12-Short tone.mp3", 2, 440, pad=1.0)
    result = pack(clips, out)
    assert result.returncode == 0, result.stderr
    image = out.read_bytes()
    magic, version, count, rate, entries = struct.unpack("<4sHHII", image[:16])
    assert count == 2 and rate == 24000
    parsed = [struct.unpack("<HHII52s", image[16 + 64 * i:16 + 64 * (i + 1)]) for i in range(2)]
    ids = [p[0] for p in parsed]
    assert ids == [12, 20], ids  # Sorted by number, not by file order.
    names = [p[4].rstrip(b"\0").decode() for p in parsed]
    assert names == ["Short tone", "Long tone"], names
    for ringtone, _, offset, samples, _ in parsed:
        assert offset % 4 == 0 and offset + 2 * samples <= len(image)
        pcm = struct.unpack(f"<{samples}h", image[offset:offset + 2 * samples])
        peak = max(abs(s) for s in pcm)
        assert 4000 <= peak <= 4096, (ringtone, peak)
        if ringtone == 12:
            # 1 s of padding on each side trimmed away: about 2 s remain.
            assert 1.9 * 24000 <= samples <= 2.1 * 24000, samples
            assert abs(pcm[0]) < 2000  # Starts at the tone, not mid-silence.
        else:
            assert samples == 30 * 24000  # Cut at 30 s ...
            tail = max(abs(s) for s in pcm[-240:])
            assert tail < 100, tail  # ... with a fade-out.

    # Error cases: bad name, duplicate number, too big for the partition.
    (clips / "5-Too low.wav").write_bytes((clips / "12-Short tone.mp3").read_bytes())
    assert "NN-Name" in pack(clips, out).stderr
    (clips / "5-Too low.wav").unlink()
    shutil.copy(clips / "12-Short tone.mp3", clips / "12_Duplicate.mp3")
    assert "used twice" in pack(clips, out).stderr
    (clips / "12_Duplicate.mp3").unlink()
    assert "do not fit" in pack(clips, out, "0x10000").stderr

print("pack_ringtones_test: layout, ordering, trimming, cut, fade, level and errors passed")
