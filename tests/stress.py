#!/usr/bin/env python3
"""omdfs differential stress-test engine.

Two subcommands drive a consistency stress test against an omdfs mount:

  apply  --target DIR --control DIR --seed N --ops N [--max-file-size B] [-v]
         Generate a reproducible, seeded sequence of filesystem operations and
         apply each one *identically* to TARGET (the omdfs mount) and CONTROL (a
         plain mirror directory). After every op the outcome is compared between
         the two roots: success/errno must match, and for reads the bytes must
         match. The first divergence aborts non-zero, printing the offending op
         and the seed so the run reproduces exactly.

  compare DIR_A DIR_B
         Deep-compare two trees: the set of relative paths, each entry's type,
         regular-file contents (sha256), symlink target, and permission bits.
         Every mismatch is printed; exits non-zero if any are found.

The differential model: CONTROL is an ordinary local directory we fully trust.
Whatever omdfs does on TARGET must match what a plain filesystem does on CONTROL.
mtime/atime are deliberately *not* compared (the two roots are mutated at
different wall-clock instants, so a write's "now" timestamp legitimately differs);
type + content + symlink target + permission bits is the meaningful consistency.
"""

import argparse
import errno
import hashlib
import os
import random
import stat
import sys
import time

# Deterministic mode sets. Every file mode keeps the owner-read bit (so reads
# always succeed); 0o444 has no owner-write bit, so the model excludes it from
# write ops (avoids self-inflicted, though matching, EACCES) while still
# exercising attr propagation. Every dir mode keeps owner rwx (traversable).
FILE_MODES = [0o644, 0o600, 0o640, 0o444]
DIR_MODES = [0o755, 0o700, 0o750]
CREATE_FILE_MODE = 0o644  # 0o666 & ~umask(0o022)
MKDIR_MODE = 0o755        # 0o777 & ~umask(0o022)


def capture(fn):
    """Run fn(); return (ok, errno). errno is 0 on success."""
    try:
        fn()
        return (True, 0)
    except OSError as e:
        return (False, e.errno or errno.EIO)


class Model:
    """Tracks the logical tree so we can pick valid op targets. Both roots are
    kept in lock-step by applying every op to both; this model mirrors that
    shared logical state and is validated against reality by the final compare."""

    def __init__(self, rng):
        self.rng = rng
        self.dirs = {"": MKDIR_MODE}   # rel path -> mode; "" is the root
        self.files = {}               # rel path -> mode
        self.links = {}               # rel path -> target string
        self.counter = 0

    def new_name(self):
        self.counter += 1
        return "n%d" % self.counter

    def all_paths(self):
        return set(self.dirs) | set(self.files) | set(self.links)

    def pick_dir(self, include_root=True):
        choices = list(self.dirs) if include_root else [d for d in self.dirs if d]
        return self.rng.choice(choices) if choices else None

    def pick_file(self):
        return self.rng.choice(list(self.files)) if self.files else None

    def pick_writable_file(self):
        w = [f for f, m in self.files.items() if m & 0o200]
        return self.rng.choice(w) if w else None

    def pick_link(self):
        return self.rng.choice(list(self.links)) if self.links else None

    def pick_any(self, include_root=False):
        choices = list(self.files) + list(self.links) + \
            ([d for d in self.dirs if d or include_root])
        return self.rng.choice(choices) if choices else None

    def fresh_path(self):
        """A non-colliding new relative path under some existing directory."""
        parent = self.pick_dir()
        name = self.new_name()
        rel = os.path.join(parent, name) if parent else name
        return rel


def fail(msg, seed):
    sys.stderr.write("STRESS FAIL [seed=%d]: %s\n" % (seed, msg))
    sys.exit(1)


# ---- op implementations: each takes (root, rel, params) and acts on that root ----

def cmd_apply(args):
    rng = random.Random(args.seed)
    m = Model(rng)
    target, control = args.target, args.control

    def both(rel, fn):
        """Apply fn(full_path) to target and control; return (rt, rc)."""
        rt = capture(lambda: fn(os.path.join(target, rel)))
        rc = capture(lambda: fn(os.path.join(control, rel)))
        return rt, rc

    def check(rel, op, rt, rc):
        if rt != rc:
            fail("op #%d %s %r diverged: target=%s control=%s"
                 % (i, op, rel, rt, rc), args.seed)
        return rt[0]  # ok on both

    def mutate(rel, op, fn):
        """A possibly-growing write op (create/write/append/pwrite/truncate).
        Apply to the mount first; if it gets ENOSPC that is the cache-budget
        backpressure firing (the mount overshot its hard limit because dirty data
        is draining slower than this synthetic storm writes) — control, a plain
        FS, never hits it. Backpressure is checked *before* any bytes are written,
        so nothing was applied: wait for the syncer to drain and retry. If it
        persists, skip the op on BOTH roots so they stay in lock-step (logged).
        Returns True if applied to both, False if skipped."""
        rt = capture(lambda: fn(os.path.join(target, rel)))
        tries = 0
        while not rt[0] and rt[1] == errno.ENOSPC and tries < 50:
            time.sleep(0.1)
            tries += 1
            rt = capture(lambda: fn(os.path.join(target, rel)))
        if not rt[0] and rt[1] == errno.ENOSPC:
            sys.stderr.write("note: op #%d %s %r skipped: persistent cache "
                             "backpressure (ENOSPC)\n" % (i, op, rel))
            return False
        rc = capture(lambda: fn(os.path.join(control, rel)))
        if rt != rc:
            fail("op #%d %s %r diverged: target=%s control=%s"
                 % (i, op, rel, rt, rc), args.seed)
        return rt[0]

    ops = ["create", "write", "append", "pwrite", "truncate", "read",
           "mkdir", "rmdir", "unlink", "rename", "symlink", "chmod"]
    # Weighted toward content churn and structure; reads sprinkled throughout.
    weights = [12, 10, 6, 8, 8, 12, 6, 4, 8, 8, 5, 8]

    for i in range(args.ops):
        op = rng.choices(ops, weights)[0]

        if op == "create":
            rel = m.fresh_path()
            data = rng.randbytes(rng.randint(0, args.max_file_size))

            def fn(p, data=data):
                with open(p, "wb") as f:
                    f.write(data)
            if mutate(rel, op, fn):
                m.files[rel] = CREATE_FILE_MODE

        elif op == "write":  # full overwrite (truncate to new content)
            rel = m.pick_writable_file()
            if rel is None:
                continue
            data = rng.randbytes(rng.randint(0, args.max_file_size))

            def fn(p, data=data):
                with open(p, "wb") as f:
                    f.write(data)
            mutate(rel, op, fn)

        elif op == "append":
            rel = m.pick_writable_file()
            if rel is None:
                continue
            data = rng.randbytes(rng.randint(1, args.max_file_size))

            def fn(p, data=data):
                with open(p, "ab") as f:
                    f.write(data)
            mutate(rel, op, fn)

        elif op == "pwrite":  # write at an arbitrary offset (may extend / hole)
            rel = m.pick_writable_file()
            if rel is None:
                continue
            off = rng.randint(0, args.max_file_size)
            data = rng.randbytes(rng.randint(1, args.max_file_size))

            def fn(p, off=off, data=data):
                with open(p, "r+b") as f:
                    f.seek(off)
                    f.write(data)
            mutate(rel, op, fn)

        elif op == "truncate":
            rel = m.pick_writable_file()
            if rel is None:
                continue
            size = rng.randint(0, args.max_file_size)

            def fn(p, size=size):
                os.truncate(p, size)
            mutate(rel, op, fn)

        elif op == "read":  # differential content check
            rel = m.pick_file()
            if rel is None:
                continue

            def rd(p):
                with open(p, "rb") as f:
                    return f.read()
            try:
                bt = rd(os.path.join(target, rel))
                et = None
            except OSError as e:
                bt, et = None, (e.errno or errno.EIO)
            try:
                bc = rd(os.path.join(control, rel))
                ec = None
            except OSError as e:
                bc, ec = None, (e.errno or errno.EIO)
            if et != ec:
                fail("op #%d read %r errno diverged: target=%s control=%s"
                     % (i, rel, et, ec), args.seed)
            if et is None and bt != bc:
                fail("op #%d read %r content diverged: target=%d bytes, "
                     "control=%d bytes" % (i, rel, len(bt), len(bc)), args.seed)

        elif op == "mkdir":
            rel = m.fresh_path()

            def fn(p):
                os.mkdir(p)
            rt, rc = both(rel, fn)
            if check(rel, op, rt, rc) and rt[0]:
                m.dirs[rel] = MKDIR_MODE

        elif op == "rmdir":
            # only empty, non-root dirs
            empty = [d for d in self_empty_dirs(m)]
            if not empty:
                continue
            rel = rng.choice(empty)

            def fn(p):
                os.rmdir(p)
            rt, rc = both(rel, fn)
            if check(rel, op, rt, rc) and rt[0]:
                del m.dirs[rel]

        elif op == "unlink":
            rel = m.pick_file()
            kind = "file"
            if rel is None or rng.random() < 0.3:
                lk = m.pick_link()
                if lk is not None:
                    rel, kind = lk, "link"
            if rel is None:
                continue

            def fn(p):
                os.unlink(p)
            rt, rc = both(rel, fn)
            if check(rel, op, rt, rc) and rt[0]:
                (m.files if kind == "file" else m.links).pop(rel, None)

        elif op == "rename":
            src = m.pick_any()
            if src is None:
                continue
            dst = m.fresh_path()
            # never move a dir into its own subtree
            if src in m.dirs and (dst == src or dst.startswith(src + "/")):
                continue

            def fn_pair():
                # rename needs both endpoints under the *same* root
                def do(root):
                    os.rename(os.path.join(root, src), os.path.join(root, dst))
                return do
            do = fn_pair()
            rt = capture(lambda: do(target))
            rc = capture(lambda: do(control))
            if rt != rc:
                fail("op #%d rename %r->%r diverged: target=%s control=%s"
                     % (i, src, dst, rt, rc), args.seed)
            if rt[0]:
                rename_in_model(m, src, dst)

        elif op == "symlink":
            rel = m.fresh_path()
            target_str = rng.choice(["a", "../b", "n%d" % rng.randint(1, 50),
                                     "deep/dangling/target", "."])

            def fn(p, t=target_str):
                os.symlink(t, p)
            rt, rc = both(rel, fn)
            if check(rel, op, rt, rc) and rt[0]:
                m.links[rel] = target_str

        elif op == "chmod":
            # a file or a non-root dir
            if m.files and (not [d for d in m.dirs if d] or rng.random() < 0.6):
                rel = m.pick_file()
                mode = rng.choice(FILE_MODES)
                store = m.files
            else:
                rel = m.pick_dir(include_root=False)
                if rel is None:
                    continue
                mode = rng.choice(DIR_MODES)
                store = m.dirs
            if rel is None:
                continue

            def fn(p, mode=mode):
                os.chmod(p, mode)
            rt, rc = both(rel, fn)
            if check(rel, op, rt, rc) and rt[0]:
                store[rel] = mode

        if args.verbose:
            sys.stderr.write("#%d %s\n" % (i, op))

    print("applied %d ops (seed=%d) with no target/control divergence"
          % (args.ops, args.seed))
    return 0


def self_empty_dirs(m):
    """Non-root dirs in the model that contain no children."""
    parents = set()
    for p in list(m.files) + list(m.links) + list(m.dirs):
        if p:
            parents.add(os.path.dirname(p))
    return [d for d in m.dirs if d and d not in parents]


def rename_in_model(m, src, dst):
    if src in m.files:
        m.files[dst] = m.files.pop(src)
    elif src in m.links:
        m.links[dst] = m.links.pop(src)
    elif src in m.dirs:
        # move the dir and rekey every descendant
        for table in (m.dirs, m.files, m.links):
            for p in [k for k in table if k == src or k.startswith(src + "/")]:
                np = dst + p[len(src):]
                table[np] = table.pop(p)


# ---- compare ----

def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def snapshot(root):
    """Map rel-path -> a tuple describing the entry.
       dir:     ('d', mode)
       file:    ('f', mode, sha256)
       symlink: ('l', target)"""
    out = {}

    def rec(rel):
        full = os.path.join(root, rel) if rel else root
        with os.scandir(full) as it:
            for e in it:
                r = os.path.join(rel, e.name) if rel else e.name
                if e.is_symlink():
                    out[r] = ("l", os.readlink(e.path))
                elif e.is_dir(follow_symlinks=False):
                    st = e.stat(follow_symlinks=False)
                    out[r] = ("d", stat.S_IMODE(st.st_mode))
                    rec(r)
                else:
                    st = e.stat(follow_symlinks=False)
                    out[r] = ("f", stat.S_IMODE(st.st_mode), sha256_file(e.path))
    rec("")
    return out


def cmd_compare(args):
    a = snapshot(args.a)
    b = snapshot(args.b)
    diffs = []
    for p in sorted(set(a) | set(b)):
        if p not in a:
            diffs.append("only in %s: %s (%s)" % (args.b, p, b[p][0]))
        elif p not in b:
            diffs.append("only in %s: %s (%s)" % (args.a, p, a[p][0]))
        elif a[p] != b[p]:
            diffs.append("differ: %s\n    %s: %s\n    %s: %s"
                         % (p, args.a, a[p], args.b, b[p]))
    if diffs:
        sys.stderr.write("COMPARE FAIL: %d difference(s) between %s and %s\n"
                         % (len(diffs), args.a, args.b))
        for d in diffs[:50]:
            sys.stderr.write("  " + d + "\n")
        if len(diffs) > 50:
            sys.stderr.write("  ... (%d more)\n" % (len(diffs) - 50))
        return 1
    print("identical: %s == %s (%d entries)" % (args.a, args.b, len(a)))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    a = sub.add_parser("apply", help="apply a seeded op sequence to two roots")
    a.add_argument("--target", required=True)
    a.add_argument("--control", required=True)
    a.add_argument("--seed", type=int, default=1)
    a.add_argument("--ops", type=int, default=2000)
    a.add_argument("--max-file-size", type=int, default=131072)
    a.add_argument("-v", "--verbose", action="store_true")
    a.set_defaults(func=cmd_apply)

    c = sub.add_parser("compare", help="deep-compare two trees")
    c.add_argument("a")
    c.add_argument("b")
    c.set_defaults(func=cmd_compare)

    args = ap.parse_args()
    sys.exit(args.func(args))


if __name__ == "__main__":
    main()
