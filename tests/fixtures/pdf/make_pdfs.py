"""Writes the PDF fixtures with the standard library only.

  python -I tests/fixtures/pdf/make_pdfs.py                       two-pages.pdf, landscape.pdf, encrypted.pdf next to this script
  python -I tests/fixtures/pdf/make_pdfs.py --pages N OUT.pdf     one N-page portrait document (for memory checks)

encrypted.pdf: Standard security handler, V1/R2, RC4-40, user password "x", empty owner password (= user password).
"""
import hashlib
import os
import struct
import sys

PAD = bytes.fromhex("28BF4E5E4E758A4164004E56FFFA01082E2E00B6D0683E802F0CA9FE6453697A")


def rc4(key: bytes, data: bytes) -> bytes:
    s = list(range(256))
    j = 0
    for i in range(256):
        j = (j + s[i] + key[i % len(key)]) & 255
        s[i], s[j] = s[j], s[i]
    out = bytearray()
    i = j = 0
    for byte in data:
        i = (i + 1) & 255
        j = (j + s[i]) & 255
        s[i], s[j] = s[j], s[i]
        out.append(byte ^ s[(s[i] + s[j]) & 255])
    return bytes(out)


def page_content(title: str, width: int, height: int) -> bytes:
    lines = [
        "BT /F1 36 Tf 48 %d Td (%s) Tj ET" % (height - 96, title),
        "BT /F1 18 Tf 48 %d Td (The quick brown fox jumps over the lazy dog.) Tj ET" % (height - 140),
        "BT /F1 18 Tf 48 %d Td (0123456789 FastLook PDF fixture) Tj ET" % (height - 170),
        "0.2 0.4 0.8 rg 48 %d %d 40 re f" % (height - 260, width - 96),
        "0.9 0.3 0.2 RG 3 w 48 48 %d %d re S" % (width - 96, height - 360),
    ]
    return "\n".join(lines).encode("latin-1")


def build(pages, password=None) -> bytes:
    """pages: list of (title, width, height)."""
    file_id = hashlib.md5(b"fastlook-pdf-fixture").digest()
    key = None
    enc_dict = b""
    if password is not None:
        pw = (password.encode("latin-1") + PAD)[:32]
        o = rc4(hashlib.md5(pw).digest()[:5], pw)  # owner password = user password
        p = -4
        key = hashlib.md5(pw + o + struct.pack("<i", p) + file_id).digest()[:5]
        u = rc4(key, PAD)
        enc_dict = b"<< /Filter /Standard /V 1 /R 2 /O <%s> /U <%s> /P %d >>" % (o.hex().encode(), u.hex().encode(), p)

    n = len(pages)
    # 1 catalog, 2 pages, 3 font, then per page: page object + content stream, last: encrypt dict
    objects = {}
    objects[1] = b"<< /Type /Catalog /Pages 2 0 R >>"
    kids = " ".join("%d 0 R" % (4 + 2 * i) for i in range(n))
    objects[2] = ("<< /Type /Pages /Count %d /Kids [%s] >>" % (n, kids)).encode()
    objects[3] = b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"
    streams = {}
    for i, (title, w, h) in enumerate(pages):
        page_no, content_no = 4 + 2 * i, 5 + 2 * i
        objects[page_no] = (
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] /Resources << /Font << /F1 3 0 R >> >> /Contents %d 0 R >>"
            % (w, h, content_no)
        ).encode()
        streams[content_no] = page_content(title, w, h)
    encrypt_no = 4 + 2 * n
    if enc_dict:
        objects[encrypt_no] = enc_dict

    out = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
    offsets = {}
    for num in sorted(set(objects) | set(streams)):
        offsets[num] = len(out)
        out += b"%d 0 obj\n" % num
        if num in streams:
            data = streams[num]
            if key is not None:
                okey = hashlib.md5(key + struct.pack("<I", num)[:3] + b"\0\0").digest()[:10]
                data = rc4(okey, data)
            out += b"<< /Length %d >>\nstream\n" % len(data) + data + b"\nendstream"
        else:
            out += objects[num]
        out += b"\nendobj\n"
    xref = len(out)
    size = max(offsets) + 1
    out += b"xref\n0 %d\n0000000000 65535 f \n" % size
    for num in range(1, size):
        out += b"%010d 00000 n \n" % offsets[num]
    trailer = "<< /Size %d /Root 1 0 R /ID [<%s> <%s>]" % (size, file_id.hex(), file_id.hex())
    if enc_dict:
        trailer += " /Encrypt %d 0 R" % encrypt_no
    out += ("trailer\n" + trailer + " >>\nstartxref\n%d\n%%%%EOF\n" % xref).encode()
    return bytes(out)


def write(path: str, data: bytes) -> None:
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def main(argv) -> int:
    if len(argv) == 4 and argv[1] == "--pages":
        count = int(argv[2])
        write(argv[3], build([("Page %d" % (i + 1), 612, 792) for i in range(count)]))
        return 0
    if len(argv) != 1:
        print(__doc__)
        return 2
    here = os.path.dirname(os.path.abspath(__file__))
    write(os.path.join(here, "two-pages.pdf"), build([("Page one", 612, 792), ("Page two", 612, 792)]))
    write(os.path.join(here, "landscape.pdf"), build([("Landscape", 792, 612)]))
    write(os.path.join(here, "encrypted.pdf"), build([("Secret", 612, 792)], password="x"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
