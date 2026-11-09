#!/usr/bin/env python3
"""Assemble genuine screenshots S01-S24 and their one-line captions into <SRN>.pdf.

    python3 docs/build_pdf.py --srn YOUR_FULL_SRN --shots DIR [--captions FILE]

DIR must hold S01.png ... S24.png (.png/.jpg/.jpeg). Nothing is generated or
edited: the images are placed in the PDF exactly as captured.
"""
import argparse
import os
import re
import sys

try:
    from PIL import Image
    from reportlab.lib.pagesizes import A4, landscape
    from reportlab.lib.utils import ImageReader, simpleSplit
    from reportlab.pdfgen import canvas
except ImportError:
    sys.exit("error: needs reportlab and pillow:  python3 -m pip install "
             "--user reportlab pillow")

PLACEHOLDERS = {"YOUR_FULL_SRN", "PES1UGXXXXXXX", "SRN", "YOURSRN"}
EXTS = (".png", ".jpg", ".jpeg")


def load_captions(path):
    caps = {}
    for line in open(path, encoding="utf-8"):
        m = re.match(r"\s*(S\d\d)\s*\|\s*(.+?)\s*$", line)
        if m:
            caps[m.group(1)] = m.group(2)
    return caps


def find_image(folder, tag):
    for ext in EXTS:
        for name in (tag + ext, tag.lower() + ext):
            p = os.path.join(folder, name)
            if os.path.isfile(p):
                return p
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--srn", required=True, help="your complete SRN")
    ap.add_argument("--shots", required=True, help="folder with S01..S24 images")
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument("--captions", default=os.path.join(here, "captions.txt"))
    ap.add_argument("--out-dir", default=".", help="where to write <SRN>.pdf")
    a = ap.parse_args()

    srn = a.srn.strip()
    if not re.fullmatch(r"[A-Za-z0-9_-]+", srn) or srn.upper() in PLACEHOLDERS \
            or "XXXX" in srn.upper():
        sys.exit("error: '%s' is not a real SRN (placeholders are not allowed)" % srn)

    caps = load_captions(a.captions)
    tags = ["S%02d" % i for i in range(1, 25)]
    missing = [t for t in tags if find_image(a.shots, t) is None]
    if missing:
        sys.exit("error: missing screenshots (capture them, do not skip): "
                 + ", ".join(missing))
    nocap = [t for t in tags if t not in caps]
    if nocap:
        sys.exit("error: no caption for: " + ", ".join(nocap))

    out = os.path.join(a.out_dir, srn + ".pdf")
    page = landscape(A4)
    pw, ph = page
    margin, cap_h = 28, 56
    c = canvas.Canvas(out, pagesize=page)
    c.setTitle(srn)
    for t in tags:
        img_path = find_image(a.shots, t)
        with Image.open(img_path) as im:
            iw, ih = im.size
        c.setFont("Helvetica-Bold", 14)
        c.drawString(margin, ph - margin - 8, t)
        box_w, box_h = pw - 2 * margin, ph - 2 * margin - 24 - cap_h
        scale = min(box_w / iw, box_h / ih)
        w, h = iw * scale, ih * scale
        x, y = margin + (box_w - w) / 2, margin + cap_h + (box_h - h)
        c.drawImage(ImageReader(img_path), x, y, w, h)
        c.setFont("Helvetica", 11)
        lines = simpleSplit(caps[t], "Helvetica", 11, pw - 2 * margin)
        for k, line in enumerate(lines[:2]):
            c.drawString(margin, margin + cap_h - 22 - 14 * k, line)
        c.showPage()
    c.save()
    print("wrote %s (%d pages)" % (out, len(tags)))


if __name__ == "__main__":
    main()
