#!/usr/bin/env python3
"""Pack esp32/ringtones/NN-Name.<audio> into the "ringtones" partition image.

Each file is decoded with ffmpeg to mono PCM16 at SAMPLE_RATE, trimmed of
leading/trailing silence, cut at MAX_SECONDS with a fade-out and normalized
to the ringer's peak level. Layout (little endian), read by
main/ringtone_pack.c:

  header  16 bytes: "DRTP", u16 version=1, u16 count, u32 sample_rate,
                    u32 entries_offset (=16)
  entries 64 bytes each, sorted by id: u16 id, u16 reserved, u32 offset,
                    u32 samples, char name[52] (UTF-8, NUL padded)
  data    PCM16 samples, each clip 4-byte aligned

With no files the image holds just an empty header, so no ffmpeg is needed.
"""
from array import array
from pathlib import Path
import argparse
import re
import shutil
import struct
import subprocess
import sys

SAMPLE_RATE = 24000
MAX_SECONDS = 30
FADE_SECONDS = 1
PEAK = 4096  # Matches the built-in ringtones' level.
SILENCE = 0.01  # Fraction of the peak counted as silence when trimming.
NAME_BYTES = 52
HEADER = struct.Struct("<4sHHII")
ENTRY = struct.Struct("<HHII52s")
NAME_PATTERN = re.compile(r"^(\d{2})[-_ ](.+)\.[A-Za-z0-9]+$")


def decode(path: Path) -> array:
    command = ["ffmpeg", "-v", "error", "-nostdin", "-i", str(path), "-t", str(MAX_SECONDS + 1),
               "-ac", "1", "-ar", str(SAMPLE_RATE), "-f", "s16le", "-"]
    result = subprocess.run(command, capture_output=True)
    if result.returncode != 0:
        raise SystemExit(f"{path.name}: ffmpeg failed: {result.stderr.decode(errors='replace').strip()}")
    samples = array("h")
    samples.frombytes(result.stdout[: len(result.stdout) // 2 * 2])
    if sys.byteorder != "little":
        samples.byteswap()
    return samples


def prepare(samples: array, name: str) -> array:
    peak = max((abs(s) for s in samples), default=0)
    if peak == 0:
        raise SystemExit(f"{name}: no audio")
    threshold = peak * SILENCE
    first = next(i for i, s in enumerate(samples) if abs(s) > threshold)
    last = len(samples) - next(i for i, s in enumerate(reversed(samples)) if abs(s) > threshold)
    samples = samples[first:last]
    limit = MAX_SECONDS * SAMPLE_RATE
    if len(samples) > limit:
        print(f"ringtones: {name}: cut to {MAX_SECONDS} s", file=sys.stderr)
        samples = samples[:limit]
        fade = FADE_SECONDS * SAMPLE_RATE
        for i in range(fade):
            samples[limit - fade + i] = int(samples[limit - fade + i] * (fade - i) / fade)
    peak = max(abs(s) for s in samples)
    return array("h", (int(s * PEAK / peak) for s in samples))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dir", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--partition-size", type=lambda v: int(v, 0), required=True)
    args = parser.parse_args()

    files = []
    if args.dir.is_dir():
        for path in sorted(args.dir.iterdir()):
            if path.name.startswith(".") or path.name == "README.md" or not path.is_file():
                continue
            match = NAME_PATTERN.match(path.name)
            if not match or not 10 <= int(match.group(1)) <= 99:
                raise SystemExit(f"ringtones: {path.name}: name it NN-Name.ext with NN from 10 to 99")
            files.append((int(match.group(1)), match.group(2), path))
    ids = [f[0] for f in files]
    duplicates = sorted({i for i in ids if ids.count(i) > 1})
    if duplicates:
        raise SystemExit(f"ringtones: number used twice: {', '.join(map(str, duplicates))}")
    if files and not shutil.which("ffmpeg"):
        raise SystemExit("ringtones: ffmpeg is needed to convert the files in " + str(args.dir))

    entries_size = HEADER.size + ENTRY.size * len(files)
    offset = (entries_size + 3) & ~3
    entries, blobs = [], []
    for ringtone, name, path in sorted(files):
        pcm = prepare(decode(path), path.name)
        encoded = name.encode("utf-8")[: NAME_BYTES - 1]
        entries.append(ENTRY.pack(ringtone, 0, offset, len(pcm), encoded))
        if sys.byteorder != "little":
            pcm.byteswap()
        data = pcm.tobytes() + b"\0" * (-len(pcm.tobytes()) % 4)
        blobs.append(data)
        offset += len(data)
        print(f"ringtones: {ringtone:2d} {name}: {len(pcm) / SAMPLE_RATE:.1f} s", file=sys.stderr)
    if offset > args.partition_size:
        raise SystemExit(f"ringtones: {offset / 1048576:.1f} MiB of clips do not fit the "
                         f"{args.partition_size / 1048576:.1f} MiB partition; shorten or remove some")
    image = HEADER.pack(b"DRTP", 1, len(files), SAMPLE_RATE, HEADER.size) + b"".join(entries)
    image += b"\0" * (-len(image) % 4) + b"".join(blobs)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(image)
    print(f"ringtones: {len(files)} clip(s), {len(image)} bytes", file=sys.stderr)


if __name__ == "__main__":
    main()
