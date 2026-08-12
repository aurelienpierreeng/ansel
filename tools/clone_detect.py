#!/usr/bin/env python3
"""Token-level clone detection between two source trees.

Answers "how much code do these two codebases actually share?" in a way a line diff
cannot. A line diff calls a reindented line changed, a reflowed argument list changed,
and a renamed local variable changed. On a fork that has restyled its tree - Ansel
converted 245 headers from `#pragma once` to include guards, so not one file is
byte-identical to darktable - line comparison understates sharing badly.

This works on TOKENS instead, using winnowing (Schleimer, Wilkerson & Aiken 2003), the
algorithm behind MOSS:

  1. tokenize, discarding whitespace and comments entirely
  2. hash every k-gram of consecutive tokens
  3. in each window of w consecutive hashes keep the minimum

Step 3 is what makes it work. Selecting fingerprints by a property of the hashes rather
than by position means the same code selects the same fingerprints wherever it sits in a
file, so insertions and deletions elsewhere do not shift the match. It guarantees
detecting any shared run of at least k + w - 1 tokens, while storing only about 1/w of
the hashes.

Two normalisations are reported, because they answer different questions:

  strict      identifiers kept. "Is this the same code?" Copy-paste with renaming
              counts as different.
  normalised  identifiers, numbers and strings replaced by placeholders, keywords and
              punctuation kept. "Is this the same code shape?" Catches a function
              carried across and renamed, which for a fork is still inherited code.

Usage:
  python3 tools/clone_detect.py --a /path/to/tree-a --b /path/to/tree-b -o clones.json
"""

import argparse
import json
import os
import re
import sys
from collections import defaultdict, deque

SOURCE_SUFFIXES = (".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".m", ".mm")
EXCLUDED_DIR_PARTS = ("/external/", "/tests/integration/", "/image_test/samples/",
                      "/apps/ansel-chart/", "/doxygen-awesome-css/", "/.git/")

# Order matters: comments and literals must be recognised before the operator rule,
# or a '/' would be tokenised on its own and the comment body treated as code.
TOKEN_RE = re.compile(r"""
      (?P<ws>\s+)
    | (?P<line_comment>//[^\n]*)
    | (?P<block_comment>/\*.*?\*/)
    | (?P<string>"(?:\\.|[^"\\])*")
    | (?P<char>'(?:\\.|[^'\\])*')
    | (?P<number>\.?\d[0-9A-Za-z_.]*(?:[eEpP][+-][0-9]+)?)
    | (?P<ident>[A-Za-z_][A-Za-z0-9_]*)
    | (?P<op>[^\sA-Za-z0-9_])
""", re.VERBOSE | re.DOTALL)

C_KEYWORDS = frozenset("""
auto break case char const continue default do double else enum extern float for goto
if inline int long register restrict return short signed sizeof static struct switch
typedef union unsigned void volatile while _Bool _Complex _Atomic _Generic
class namespace template typename public private protected virtual new delete this
operator try catch throw using bool true false nullptr constexpr static_cast
reinterpret_cast const_cast dynamic_cast
""".split())

MOD = (1 << 61) - 1          # Mersenne prime: cheap modular arithmetic, few collisions
BASE = 1000003


def is_excluded(path):
    p = "/" + path.replace(os.sep, "/").lstrip("/")
    return any(part in p for part in EXCLUDED_DIR_PARTS)


def tokenize(text, normalise):
    """Token strings, with comments and whitespace dropped."""
    out = []
    for m in TOKEN_RE.finditer(text):
        kind = m.lastgroup
        if kind in ("ws", "line_comment", "block_comment"):
            continue
        value = m.group()
        if normalise:
            if kind == "ident":
                value = value if value in C_KEYWORDS else "\x01"
            elif kind == "number":
                value = "\x02"
            elif kind in ("string", "char"):
                value = "\x03"
        out.append(value)
    return out


def fingerprints(tokens, k, w):
    """Winnowed fingerprints of a token list, as a set of hashes.

    The k-gram hashes are produced with a rolling polynomial hash, then winnowed with a
    monotonic deque so the whole pass is linear rather than O(n*w).
    """
    n = len(tokens)
    if n < k:
        return set()
    ids = [hash(t) & 0xFFFFFFFF for t in tokens]

    high = pow(BASE, k - 1, MOD)
    h = 0
    for i in range(k):
        h = (h * BASE + ids[i]) % MOD
    hashes = [h]
    for i in range(k, n):
        h = ((h - ids[i - k] * high) * BASE + ids[i]) % MOD
        hashes.append(h)

    if w <= 1:
        return set(hashes)
    picked = set()
    dq = deque()                       # indices, hashes increasing
    for i, hv in enumerate(hashes):
        while dq and hashes[dq[-1]] >= hv:
            dq.pop()
        dq.append(i)
        while dq[0] <= i - w:
            dq.popleft()
        if i >= w - 1:
            picked.add(hashes[dq[0]])
    return picked


def scan(root, k, w, normalise):
    """Fingerprint every production file under root."""
    root = os.path.abspath(root)
    per_file, corpus = {}, set()
    tokens_total = 0
    for dirpath, dirnames, filenames in os.walk(root):
        rel_dir = "/" + os.path.relpath(dirpath, root).replace(os.sep, "/") + "/"
        if is_excluded(rel_dir):
            dirnames[:] = []
            continue
        for name in sorted(filenames):
            if not name.lower().endswith(SOURCE_SUFFIXES):
                continue
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            try:
                with open(full, encoding="utf-8", errors="replace") as fh:
                    text = fh.read()
            except OSError:
                continue
            toks = tokenize(text, normalise)
            tokens_total += len(toks)
            fp = fingerprints(toks, k, w)
            if fp:
                per_file[rel] = fp
                corpus |= fp
    return per_file, corpus, tokens_total


def compare(name_a, a_files, a_corpus, name_b, b_files, b_corpus, a_tokens, b_tokens):
    shared = a_corpus & b_corpus
    per_file = []
    for rel, fp in a_files.items():
        if not fp:
            continue
        hit = len(fp & b_corpus)
        per_file.append({"file": rel, "fingerprints": len(fp),
                         "shared": hit, "share": round(100.0 * hit / len(fp), 1)})
    per_file.sort(key=lambda r: (-r["share"], -r["fingerprints"]))
    buckets = defaultdict(int)
    for r in per_file:
        if r["share"] >= 90:
            buckets[">=90%"] += 1
        elif r["share"] >= 50:
            buckets["50-90%"] += 1
        elif r["share"] >= 10:
            buckets["10-50%"] += 1
        else:
            buckets["<10%"] += 1
    return {
        "a": name_a, "b": name_b,
        "a_files": len(a_files), "b_files": len(b_files),
        "a_tokens": a_tokens, "b_tokens": b_tokens,
        "a_fingerprints": len(a_corpus), "b_fingerprints": len(b_corpus),
        "shared_fingerprints": len(shared),
        "share_of_a": round(100.0 * len(shared) / max(1, len(a_corpus)), 1),
        "share_of_b": round(100.0 * len(shared) / max(1, len(b_corpus)), 1),
        "buckets": dict(buckets),
        "most_shared": per_file[:25],
        "least_shared": [r for r in per_file if r["fingerprints"] >= 40][-25:],
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--a", required=True, help="first source tree")
    ap.add_argument("--b", required=True, help="second source tree")
    ap.add_argument("--name-a", default="a")
    ap.add_argument("--name-b", default="b")
    ap.add_argument("-k", type=int, default=20, help="k-gram size in tokens")
    ap.add_argument("-w", type=int, default=12, help="winnowing window")
    ap.add_argument("-o", "--out", default="clones.json")
    args = ap.parse_args()

    report = {"k": args.k, "w": args.w,
              "guaranteed_match_length": args.k + args.w - 1}
    for mode in ("strict", "normalised"):
        norm = mode == "normalised"
        sys.stderr.write("clone_detect: scanning %s (%s)\n" % (args.name_a, mode))
        af, ac, at = scan(args.a, args.k, args.w, norm)
        sys.stderr.write("clone_detect: scanning %s (%s)\n" % (args.name_b, mode))
        bf, bc, bt = scan(args.b, args.k, args.w, norm)
        report[mode] = compare(args.name_a, af, ac, args.name_b, bf, bc, at, bt)
        r = report[mode]
        sys.stderr.write("clone_detect: %s - %.1f%% of %s shared with %s\n"
                         % (mode, r["share_of_a"], args.name_a, args.name_b))

    with open(args.out, "w", encoding="utf-8") as fh:
        json.dump(report, fh, indent=1)
    sys.stderr.write("clone_detect: wrote %s\n" % args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
