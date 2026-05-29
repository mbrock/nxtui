from __future__ import annotations

import os
import subprocess
from pathlib import Path


def env_int(name: str, default: int) -> int:
    return int(os.environ.get(name, str(default)))


def env_float(name: str, default: float) -> float:
    return float(os.environ.get(name, str(default)))


def env_float_first(names: list[str], default: float) -> float:
    for name in names:
        if name in os.environ:
            return float(os.environ[name])
    return default


def default_comm(default: str = "nxt-echo-bench") -> str:
    return os.environ.get(
        "BENCH_PERF_COMM",
        os.environ.get("BENCH_COMM", default))


def convert_perf_data(data: Path, json_path: Path) -> None:
    if not data.exists():
        raise SystemExit(f"perf data not found: {data}")

    if json_path.exists() and json_path.stat().st_mtime >= data.stat().st_mtime:
        return

    subprocess.run(
        [
            "sudo",
            "perf",
            "data",
            "convert",
            "--force",
            "-i",
            str(data),
            "--to-json",
            str(json_path),
        ],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    subprocess.run(
        ["sudo", "chmod", "a+r", str(json_path)],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def as_int(raw: object) -> int:
    if raw is None:
        return 0
    if isinstance(raw, int):
        return raw
    text = str(raw)
    return int(text, 16) if text.startswith("0x") else int(text)


def pct(part: int, total: int) -> str:
    return f"{100.0 * part / max(total, 1):3.0f}"


def n(value: int | float) -> str:
    if isinstance(value, float):
        return f"{value:,.1f}"
    return f"{value:,}"
