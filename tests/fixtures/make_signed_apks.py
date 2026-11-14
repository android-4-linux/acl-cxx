# SPDX-License-Identifier: Apache-2.0
# Provenance: original test fixture generator. The APK Signing Block layout and
# the chunked content digest follow AOSP's published "APK Signature Scheme
# v2/v3" description; this is an independent second implementation of them, so
# the C++ verifier is not only tested against itself. No GPL-3.0 sources.
#
# Emit signed, mis-signed and tampered APKs for pkg/apk_verify tests.
# Usage: make_signed_apks.py <outdir> <manifest.txt>
#
# Keys are generated fresh on every run with the `openssl` command; nothing
# secret or binary is committed. The manifest has one line per APK:
#   <file> <status> <scheme> <signer-cert-sha256,...|->
import hashlib
import io
import os
import random
import struct
import subprocess
import sys
import tempfile
import zipfile

MAGIC = b"APK Sig Block 42"
V2, V3, V31 = 0x7109871A, 0xF05368C0, 0x1B93AD61
PADDING_ID = 0x42726577
ATTR_STRIP, ATTR_ROT_MIN_SDK = 0xBEEFF00D, 0x559F8B02
SDK_MAX = 0x7FFFFFFF
CHUNK = 1024 * 1024

# id -> (hashlib name, extra `openssl dgst` options)
ALGS = {
    0x0101: ("sha256", ["-sigopt", "rsa_padding_mode:pss", "-sigopt",
                        "rsa_pss_saltlen:32", "-sigopt", "rsa_mgf1_md:sha256"]),
    0x0102: ("sha512", ["-sigopt", "rsa_padding_mode:pss", "-sigopt",
                        "rsa_pss_saltlen:64", "-sigopt", "rsa_mgf1_md:sha512"]),
    0x0103: ("sha256", []),
    0x0104: ("sha512", []),
    0x0201: ("sha256", []),
    0x0202: ("sha512", []),
    0x0301: ("sha256", []),
    0x0421: ("sha256", []),  # fs-verity ID: signed here, never verifiable
}


def run(*argv, stdin=None):
    return subprocess.run(argv, input=stdin, check=True,
                          capture_output=True).stdout


class Key:
    """A private key, its self-signed certificate and its SubjectPublicKeyInfo."""

    def __init__(self, work, name, genpkey_args):
        self.pem = os.path.join(work, name + ".pem")
        run("openssl", "genpkey", *genpkey_args, "-out", self.pem)
        self.cert = run("openssl", "req", "-x509", "-new", "-key", self.pem,
                        "-subj", "/CN=a4l-test-" + name, "-days", "3650",
                        "-sha256", "-outform", "DER")
        self.spki = run("openssl", "pkey", "-in", self.pem, "-pubout",
                        "-outform", "DER")
        self.cert_sha256 = hashlib.sha256(self.cert).hexdigest()

    def sign(self, alg, data, opts=None):
        name, default_opts = ALGS[alg]
        opts = default_opts if opts is None else opts
        return run("openssl", "dgst", "-" + name, "-sign", self.pem, *opts,
                   stdin=data)


def u32(v):
    return struct.pack("<I", v)


def lp(b):
    """A uint32 length prefix followed by the bytes."""
    return u32(len(b)) + b


def base_zip(big=False, comment=b""):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        entries = [
            ("AndroidManifest.xml", b"<manifest/>", zipfile.ZIP_STORED),
            ("classes.dex", b"dex\n035\x00" + bytes(64), zipfile.ZIP_DEFLATED),
            ("lib/x86_64/libfoo.so", b"\x7fELF-x86_64", zipfile.ZIP_STORED),
        ]
        if big:  # > 2 chunks of entry data, so the chunk loop is exercised
            entries.append(("assets/blob.bin",
                            random.Random(42).randbytes(2 * CHUNK + 4321),
                            zipfile.ZIP_STORED))
        for name, data, method in entries:
            info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = method
            z.writestr(info, data)
        z.comment = comment
    return buf.getvalue()


def split_zip(z):
    """(bytes before the central directory, central directory, EOCD)."""
    eocd_off = z.rfind(b"PK\x05\x06")
    cd_size, cd_off = struct.unpack_from("<II", z, eocd_off + 12)
    assert cd_off + cd_size == eocd_off
    return z[:cd_off], z[cd_off:eocd_off], z[eocd_off:]


def content_digest(z, hash_name):
    """Digest of the three sections as they are before a block is inserted, when
    the EOCD's central-directory offset already equals the block's offset."""
    chunks = []
    for section in split_zip(z):
        for off in range(0, len(section), CHUNK):
            c = section[off:off + CHUNK]
            chunks.append(hashlib.new(hash_name, b"\xa5" + u32(len(c)) + c)
                          .digest())
    return hashlib.new(hash_name,
                       b"\x5a" + u32(len(chunks)) + b"".join(chunks)).digest()


def signer(z, key, algs, v3=None, attrs=(), *, digest_patch=None,
           cert=None, sig_patch=False, digest_algs=None, unsigned_sdk=None,
           sign_opts=None):
    """One signer record. `v3` is (min_sdk, max_sdk) or None for v2. The keyword
    arguments each break exactly one rule."""
    digests = b""
    for a in (digest_algs or algs):
        d = content_digest(z, ALGS[a][0])
        if a == 0x0421:
            d += bytes(8)  # verity digests carry a trailing length
        if digest_patch:
            d = bytes([d[0] ^ 0xFF]) + d[1:]
        digests += lp(u32(a) + lp(d))
    signed = lp(digests) + lp(lp(cert or key.cert))
    if v3:
        signed += u32(v3[0]) + u32(v3[1])
    signed += lp(b"".join(lp(u32(i) + v) for i, v in attrs))
    sigs = b""
    for a in algs:
        s = key.sign(a, signed, sign_opts)
        if sig_patch:
            s = s[:-1] + bytes([s[-1] ^ 0x01])
        sigs += lp(u32(a) + lp(s))
    out = lp(signed)
    if v3:
        lo, hi = unsigned_sdk or v3
        out += u32(lo) + u32(hi)
    return out + lp(sigs) + lp(key.spki)


def block_value(*signers):
    return lp(b"".join(lp(s) for s in signers))


def with_block(z, pairs, *, footer_size_delta=0):
    """Insert a signing block holding `pairs` [(id, value)] before the central
    directory and point the EOCD at the moved directory."""
    before, cd, eocd = split_zip(z)
    body = b"".join(struct.pack("<QI", len(v) + 4, i) + v for i, v in pairs)
    size = len(body) + 24
    block = (struct.pack("<Q", size) + body +
             struct.pack("<Q", size + footer_size_delta) + MAGIC)
    eocd = eocd[:16] + u32(len(before) + len(block)) + eocd[20:]
    return before + block + cd + eocd


def flip(b, off):
    return b[:off] + bytes([b[off] ^ 0x01]) + b[off + 1:]


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write("usage: make_signed_apks.py <outdir> <manifest>\n")
        return 2
    outdir, manifest = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    work = tempfile.mkdtemp(prefix="a4l-keys-")
    rsa = Key(work, "rsa", ["-algorithm", "RSA", "-pkeyopt",
                            "rsa_keygen_bits:2048"])
    ec = Key(work, "ec", ["-algorithm", "EC", "-pkeyopt",
                          "ec_paramgen_curve:P-256"])
    ec2 = Key(work, "ec2", ["-algorithm", "EC", "-pkeyopt",
                            "ec_paramgen_curve:P-256"])
    ec384 = Key(work, "ec384", ["-algorithm", "EC", "-pkeyopt",
                                "ec_paramgen_curve:P-384"])
    dsa_params = os.path.join(work, "dsa-params.pem")
    run("openssl", "genpkey", "-genparam", "-algorithm", "DSA", "-pkeyopt",
        "dsa_paramgen_bits:2048", "-pkeyopt", "dsa_paramgen_q_bits:256",
        "-out", dsa_params)
    dsa = Key(work, "dsa", ["-paramfile", dsa_params])

    z = base_zip()
    big = base_zip(big=True)
    commented = base_zip(comment=b"a comment the signature must cover")
    lines = []

    def emit(name, data, status, scheme="none", keys=()):
        with open(os.path.join(outdir, name + ".apk"), "wb") as f:
            f.write(data)
        certs = ",".join(k.cert_sha256 for k in keys) or "-"
        lines.append(f"{name}.apk {status} {scheme} {certs}")

    def v2(zz, *signers):
        return with_block(zz, [(V2, block_value(*signers))])

    # --- every algorithm, v2 ------------------------------------------------
    for label, key, alg in [
        ("rsa-pss-sha256", rsa, 0x0101), ("rsa-pss-sha512", rsa, 0x0102),
        ("rsa-pkcs1-sha256", rsa, 0x0103), ("rsa-pkcs1-sha512", rsa, 0x0104),
        ("ec-sha256", ec, 0x0201), ("ec-sha512", ec384, 0x0202),
        ("dsa-sha256", dsa, 0x0301),
    ]:
        emit("ok-v2-" + label, v2(z, signer(z, key, [alg])), "verified", "v2",
             [key])

    # --- shapes that must verify --------------------------------------------
    emit("ok-v2-two-signers",
         v2(z, signer(z, rsa, [0x0103]), signer(z, ec, [0x0201])),
         "verified", "v2", [rsa, ec])
    emit("ok-v2-two-algorithms", v2(z, signer(z, rsa, [0x0103, 0x0104])),
         "verified", "v2", [rsa])
    emit("ok-v2-verity-and-sha256", v2(z, signer(z, rsa, [0x0421, 0x0103])),
         "verified", "v2", [rsa])
    emit("ok-v2-multi-chunk", v2(big, signer(big, ec, [0x0201])), "verified",
         "v2", [ec])
    emit("ok-v2-eocd-comment", v2(commented, signer(commented, ec, [0x0201])),
         "verified", "v2", [ec])
    emit("ok-v2-padded-block",
         with_block(z, [(0x12345678, b"unknown pair"),
                        (V2, block_value(signer(z, ec, [0x0201]))),
                        (PADDING_ID, bytes(1000))]),
         "verified", "v2", [ec])
    v3_all = signer(z, ec, [0x0201], v3=(24, SDK_MAX))
    emit("ok-v3", with_block(z, [(V3, block_value(v3_all))]), "verified", "v3",
         [ec])
    v2_declares_v3 = signer(z, ec, [0x0201], attrs=[(ATTR_STRIP, u32(3))])
    emit("ok-v2-and-v3",
         with_block(z, [(V2, block_value(v2_declares_v3)),
                        (V3, block_value(v3_all))]),
         "verified", "v3", [ec])
    # Rotation: the old key signs v3 for every SDK and names the SDK from which
    # a v3.1 signer takes over; the new key signs v3.1 from that SDK.
    def rotated(min_sdk):
        old = signer(z, ec, [0x0201], v3=(24, SDK_MAX),
                     attrs=[(ATTR_ROT_MIN_SDK, u32(min_sdk))])
        new = signer(z, ec2, [0x0201], v3=(min_sdk, SDK_MAX))
        return old, new
    old33, new33 = rotated(33)
    emit("ok-v31-rotated",
         with_block(z, [(V3, block_value(old33)), (V31, block_value(new33))]),
         "verified", "v3.1", [ec2])
    old34, new34 = rotated(34)
    emit("ok-v31-for-a-later-platform",
         with_block(z, [(V3, block_value(old34)), (V31, block_value(new34))]),
         "verified", "v3", [ec])

    # --- not signed, or not checkable ---------------------------------------
    emit("unsigned", z, "unsigned")
    emit("unsigned-block-without-signature",
         with_block(z, [(PADDING_ID, bytes(64))]), "unsigned")
    emit("unsupported-verity-only", v2(z, signer(z, rsa, [0x0421])),
         "unsupported")

    # --- tampering after signing --------------------------------------------
    good = v2(commented, signer(commented, ec, [0x0201]))
    before, cd, eocd = split_zip(commented)
    block_len = len(good) - len(commented)
    emit("bad-entry-byte", flip(good, 40), "invalid")
    emit("bad-central-directory-byte",
         flip(good, len(before) + block_len + 38), "invalid")  # external attrs
    emit("bad-eocd-comment-byte", flip(good, len(good) - 1), "invalid")
    emit("bad-signed-data-byte",
         flip(good, len(before) + 8 + 12 + 4 + 4 + 8), "invalid")
    emit("bad-trailing-data", good + b"appended", "invalid")

    # --- signatures that break exactly one rule -----------------------------
    emit("bad-signature", v2(z, signer(z, ec, [0x0201], sig_patch=True)),
         "invalid")
    emit("bad-digest-properly-signed",
         v2(z, signer(z, ec, [0x0201], digest_patch=True)), "invalid")
    emit("bad-key-does-not-match-certificate",
         v2(z, signer(z, ec2, [0x0201], cert=ec.cert)), "invalid")
    emit("bad-pss-salt-length",  # a valid PSS signature, salt 20 instead of 32
         v2(z, signer(z, rsa, [0x0101], sign_opts=[
             "-sigopt", "rsa_padding_mode:pss", "-sigopt",
             "rsa_pss_saltlen:20", "-sigopt", "rsa_mgf1_md:sha256"])),
         "invalid")
    emit("bad-key-type-for-algorithm", v2(z, signer(z, rsa, [0x0201])),
         "invalid")
    emit("bad-algorithm-lists-differ",
         v2(z, signer(z, rsa, [0x0103], digest_algs=[0x0104])), "invalid")
    emit("bad-signers-disagree",
         v2(z, signer(z, rsa, [0x0103]),
            signer(z, ec, [0x0201], digest_patch=True)), "invalid")
    emit("bad-v3-stripped", v2(z, v2_declares_v3), "invalid")
    emit("bad-v31-stripped", with_block(z, [(V3, block_value(old33))]),
         "invalid")
    emit("bad-v3-sdk-range-differs",
         with_block(z, [(V3, block_value(
             signer(z, ec, [0x0201], v3=(24, SDK_MAX),
                    unsigned_sdk=(1, SDK_MAX))))]), "invalid")
    emit("bad-v3-not-for-this-platform",
         with_block(z, [(V3, block_value(
             signer(z, ec, [0x0201], v3=(34, SDK_MAX))))]), "invalid")
    emit("bad-v3-two-signers",
         with_block(z, [(V3, block_value(v3_all, signer(
             z, ec2, [0x0201], v3=(24, SDK_MAX))))]), "invalid")
    emit("bad-v3-falls-back-to-nothing",  # broken v3 must not fall back to v2
         with_block(z, [(V2, block_value(signer(z, ec, [0x0201]))),
                        (V3, block_value(signer(z, ec, [0x0201],
                                                v3=(24, SDK_MAX),
                                                sig_patch=True)))]),
         "invalid")
    emit("bad-block-sizes-differ",
         with_block(z, [(V2, block_value(signer(z, ec, [0x0201])))],
                    footer_size_delta=8), "invalid")
    emit("bad-signer-length-overruns",
         with_block(z, [(V2, u32(0xFFFFFF) + bytes(32))]), "invalid")
    emit("bad-empty-signer-list", with_block(z, [(V2, lp(b""))]), "invalid")

    with open(manifest, "w", encoding="ascii") as f:
        f.write("\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
