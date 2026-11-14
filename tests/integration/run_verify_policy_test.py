# SPDX-License-Identifier: Apache-2.0
# Provenance: original test harness; ADR-0009 C1. No GPL-3.0 sources.
#
# The signature policy as a user meets it: exec the real `a4l` binary on signed,
# unsigned and tampered APKs and assert on the exit code, for `a4l verify` and
# for `a4l run` under each --verify mode, a pinned signer and the environment
# seams. --vm=null keeps a VM out of it: an admitted APK exits 0 after the
# package layer, a refused one exits 3 before it.
#
# Usage: run_verify_policy_test.py <a4l_binary> <apk_dir> <manifest>
import os
import subprocess
import sys

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

    def expect(code, *args, env=None):
        full_env = {k: v for k, v in os.environ.items()
                    if not k.startswith("ACL_APK_")}
        full_env.update(env or {})
        argv = [a4l, *args]
        argv = [os.path.join(apk_dir, a) if a.endswith(".apk") else a
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

    if failures:
        sys.stderr.write("\n".join("FAIL: " + f for f in failures) + "\n")
        return 1
    sys.stdout.write("OK: verify and run policy exit codes all as specified\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
