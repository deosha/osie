# SPDX-License-Identifier: Apache-2.0
"""Tokenizer and hashed features. MUST stay byte-identical to the C++ code in
src/intent/frame.cpp (tokenize) and src/inference/neural_parser.cpp
(token_features). Parity is checked by ml/evaluation/check_parity.py."""
from __future__ import annotations

from dataclasses import dataclass

_STRIP = b"._-"


def _tok_char(c: int) -> bool:
    return (48 <= c <= 57) or (65 <= c <= 90) or (97 <= c <= 122) or c in (46, 95, 45) or c >= 0x80


def _ascii_lower(b: bytes) -> bytes:
    return bytes(c + 32 if 65 <= c <= 90 else c for c in b)


@dataclass(frozen=True)
class Token:
    text: str
    lower: str
    begin: int  # byte offsets into the UTF-8 utterance
    end: int


def tokenize(text: str) -> list[Token]:
    raw = text.encode("utf-8")
    out: list[Token] = []
    i, n = 0, len(raw)
    while i < n:
        while i < n and not _tok_char(raw[i]):
            i += 1
        b = i
        while i < n and _tok_char(raw[i]):
            i += 1
        e = i
        while b < e and raw[b] in _STRIP:
            b += 1
        while e > b and raw[e - 1] in _STRIP:
            e -= 1
        if b == e:
            continue
        t = raw[b:e]
        out.append(Token(t.decode("utf-8", "replace"), _ascii_lower(t).decode("utf-8", "replace"), b, e))
    return out


def fnv1a(data: bytes) -> int:
    h = 2166136261
    for c in data:
        h ^= c
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def _shape(tok: bytes) -> bytes:
    out = bytearray()
    for c in tok:
        if 65 <= c <= 90:
            s = ord("X")
        elif 97 <= c <= 122:
            s = ord("x")
        elif 48 <= c <= 57:
            s = ord("d")
        elif c >= 0x80:
            s = ord("u")
        else:
            s = c
        if not out or out[-1] != s:
            out.append(s)
        if len(out) >= 8:
            break
    return bytes(out)


KNOWLEDGE = [  # must match knowledge_category_list() in src/inference/neural_parser.cpp
    ("cpu", "cpu cpus processor processors core cores load cycles"),
    ("mem", "memory ram mem swap swapping paging footprint leak leaking"),
    ("disk", "disk disks storage drive ssd volume volumes filesystem filesystems space"),
    ("net", "network wifi ethernet interface interfaces bandwidth internet traffic ip packets"),
    ("heat", "thermal temperature overheating overheat hot heat fan fans warm"),
    ("why", "why explain cause causing reason diagnose investigate troubleshoot wrong"),
    ("slow", "slow sluggish laggy lag lagging unresponsive freezing frozen beachball hang hanging stutter crawling performance"),
    ("use", "using uses use consuming consume hogging hog eating eat taking burning chewing"),
    ("top", "top most biggest highest heaviest largest busiest worst"),
    ("act", "kill terminate quit restart reboot renice stop close set change increase decrease lower raise pin disable "
            "enable clear free optimize"),
    ("k8s", "kubernetes k8s pod pods kubectl deployment kubelet replicas"),
    ("ctr", "container containers docker cgroup cgroups"),
    ("thr", "throttled throttling throttle quota limit limited"),
    ("tree", "parent child children spawned spawn started launched owns owner tree service services daemon"),
    ("sys", "os macos linux version kernel uptime hostname model distro boot booted"),
    ("ref", "it that this these those them one"),
    ("q", "what which who how where when"),
    ("pid", "pid process"),
    ("help", "help hello hi hey commands"),
    ("ifq", "if happen happens would"),
]
_KCAT: dict[bytes, str] = {}
for _name, _words in KNOWLEDGE:
    for _w in _words.split():
        _KCAT.setdefault(_w.encode(), _name)


def token_features(token_text: str, buckets: int, nmin: int = 2, nmax: int = 4, knowledge: bool = False) -> list[int]:
    raw = token_text.encode("utf-8")
    low = _ascii_lower(raw)
    feats = [fnv1a(b"w:" + low) % buckets]
    s = b"<" + low[:24] + b">"
    for n in range(nmin, nmax + 1):
        if n > len(s):
            break
        for i in range(len(s) - n + 1):
            feats.append(fnv1a(b"c" + str(n).encode() + b":" + s[i : i + n]) % buckets)
    feats.append(fnv1a(b"s:" + _shape(raw)) % buckets)
    if knowledge and low in _KCAT:
        feats.append(fnv1a(b"k:" + _KCAT[low].encode()) % buckets)
    return feats


TAGS = ["O", "B-PROC", "I-PROC", "B-PID", "I-PID", "B-REF", "I-REF", "B-NUM", "I-NUM"]

INTENTS = [
    "top_cpu", "top_memory", "diagnose_cpu", "diagnose_memory", "diagnose_slow", "process_cpu",
    "process_memory", "process_info", "process_tree", "system_info", "cpu_topology", "memory_status",
    "disk_usage", "network_status", "thermal_status", "explain_concept", "what_if", "container_throttle",
    "action_request", "kubernetes", "help", "unsupported",
]
