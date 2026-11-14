# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; ADR-0009 C1. No GPL-3.0 sources.
#
# The signature policy as a user meets it: exec the real `a4l` binary on signed,
# unsigned and tampered APKs and assert on the exit code, for `a4l verify` and
# for `a4l run` under each --verify mode, a pinned signer and the environment
# seams. --vm=null keeps a VM out of it: an admitted APK exits 0 after the
# package layer, a refused one exits 3 before it.
#
# It also asserts where the bytes come from. An enforcing policy must run a
# private, verified copy rather than the path it was given, and a package name
# taken from a manifest must not steer extraction out of a4l's cache.
#
# Every run gets a throwaway XDG_CACHE_HOME, so the test never touches the
# user's own cache.
#
# Usage: run_verify_policy_test.py <a4l_binary> <apk_dir> <manifest>
import glob
import io
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "fixtures"))
from make_axml import build_axml  # noqa: E402

OK, USAGE, REFUSED = 0, 1, 3


def main() -> int:
    if len(sys.argv) != 4:
        sys.stderr.write(
            "usage: run_verify_policy_test.py <a4l> <apk_dir> <manifest>\n")
        return 2
    a4l, apk_dir, manifest = sys.argv[1:]
    signers = {}
    with open(manifest, encoding="ascii") as f:
        for line in f:
            name, _status, _scheme, certs = line.split()
            signers[name] = certs
    signed, unsigned, broken = ("ok-v3.apk", "unsigned.apk",
                                "bad-entry-byte.apk")
    pin = signers[signed]
    other = signers["ok-v2-rsa-pkcs1-sha256.apk"]
    upper_colons = ":".join(pin[i:i + 2].upper() for i in range(0, 64, 2))

    failures = []
    scratch = tempfile.mkdtemp(prefix="a4l-verify-policy-")
    cache = os.path.join(scratch, "cache")

    def expect(code, *args, env=None):
        full_env = {k: v for k, v in os.environ.items()
                    if not k.startswith("ACL_APK_")}
        full_env["XDG_CACHE_HOME"] = cache
        full_env.update(env or {})
        argv = [a4l, *args]
        argv = [os.path.join(apk_dir, a)
                if a.endswith(".apk") and not os.path.isabs(a) else a
                for a in argv]
        p = subprocess.run(argv, capture_output=True, text=True, timeout=60,
                           env=full_env)
        if p.returncode != code:
            failures.append(f"{' '.join(args)} env={env}: exit "
                            f"{p.returncode}, want {code}\n{p.stdout}{p.stderr}")
        return p.stdout + p.stderr

    # a4l verify: 0 only when the signature verifies (and matches a pin).
    out = expect(OK, "verify", signed)
    if f"signer: {pin}" not in out:
        failures.append("verify did not print the signer digest:\n" + out)
    expect(REFUSED, "verify", unsigned)
    expect(REFUSED, "verify", broken)
    expect(OK, "verify", signed, "--signer=" + pin)
    expect(REFUSED, "verify", signed, "--signer=" + other)
    expect(USAGE, "verify", signed, "--signer=nothex")

    run = ("run", "--vm=null")
    # Default (warn): a broken signature is refused, an unsigned APK runs.
    expect(OK, *run, signed)
    expect(OK, *run, unsigned)
    expect(REFUSED, *run, broken)
    # require: only a verified APK runs.
    expect(OK, *run, "--verify=require", signed)
    expect(REFUSED, *run, "--verify=require", unsigned)
    expect(REFUSED, *run, "--verify=require", broken)
    # off: nothing is checked.
    expect(OK, *run, "--verify=off", broken)
    expect(USAGE, *run, "--verify=sometimes", signed)
    # A pin admits its signer only, in any spelling, and overrides off.
    expect(OK, *run, "--signer=" + pin, signed)
    expect(OK, *run, "--signer=" + upper_colons, signed)
    expect(OK, *run, f"--signer={other},{pin}", signed)
    expect(REFUSED, *run, "--signer=" + other, signed)
    expect(REFUSED, *run, "--signer=" + pin, unsigned)
    expect(REFUSED, *run, "--verify=off", "--signer=" + other, signed)
    expect(USAGE, *run, "--signer=00", signed)
    # The environment seams, and the flag winning over them.
    expect(REFUSED, *run, unsigned, env={"ACL_APK_VERIFY": "require"})
    expect(OK, *run, "--verify=warn", unsigned,
           env={"ACL_APK_VERIFY": "require"})
    expect(USAGE, *run, signed, env={"ACL_APK_VERIFY": "maybe"})
    expect(REFUSED, *run, signed, env={"ACL_APK_SIGNER": other})
    expect(OK, *run, "--signer=" + pin, signed, env={"ACL_APK_SIGNER": other})
    expect(USAGE, *run, signed, env={"ACL_APK_SIGNER": "zz"})

    # --- an enforcing policy runs a private copy, not the path it was given ---
    def read(path):
        with open(path, "rb") as f:
            return f.read()

    def check(ok, what):
        if not ok:
            failures.append(what)

    shutil.rmtree(cache, ignore_errors=True)
    src = os.path.join(scratch, "app.apk")
    shutil.copy(os.path.join(apk_dir, "ok-v2-eocd-comment.apk"), src)
    original = read(src)
    out = expect(OK, *run, "--verify=require", src)
    copies = glob.glob(os.path.join(cache, "acl", "verified", "*", "base.apk"))
    check(len(copies) == 1, f"want one verified copy, found {copies}")
    if len(copies) == 1:
        copy = copies[0]
        check(f"running the verified copy {copy} (copied)" in out,
              "first enforced run did not report a fresh copy:\n" + out)
        check(read(copy) == original, "verified copy differs from the source")
        check(os.stat(copy).st_mode & 0o777 == 0o400, "copy is not mode 0400")
        lib = os.path.join(os.path.dirname(copy), "lib", "libfoo.so")
        check(os.path.exists(lib), "native lib not extracted beside the copy")
        check(f"-> {os.path.dirname(lib)}" in out,
              "libraries were not loaded from beside the copy:\n" + out)

        # The source is rewritten after it was verified once. Its signing block
        # is intact, so it maps to the same copy; the copy runs and the changed
        # bytes are never read.
        with open(src, "r+b") as f:
            f.seek(40)
            f.write(bytes([original[40] ^ 0x01]))
        out = expect(OK, *run, "--verify=require", src)
        check("(reused)" in out, "second enforced run did not reuse:\n" + out)
        check(read(copy) == original, "copy changed after the source did")
        # Damage to the copy itself is caught too: with the source also bad,
        # there is nothing left that verifies.
        os.chmod(copy, 0o600)
        with open(copy, "r+b") as f:
            f.seek(41)
            f.write(b"\xff")
        expect(REFUSED, *run, "--verify=require", src)
        check(not glob.glob(os.path.join(cache, "acl", "verified", "*")),
              "a copy that does not verify was left in the cache")
    # The reporting modes leave the path alone and make no copy.
    shutil.rmtree(cache, ignore_errors=True)
    out = expect(OK, *run, signed)
    check("verified copy" not in out and
          not os.path.exists(os.path.join(cache, "acl", "verified")),
          "--verify=warn made a copy:\n" + out)

    # --- a manifest's package name cannot steer extraction out of the cache ---
    shutil.rmtree(cache, ignore_errors=True)
    for hostile in ["../../../escaped", "/" + os.path.join(scratch, "abs"),
                    "com.example/../../../../escaped2"]:
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w") as z:
            z.writestr("AndroidManifest.xml", build_axml(package=hostile))
            z.writestr("classes.dex", b"dex\n035\x00" + bytes(64))
            z.writestr("lib/x86_64/libevil.so", b"\x7fELF")
            z.writestr("lib/arm64-v8a/libevil.so", b"\x7fELF")
        hostile_apk = os.path.join(scratch, "hostile.apk")
        with open(hostile_apk, "wb") as f:
            f.write(buf.getvalue())
        expect(OK, *run, hostile_apk)
        landed = [os.path.relpath(p, scratch)
                  for p in glob.glob(os.path.join(scratch, "**", "libevil.so"),
                                     recursive=True)]
        check(landed == [os.path.join("cache", "acl", "app", "lib",
                                      "libevil.so")],
              f"package {hostile!r} put a library at {landed}")
    shutil.rmtree(scratch, ignore_errors=True)

    if failures:
        sys.stderr.write("\n".join("FAIL: " + f for f in failures) + "\n")
        return 1
    sys.stdout.write("OK: verify and run policy exit codes all as specified\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
