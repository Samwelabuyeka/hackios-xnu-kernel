#!/usr/bin/env python3
"""Generate the decoder table for the PAC emulator (pac_decoder.h) from what GNU as assembles.

Why generated: a hand-typed table of pointer-authentication encodings is a table typed from memory,
and a wrong bit position is a kernel bug that shows up as a random crash in user space. Every entry
here is produced by assembling the instruction for armv8.3-a, and every operand position is
*inferred*: the generator assembles variants that move one register bit, or one bit of the immediate,
at a time, and records which encoding bits moved. Nothing about the layout is assumed -- not even
that a field is contiguous.

Three things that inference caught, and that a typed table gets wrong:
  * PACGA's third register is at bits 20:16, where every other PAC form has no third register;
  * LDRAA/LDRAB's offset is a 10-bit signed field *split* across bit 22 (the sign) and bits 20:12,
    scaled by 8. So -4096 is S=1,imm9=0 and -2048 is S=1,imm9=0x100. A "plain byte offset" decoder
    is wrong for every negative offset;
  * PACIA Xd, Xn signs Xd using Xn as the modifier -- it does not copy Xn into Xd. Getting that
    backwards changes nothing for identity signing but would break a hashing implementation, and
    the emulator's tests check the operand wiring explicitly.

If an inferred field is not a clean ascending bit range, or a probe moves more than one encoding bit,
this script fails rather than emitting a table that is wrong in a way the tests might not cover.

usage: gen_encodings.py [output.h]
"""
import os
import subprocess
import sys
import tempfile

AS = os.environ.get("AS", "aarch64-linux-gnu-as")
OBJCOPY = os.environ.get("OBJCOPY", "aarch64-linux-gnu-objcopy")

# (assembly template, op enum, operand slots in assembler order)
SPECS = [
    ("pacia %s, %s", "PAC_OP_PACIA", ["rd", "rn"]),
    ("pacib %s, %s", "PAC_OP_PACIB", ["rd", "rn"]),
    ("pacda %s, %s", "PAC_OP_PACDA", ["rd", "rn"]),
    ("pacdb %s, %s", "PAC_OP_PACDB", ["rd", "rn"]),
    ("autia %s, %s", "PAC_OP_AUTIA", ["rd", "rn"]),
    ("autib %s, %s", "PAC_OP_AUTIB", ["rd", "rn"]),
    ("autda %s, %s", "PAC_OP_AUTDA", ["rd", "rn"]),
    ("autdb %s, %s", "PAC_OP_AUTDB", ["rd", "rn"]),
    ("paciza %s", "PAC_OP_PACIZA", ["rd"]),
    ("pacizb %s", "PAC_OP_PACIZB", ["rd"]),
    ("pacdza %s", "PAC_OP_PACDZA", ["rd"]),
    ("pacdzb %s", "PAC_OP_PACDZB", ["rd"]),
    ("autiza %s", "PAC_OP_AUTIZA", ["rd"]),
    ("autizb %s", "PAC_OP_AUTIZB", ["rd"]),
    ("autdza %s", "PAC_OP_AUTDZA", ["rd"]),
    ("autdzb %s", "PAC_OP_AUTDZB", ["rd"]),
    ("xpaci %s", "PAC_OP_XPACI", ["rd"]),
    ("xpacd %s", "PAC_OP_XPACD", ["rd"]),
    ("pacga %s, %s, %s", "PAC_OP_PACGA", ["rd", "rn", "rm"]),
    ("retaa", "PAC_OP_RETAA", []),
    ("retab", "PAC_OP_RETAB", []),
    ("braa %s, %s", "PAC_OP_BRAA", ["rn", "rm"]),
    ("brab %s, %s", "PAC_OP_BRAB", ["rn", "rm"]),
    ("blraa %s, %s", "PAC_OP_BLRAA", ["rn", "rm"]),
    ("blrab %s, %s", "PAC_OP_BLRAB", ["rn", "rm"]),
    ("braaz %s", "PAC_OP_BRAAZ", ["rn"]),
    ("brabz %s", "PAC_OP_BRABZ", ["rn"]),
    ("blraaz %s", "PAC_OP_BLRAAZ", ["rn"]),
    ("blrabz %s", "PAC_OP_BLRABZ", ["rn"]),
    ("ldraa %s, [%s, #%d]", "PAC_OP_LDRAA_IMM", ["rd", "rn", "imm"]),
    ("ldrab %s, [%s, #%d]", "PAC_OP_LDRAB_IMM", ["rd", "rn", "imm"]),
    ("eretaa", "PAC_OP_ERETAA", []),
    ("eretab", "PAC_OP_ERETAB", []),
]

# The offset of LDRAA/LDRAB is a signed field scaled by 8. Its width is not assumed: the generator
# tries the candidates and keeps the one whose probes all assemble.
IMM_WIDTHS = (10, 9, 12)


def fill(tmpl, slots, **over):
    args = [over.get("imm", 0) if s == "imm" else over.get(s, "x0") for s in slots]
    return tmpl % tuple(args)


def reg_variants(tmpl, slots):
    out = [(fill(tmpl, slots), {"kind": "base"})]
    for slot in slots:
        if slot == "imm":
            continue
        for bit in range(5):
            out.append((fill(tmpl, slots, **{slot: "x%d" % (1 << bit)}),
                        {"kind": "reg", "slot": slot, "bit": bit}))
    return out


def imm_probes(width):
    """(offset, raw field value) probing each bit of a `width`-bit signed field scaled by 8"""
    out = []
    for b in range(width - 1):
        out.append(((1 << b) * 8, 1 << b))
    out.append((-(1 << (width - 1)) * 8, -(1 << (width - 1))))   # the sign bit alone
    return out


def assemble(lines):
    src = [".text", ".globl t", "t:"] + ["\t" + t for t, _ in lines]
    with tempfile.TemporaryDirectory() as d:
        s, o, b = (os.path.join(d, n) for n in ("in.s", "in.o", "in.bin"))
        open(s, "w").write("\n".join(src) + "\n")
        p = subprocess.run([AS, "-march=armv8.3-a", "-o", o, s], capture_output=True)
        if p.returncode != 0:
            return None
        subprocess.run([OBJCOPY, "-O", "binary", "--only-section=.text", o, b], check=True)
        raw = open(b, "rb").read()
    return [int.from_bytes(raw[i:i + 4], "little") for i in range(0, len(raw) - 3, 4)]


def single_bit_delta(base, enc, what):
    d = base ^ enc
    if d == 0 or d & (d - 1):
        sys.exit("gen_encodings: %s moves several encoding bits (0x%x): not a plain bit" % (what, d))
    return d.bit_length() - 1


def infer_slot_bits(base, vs, encs, start, slots, tmpl):
    bits = {s: [None] * 5 for s in slots if s != "imm"}
    for i, (text, desc) in enumerate(vs):
        if desc["kind"] != "reg":
            continue
        bits[desc["slot"]][desc["bit"]] = single_bit_delta(base, encs[start + i], text)
    for slot, b in bits.items():
        if None in b or b != sorted(b) or (b[-1] - b[0]) != 4:
            sys.exit("gen_encodings: %r slot %s: bits %s are not 5 contiguous ascending positions"
                     % (tmpl, slot, b))
    return bits


def infer_imm(base, tmpl, slots, start):
    """Find the immediate's width and the encoding bit of each of its bits.

    Returns (width, imm_pos) where imm_pos[b] is the encoding bit carrying bit b of the raw field.
    The bits need not be contiguous: LDRAA's sign bit sits at 22, away from imm9 at 20:12.
    """
    for width in IMM_WIDTHS:
        probes = imm_probes(width)
        lines = [(fill(tmpl, slots, imm=off), {"kind": "imm", "raw": raw, "off": off})
                 for off, raw in probes]
        encs = assemble(lines)
        if encs is None:
            continue
        imm_pos = []
        ok = True
        for i, (text, desc) in enumerate(lines):
            try:
                imm_pos.append(single_bit_delta(base, encs[i], text))
            except SystemExit:
                ok = False
                break
        if not ok or len(set(imm_pos)) != width:
            continue
        # the probes must reconstruct every value: check a few that are not single-bit
        checks = [(-8, -1), (8 * ((1 << (width - 1)) - 1), (1 << (width - 1)) - 1),
                  (min(off for off, _ in probes), min(r for _, r in probes))]
        clines = [(fill(tmpl, slots, imm=off), {"kind": "imm", "raw": raw, "off": off})
                  for off, raw in checks]
        cencs = assemble(clines)
        if cencs is None:
            continue
        good = True
        for i, (off, raw) in enumerate(checks):
            want = 0
            for b in range(width):
                if (raw & 0xFFFFFFFF) >> b & 1:
                    want |= 1 << imm_pos[b]
            if (base ^ cencs[i]) != want:
                good = False
                break
        if good:
            return width, imm_pos
    sys.exit("gen_encodings: could not establish the immediate layout of %r" % tmpl)


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else "pac_decoder.h"
    rows = []
    for tmpl, op, slots in SPECS:
        vs = reg_variants(tmpl, slots)
        encs = assemble(vs)
        if encs is None:
            sys.exit("gen_encodings: %r did not assemble: this CPU's assembler is older than "
                     "armv8.3?" % tmpl)
        base = encs[0]
        slot_bits = infer_slot_bits(base, vs, encs, 0, slots, tmpl)
        imm_width, imm_pos = (0, []) if "imm" not in slots else infer_imm(base, tmpl, slots, 0)
        mask = 0xFFFFFFFF
        for i in range(len(vs)):
            mask &= ~(base ^ encs[i]) & 0xFFFFFFFF
        # the immediate is an operand, not part of the instruction's identity: clear it from the
        # mask, or an LDRAA with any non-zero offset would never match this form
        for b in range(imm_width):
            mask &= ~(1 << imm_pos[b]) & 0xFFFFFFFF
        rows.append((base, mask, op, slots, slot_bits, imm_width, imm_pos, tmpl))

    out = [
        "/* generated by gen_encodings.py from GNU as -march=armv8.3-a: do not edit */",
        "/* every operand position below was inferred from the assembler, not typed. */",
        "struct pac_form {",
        "\tuint32_t value, mask;",
        "\tuint8_t op;",
        "\tuint8_t has_rd, has_rn, has_rm;   /* 0: the instruction has no such operand */",
        "\tuint8_t rd_pos, rn_pos, rm_pos;   /* first encoding bit of that 5-bit field */",
        "\tuint8_t imm_width;                /* 0: no immediate; else bits in the raw field */",
        "\tsigned char imm_pos[10];          /* encoding bit of raw bit b, -1 if none */",
        "};",
        "static const struct pac_form pac_forms[] = {",
    ]
    for base, mask, op, slots, slot_bits, imm_width, imm_pos, tmpl in rows:
        rd_pos = slot_bits["rd"][0] if "rd" in slot_bits else 0
        rn_pos = slot_bits["rn"][0] if "rn" in slot_bits else 0
        rm_pos = slot_bits["rm"][0] if "rm" in slot_bits else 0
        pos = ",".join(str(p) for p in imm_pos) + "," + ",".join(["-1"] * (10 - len(imm_pos))) \
            if imm_pos else ",".join(["-1"] * 10)
        out.append(
            '\t{ 0x%08xu, 0x%08xu, %-22s %d, %d, %d, %2d, %2d, %2d, %2d, {%s} }, /* %s */'
            % (base, mask, op + ",", "rd" in slots, "rn" in slots, "rm" in slots,
               rd_pos, rn_pos, rm_pos, imm_width, pos,
               tmpl.replace("%s", "x0").replace("%d", "0")))
    out.append("};")
    out.append("#define PAC_FORMS_COUNT %d" % len(rows))
    open(out_path, "w").write("\n".join(out) + "\n")
    print("%s: %d forms" % (out_path, len(rows)))


if __name__ == "__main__":
    main()
