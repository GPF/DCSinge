#!/usr/bin/env python3
"""
Trim PCM WAV files to a maximum frame count.

This is intended for Dreamcast SFX files that must stay at or below the
KOS snd_sfx_load() limit of 65534 samples.

Usage:
  tools/trim_wavs.py --apply data/spacerocks/singe/spacerocks/assets
  tools/trim_wavs.py --max-frames 65534 --apply data

By default the script is a dry run. Use --apply to rewrite files in place.
"""

from __future__ import annotations

import argparse
import os
import tempfile
import wave
import shutil
import subprocess
from pathlib import Path


def trim_wav(path: Path, max_frames: int, apply: bool, backup_ext: str) -> tuple[bool, int, int]:
    with wave.open(str(path), "rb") as src:
        params = src.getparams()
        frames = src.getnframes()

        if frames <= max_frames:
            return False, frames, frames

        if src.getcomptype() != "NONE":
            raise ValueError(f"{path}: compressed WAV not supported")

        src.rewind()
        payload = src.readframes(max_frames)

    if apply:
        backup_path = path.with_name(path.name + backup_ext)
        if not backup_path.exists():
            shutil.copy2(path, backup_path)
        tmp_fd, tmp_name = tempfile.mkstemp(prefix=path.name + ".", suffix=".tmp", dir=str(path.parent))
        try:
            with os.fdopen(tmp_fd, "wb") as tmp_fp:
                with wave.open(tmp_fp, "wb") as dst:
                    dst.setnchannels(params.nchannels)
                    dst.setsampwidth(params.sampwidth)
                    dst.setframerate(params.framerate)
                    dst.setcomptype("NONE", "not compressed")
                    dst.writeframes(payload)
            os.replace(tmp_name, path)
        finally:
            if os.path.exists(tmp_name):
                os.unlink(tmp_name)

    return True, frames, max_frames


def downsample_wav(path: Path, target_rate: int, apply: bool, backup_ext: str) -> tuple[bool, int, int]:
    with wave.open(str(path), "rb") as src:
        params = src.getparams()
        frames = src.getnframes()
        rate = src.getframerate()

    if rate <= target_rate:
        return False, frames, frames

    if apply:
        backup_path = path.with_name(path.name + backup_ext)
        if not backup_path.exists():
            shutil.copy2(path, backup_path)

        tmp_fd, tmp_name = tempfile.mkstemp(prefix=path.name + ".", suffix=".wav", dir=str(path.parent))
        os.close(tmp_fd)
        try:
            cmd = [
                "sox",
                str(path),
                "-r", str(target_rate),
                "-c", "1",
                "-t", "wav",
                tmp_name,
            ]
            subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            os.replace(tmp_name, path)
        finally:
            if os.path.exists(tmp_name):
                os.unlink(tmp_name)

    new_frames = max(1, int(round(frames * (target_rate / rate))))
    return True, frames, new_frames


def iter_wavs(root: Path):
    if root.is_file():
        if root.suffix.lower() == ".wav":
            yield root
        return

    for path in root.rglob("*.wav"):
        if path.is_file():
            yield path


def iter_backups(root: Path, backup_ext: str):
    if root.is_file():
        if root.name.endswith(backup_ext):
            yield root
        return

    for path in root.rglob(f"*{backup_ext}"):
        if path.is_file():
            yield path


def wav_frames(path: Path) -> int:
    with wave.open(str(path), "rb") as src:
        return src.getnframes()


def main() -> int:
    parser = argparse.ArgumentParser(description="Trim WAV files to a frame limit")
    parser.add_argument("paths", nargs="+", help="WAV file(s) or directories to scan")
    parser.add_argument("--max-frames", type=int, default=65534, help="Maximum frames to keep (default: 65534)")
    parser.add_argument("--backup-ext", default=".orig", help="Backup extension to use when rewriting files (default: .orig)")
    parser.add_argument("--target-rate", type=int, default=22050, help="Resample rate to use in --downsample mode (default: 22050)")
    parser.add_argument("--apply", action="store_true", help="Rewrite files in place")
    parser.add_argument("--downsample", action="store_true", help="Resample WAVs to a lower rate before trimming")
    parser.add_argument("--restore", action="store_true", help="Restore files from backup copies")
    args = parser.parse_args()

    if args.restore and args.downsample:
        raise SystemExit("--restore and --downsample are mutually exclusive")
    if args.restore and args.apply:
        raise SystemExit("--restore and --apply are mutually exclusive")

    found = 0
    trimmed = 0

    if args.restore:
        for arg in args.paths:
            for backup_path in iter_backups(Path(arg), args.backup_ext):
                found += 1
                original_path = Path(str(backup_path)[: -len(args.backup_ext)])
                if not backup_path.exists():
                    print(f"ERR  {backup_path}: missing backup")
                    continue
                shutil.copy2(backup_path, original_path)
                trimmed += 1
                print(f"RESTORE {backup_path} -> {original_path}")

        print(f"\nScanned {found} backup file(s); restored {trimmed}.")
        return 0

    wav_paths = []
    for arg in args.paths:
        wav_paths.extend(iter_wavs(Path(arg)))

    if args.downsample:
        # Work largest-first so the biggest memory consumers are handled first.
        wav_paths = sorted(wav_paths, key=wav_frames, reverse=True)

    for wav_path in wav_paths:
        found += 1
        try:
            if args.downsample:
                did_trim, before, after = downsample_wav(wav_path, args.target_rate, args.apply, args.backup_ext)
            else:
                did_trim, before, after = trim_wav(wav_path, args.max_frames, args.apply, args.backup_ext)
        except Exception as exc:
            print(f"ERR  {wav_path}: {exc}")
            continue

        if did_trim:
            trimmed += 1
            action = "DOWNSAMPLE" if args.downsample else "TRIM"
            if not args.apply:
                action = "WOULD " + action
            backup_note = f" backup={wav_path.name + args.backup_ext}" if args.apply else ""
            extra = f" rate->{args.target_rate}Hz" if args.downsample else ""
            print(f"{action} {wav_path}  {before} -> {after} frames{extra}{backup_note}")
        else:
            print(f"OK   {wav_path}  {before} frames")

    print(f"\nScanned {found} WAV file(s); trimmed {trimmed}.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
