#!/usr/bin/env python3
"""Build the Lexicon dictionary files from WordNet 3.1 (Princeton, free license).

Downloads wn3.1.dict.tar.gz (~10 MB, cached in ~/.cache/express-ghalib/),
parses the data.{noun,verb,adj,adv} synset files and writes:

  out/lexicon/dict.txt   sorted "key\tdefinition" lines (one line per sense;
                         multi-word lemmas use underscores: kick_the_bucket)
  out/lexicon/dict.idx   "XY offset" per two-letter key prefix (a-z, else '_')

Copy the lexicon/ folder to the SD card root (easiest: the Files app's
"USB transfer -> PC" mode). The on-device bucket logic in src/lexicon.cpp
must match bucket() below — change both or neither.

Usage: python3 tools/build_lexicon.py [--out DIR]   (default: ./lexicon_out)
"""
import argparse
import hashlib
import io
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request

URL = "https://wordnetcode.princeton.edu/wn3.1.dict.tar.gz"
# Checksum of the archive this script was built and verified against. HTTPS
# authenticates the host, not the bytes it serves over time — pinning the digest
# means a swapped or truncated archive fails loudly instead of silently building
# a different dictionary.
SHA256 = "3f7d8be8ef6ecc7167d39b10d66954ec734280b5bdcd57f7d9eafe429d11c22a"


def verify(path, expected, allow_mismatch=False):
    """Abort unless `path` hashes to `expected`."""
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    got = h.hexdigest()
    if got == expected:
        return
    msg = (f"checksum mismatch for {path}\n"
           f"  expected {expected}\n  got      {got}")
    if allow_mismatch:
        print(f"WARNING: {msg}")
        return
    sys.exit(f"{msg}\nDelete the file to re-download, or pass "
             f"--allow-sha-mismatch if you trust this copy.")
CACHE = os.path.expanduser("~/.cache/express-ghalib/wn3.1.dict.tar.gz")
POS_FILES = {  # tar member -> part-of-speech tag shown in definitions
    "dict/data.noun": "n",
    "dict/data.verb": "v",
    "dict/data.adj": "adj",
    "dict/data.adv": "adv",
}


def bucket(key: str) -> str:
    """Two-letter bucket; MUST match bch() in src/lexicon.cpp."""
    def norm(c: str) -> str:
        return c if "a" <= c <= "z" else "_"
    b0 = norm(key[0])
    b1 = norm(key[1]) if len(key) > 1 else "_"
    return b0 + b1


def clean_gloss(gloss: str) -> str:
    """Keep the definition part; drop quoted example sentences."""
    parts = [p.strip() for p in gloss.split(";")]
    defs = [p for p in parts if p and not p.startswith('"')]
    return "; ".join(defs) if defs else gloss.strip()


def parse_data(f, tag: str):
    """Yield (key, definition) from a WordNet data.* file."""
    for raw in f:
        line = raw.decode("utf-8", "replace")
        if line.startswith("  "):  # license header
            continue
        if "|" not in line:
            continue
        head, gloss = line.split("|", 1)
        fields = head.split()
        try:
            w_cnt = int(fields[3], 16)
        except (IndexError, ValueError):
            continue
        gloss = clean_gloss(gloss)
        if not gloss:
            continue
        for i in range(w_cnt):
            word = fields[4 + i * 2]
            # strip adjective syntactic markers: word(p), word(a), word(ip)
            if word.endswith(")") and "(" in word:
                word = word[: word.rindex("(")]
            key = word.lower()
            if not key:
                continue
            yield key, f"({tag}) {gloss}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="lexicon_out")
    ap.add_argument("--allow-sha-mismatch", action="store_true",
                    help="proceed even if the tarball checksum differs "
                         "(use when Princeton republishes the archive)")
    args = ap.parse_args()

    if not os.path.exists(CACHE):
        os.makedirs(os.path.dirname(CACHE), exist_ok=True)
        print(f"downloading {URL} ...")
        # Prefer curl: stock macOS python3 often lacks SSL root certs.
        if shutil.which("curl"):
            subprocess.run(["curl", "-fL", "-o", CACHE + ".part", URL],
                           check=True)
        else:
            urllib.request.urlretrieve(URL, CACHE + ".part")
        verify(CACHE + ".part", SHA256, args.allow_sha_mismatch)
        os.rename(CACHE + ".part", CACHE)
    else:
        verify(CACHE, SHA256, args.allow_sha_mismatch)
    print(f"using {CACHE}")

    entries = set()
    with tarfile.open(CACHE, "r:gz") as tar:
        for member, tag in POS_FILES.items():
            f = tar.extractfile(member)
            if f is None:
                sys.exit(f"missing {member} in tarball")
            n = 0
            for key, definition in parse_data(io.BufferedReader(f), tag):
                entries.add((bucket(key), key, definition))
                n += 1
            print(f"  {member}: {n} senses")

    rows = sorted(entries)
    out_dir = os.path.join(args.out, "lexicon")
    os.makedirs(out_dir, exist_ok=True)
    dict_path = os.path.join(out_dir, "dict.txt")
    idx_path = os.path.join(out_dir, "dict.idx")

    offsets = {}  # bucket -> first byte offset
    with open(dict_path, "wb") as f:
        for b, key, definition in rows:
            if b not in offsets:
                offsets[b] = f.tell()
            f.write(f"{key}\t{definition}\n".encode("utf-8"))
        size = f.tell()
    with open(idx_path, "w") as f:
        for b in sorted(offsets):
            f.write(f"{b} {offsets[b]}\n")

    print(f"wrote {dict_path} ({size/1e6:.1f} MB, {len(rows)} senses)")
    print(f"wrote {idx_path} ({len(offsets)} buckets)")
    print(f"-> copy the '{out_dir}' folder to the SD card root")


if __name__ == "__main__":
    main()
