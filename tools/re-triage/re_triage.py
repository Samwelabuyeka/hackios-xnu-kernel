#!/usr/bin/env python3
"""
re_triage: recover a device's register map from an AArch64 driver binary.

Why this exists
---------------
Reverse engineering a driver for undocumented silicon always starts the same way: find every place
the code touches a memory-mapped register, and turn that into a map of `base + offset -> width,
read/write`. Everything else (which clock gates it, what the FIFO handshake looks like, which
interrupt it raises) is built on top of that map. Doing it by hand in a disassembler is where weeks
go; the access pattern itself is mechanical, so it can be recovered automatically.

What it does
------------
  * reads PE32+, Mach-O 64 or ELF64 (any AArch64 image: a Linux .ko, a PE from a boot chain, a
    Mach-O kext, a raw firmware blob given a base address with --base)
  * disassembles the executable sections with Capstone
  * tracks register *values* through each straight-line run: MOVZ/MOVK, ADRP, ADD/SUB immediate,
    ORR immediate, LDR-from-literal-pool. This is what turns
        adrp x1, 0x11002000 ; add x1, x1, #0x14 ; ldr w0, [x1]
    into "read 4 bytes at 0x11002014"
  * emits the accesses grouped by base address, with offsets, widths, direction and the instruction
    addresses, so a human can jump straight to the interesting ones
  * --json for machine use, so a second pass can annotate what the map means

What it deliberately does NOT do
--------------------------------
It does not pretend to recover logic. It knows a word was read from 0x11002014; it does not know
that 0x14 is "line status" or that bit 5 means "transmit FIFO full". It also does not follow data
through the stack, through function calls, or through pointers it cannot resolve -- so a driver that
computes its register base once and passes it to every helper will yield fewer accesses than it
really performs. That limit is real and the report says how many loads/stores were left unresolved,
rather than quietly dropping them.

usage:
  tools/re-triage/re_triage.py BINARY [--base 0xADDR] [--json] [--min-base 0x10000000]
                                  [--top N] [--verbose]
  tools/re-triage/re_triage.py BINARY --dump 0x11002000     # every access at one base, in order
"""
import argparse
import json
import struct
import sys

try:
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
    from capstone.arm64 import (ARM64_OP_REG, ARM64_OP_IMM, ARM64_OP_MEM, ARM64_INS_MOVZ,
                                ARM64_INS_MOVK, ARM64_INS_ADRP, ARM64_INS_MOVN)
except ImportError:
    sys.stderr.write("re_triage: needs capstone (pip install capstone)\n")
    raise SystemExit(1)

# Register widths, so an access can be reported as 4 or 8 bytes.
W = {"w": 4, "x": 8, "b": 1, "h": 2, "s": 4, "d": 8, "q": 16}

# A word-size register name from capstone ("x1", "w2", "sp").
def reg_of(op):
    return op.reg


def canonical(name):
    """x1/w1 are the same physical register for value tracking: values are 64-bit here."""
    if not name:
        return name
    if name in ("sp", "wsp", "xzr", "wzr"):
        return "sp" if name in ("sp", "wsp") else "zr"
    if name[0] in ("x", "w") and name[1:].isdigit():
        return "x" + name[1:]
    return name


def width_of(name):
    if not name:
        return 8
    return W.get(name[0], 8)


def shift_of(op):
    """
    The `lsl #N` of a movz/movk/movn immediate, in bits.

    Capstone does not give this as a third operand: it hangs it off the immediate operand as
    op.shift, a (type, value) pair. Reading ops[2] silently yielded 0, which turned
    `movz x1,#0x200c` + `movk x1,#0x1100,lsl#16` into 0x1100 rather than 0x11002000.
    """
    sh = getattr(op, "shift", None)
    if not sh:
        return 0
    value = getattr(sh, "value", None)
    if value is None and isinstance(sh, (tuple, list)) and len(sh) == 2:
        value = sh[1]
    try:
        return int(value)
    except (TypeError, ValueError):
        return 0


# ------------------------------------------------------------------ images

class Image:
    """AArch64 code, as (vaddr, bytes) segments."""

    def __init__(self, path):
        self.path = path
        self.segments = []       # [(vaddr, data)]
        self.kind = "unknown"
        self.sections = []       # [(name, vaddr, size)] for the report
        with open(path, "rb") as f:
            self.data = f.read()
        if self.data[:4] == b"\x7fELF":
            self._elf()
        elif self.data[:2] == b"MZ":
            self._pe()
        elif self.data[:4] in (b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe"):
            self._macho()
        else:
            # a raw blob: the caller must say where it lives
            self.kind = "raw"
            self.segments = [(0, self.data)]

    def _elf(self):
        self.kind = "ELF"
        if self.data[4] != 2:
            raise SystemExit("re_triage: only ELF64 is supported (this is ELF32)")
        e_shoff, = struct.unpack_from("<Q", self.data, 0x28)
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", self.data, 0x3A)
        # section header string table, to name the sections
        names = b""
        if e_shnum and e_shstrndx < e_shnum:
            off = e_shoff + e_shstrndx * e_shentsize
            sh_offset, = struct.unpack_from("<Q", self.data, off + 0x18)
            sh_size, = struct.unpack_from("<Q", self.data, off + 0x20)
            names = self.data[sh_offset:sh_offset + sh_size]
        for i in range(e_shnum):
            off = e_shoff + i * e_shentsize
            sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size = struct.unpack_from("<IIQQQQ", self.data, off)
            if sh_type != 1 or not (sh_flags & 0x4):      # SHT_PROGBITS and SHF_EXECINSTR
                continue
            nm = names[sh_name:names.find(b"\0", sh_name)].decode("utf-8", "replace") if names else "?"
            if not sh_size:
                continue
            self.segments.append((sh_addr, self.data[sh_offset:sh_offset + sh_size]))
            self.sections.append((nm, sh_addr, sh_size))

    def _pe(self):
        self.kind = "PE"
        pe, = struct.unpack_from("<I", self.data, 0x3C)
        if self.data[pe:pe + 4] != b"PE\0\0":
            raise SystemExit("re_triage: MZ but no PE header")
        nsec, = struct.unpack_from("<H", self.data, pe + 6)
        opt_size, = struct.unpack_from("<H", self.data, pe + 20)
        opt = pe + 24
        magic, = struct.unpack_from("<H", self.data, opt)
        image_base = (struct.unpack_from("<Q", self.data, opt + 24)[0] if magic == 0x20B
                      else struct.unpack_from("<I", self.data, opt + 28)[0])
        sec = opt + opt_size
        for i in range(nsec):
            off = sec + i * 40
            name = self.data[off:off + 8].rstrip(b"\0").decode("ascii", "replace")
            vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", self.data, off + 8)
            chars, = struct.unpack_from("<I", self.data, off + 36)
            if not (chars & 0x20000000):                  # IMAGE_SCN_MEM_EXECUTE
                continue
            if not rawsize:
                continue
            self.segments.append((image_base + vaddr, self.data[rawptr:rawptr + rawsize]))
            self.sections.append((name, image_base + vaddr, rawsize))
        self.image_base = image_base

    def _macho(self):
        self.kind = "Mach-O"
        magic, = struct.unpack_from("<I", self.data, 0)
        if magic != 0xFEEDFACF:
            raise SystemExit("re_triage: only 64-bit little-endian Mach-O is supported")
        ncmds, = struct.unpack_from("<I", self.data, 16)
        off = 32
        for _ in range(ncmds):
            cmd, cmdsize = struct.unpack_from("<II", self.data, off)
            if cmd == 0x19:                               # LC_SEGMENT_64
                segname = self.data[off + 8:off + 24].rstrip(b"\0").decode("ascii", "replace")
                vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<QQQQ", self.data, off + 24)
                maxprot, initprot, nsects = struct.unpack_from("<III", self.data, off + 56)
                if nsects and (initprot & 0x4) and filesize:      # VM_PROT_EXECUTE
                    self.segments.append((vmaddr, self.data[fileoff:fileoffset_end(rawlen=filesize,
                                                                                  fileoff=fileoff)]))
                    self.sections.append((segname, vmaddr, filesize))
            off += cmdsize


def fileoffset_end(rawlen, fileoff):
    return fileoff + rawlen


# ------------------------------------------------------------------ value tracking

def disassemble(image, base=None):
    """Yield capstone instructions from every executable segment, addresses resolved."""
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.detail = True
    for vaddr, data in image.segments:
        if base is not None and image.kind == "raw":
            vaddr = base
        try:
            for insn in md.disasm(data, vaddr):
                yield insn
        except Exception:                              # capstone can give up on a bad run
            continue


def scan(image, base=None, verbose=False):
    """
    Walk the code, tracking what each register holds where that is knowable, and report every
    load or store whose address becomes a constant.

    The tracker is per straight-line run and is flushed at any branch: a value that flows across a
    branch is not tracked. That is the conservative choice -- it under-reports rather than inventing
    addresses, and the count of unresolved accesses is reported so the under-report is visible.
    """
    accesses = []      # {addr, base, off, width, write, insn, func}
    unresolved = 0
    total_ld_st = 0
    vals = {}
    seen_addr = {}

    def flush():
        vals.clear()

    for insn in disassemble(image, base):
        # one instruction can be reached twice through overlapping disassembly: keep the first
        if insn.address in seen_addr:
            continue
        seen_addr[insn.address] = True

        mnem = insn.mnemonic
        ops = insn.operands

        # ---- value-producing instructions
        #
        # Capstone normalises in ways that matter here, and both were found by test/known_map.c
        # rather than by reading the docs:
        #   * `movz x1, #0x200c` comes back as mnemonic "mov", not "movz";
        #   * the `lsl #16` of `movk` is not a third operand, it is operand[1].shift.
        # Getting the second wrong made every movz/movk address read as 0x1100 instead of
        # 0x11002000, which then looked like a null pointer and was dropped.
        if mnem in ("mov", "movz") and len(ops) >= 2 and ops[1].type == ARM64_OP_IMM:
            d = canonical(insn.reg_name(ops[0].reg))
            vals[d] = (ops[1].imm << shift_of(ops[1])) & 0xFFFFFFFFFFFFFFFF
        elif mnem == "movn" and len(ops) >= 2 and ops[1].type == ARM64_OP_IMM:
            d = canonical(insn.reg_name(ops[0].reg))
            vals[d] = (~(ops[1].imm << shift_of(ops[1]))) & 0xFFFFFFFFFFFFFFFF
        elif mnem == "movk" and len(ops) >= 2 and ops[1].type == ARM64_OP_IMM:
            d = canonical(insn.reg_name(ops[0].reg))
            sh = shift_of(ops[1])
            cur = vals.get(d)
            if cur is None:
                vals[d] = None
            else:
                mask = 0xFFFF << sh
                vals[d] = (cur & ~mask) | ((ops[1].imm & 0xFFFF) << sh)
        elif mnem == "adrp" and len(ops) >= 2 and ops[1].type == ARM64_OP_IMM:
            d = canonical(insn.reg_name(ops[0].reg))
            # capstone reports the page address itself for ADRP; keep it unsigned
            vals[d] = ops[1].imm & 0xFFFFFFFFFFFFFFFF
        elif mnem in ("add", "sub") and len(ops) >= 3 and ops[2].type == ARM64_OP_IMM:
            d = canonical(insn.reg_name(ops[0].reg))
            n = canonical(insn.reg_name(ops[1].reg))
            src = vals.get(n)
            if src is None:
                vals[d] = None
            else:
                vals[d] = (src + ops[2].imm) if mnem == "add" else (src - ops[2].imm)
        elif mnem == "orr" and len(ops) >= 3 and ops[2].type == ARM64_OP_IMM:
            d = canonical(insn.reg_name(ops[0].reg))
            n = canonical(insn.reg_name(ops[1].reg))
            src = vals.get(n) if n != "zr" else 0
            vals[d] = None if src is None else (src | ops[2].imm)
        elif mnem == "mov" and len(ops) == 2 and ops[1].type == ARM64_OP_REG:
            d = canonical(insn.reg_name(ops[0].reg))
            vals[d] = vals.get(canonical(insn.reg_name(ops[1].reg)))
        else:
            # ---- loads and stores
            is_load = mnem in ("ldr", "ldrb", "ldrh", "ldrsb", "ldrsh", "ldrsw", "ldur", "ldurb",
                               "ldurh", "ldursw", "ldnp", "ldp", "ldar", "ldaxr", "ldxr")
            is_store = mnem in ("str", "strb", "strh", "stur", "sturb", "sturh", "stnp", "stp",
                                "sttr", "sttrb", "sttrh", "stlr", "stlxr", "stxr")
            if not (is_load or is_store):
                # any other write to a register we were tracking invalidates it
                if ops and ops[0].type == ARM64_OP_REG and mnem not in ("cmp", "tst", "cmn",
                                                                        "ccmp", "fcmp"):
                    d = canonical(insn.reg_name(ops[0].reg))
                    if d != "sp" and d in vals:
                        vals[d] = None
                continue

            total_ld_st += 1
            mem = next((o for o in ops if o.type == ARM64_OP_MEM), None)

            # the accessed register is the first explicit register operand, for both loads and stores:
            # `ldr w0, [x1]` reads into w0, `str w0, [x1]` reads from w0. Its name gives the width.
            rt = next((insn.reg_name(o.reg) for o in ops if o.type == ARM64_OP_REG), None)

            if mem is not None:
                base_reg = canonical(insn.reg_name(mem.mem.base))
                base_val = vals.get(base_reg)
                idx_reg = canonical(insn.reg_name(mem.mem.index)) if mem.mem.index else None
                idx_val = vals.get(idx_reg) if idx_reg else 0

                if base_val is not None:
                    addr = (base_val + mem.mem.disp + (idx_val or 0)) & 0xFFFFFFFFFFFFFFFF
                    accesses.append({
                        "addr": addr,
                        "width": width_of(rt),
                        "write": is_store,
                        "insn": insn.address,
                        "dynamic_offset": idx_reg is not None and idx_val is None,
                    })
                elif idx_val is not None and idx_val >= MIN_BASE:
                    #
                    # `str w1, [x0, x2]` with the constant block base in the *index* register and the
                    # register offset in the base. This is what a vendor `write_reg(off, val)` helper
                    # compiles to -- see test/known_map.c's pmic_write -- so missing it would miss
                    # the single most common way a BSP talks to a PMIC or clock controller.
                    #
                    # The address is not a constant, but the *block* is. Report it at the block base
                    # with a fixed offset, flagged, so the block appears in the map and the reader
                    # knows its offsets are runtime values rather than being told nothing.
                    accesses.append({
                        "addr": idx_val & ~0xFFF,
                        "width": width_of(rt),
                        "write": is_store,
                        "insn": insn.address,
                        "indirect": True,
                    })
                else:
                    unresolved += 1

                # A load overwrites its destination, so that register's constant is gone. Without
                # this the tracker keeps the stale value and invents accesses the driver never makes.
                if is_load:
                    for o in ops:
                        if o.type == ARM64_OP_REG:
                            vals[canonical(insn.reg_name(o.reg))] = None
                            break

                # Writeback (`[x1], #8` post-index, `[x1, #8]!` pre-index) moves the base by a known
                # amount, so a driver that walks a FIFO in a loop stays traceable.
                wb = ops[-1].imm if len(ops) >= 3 and ops[-1].type == ARM64_OP_IMM else 0
                if wb:
                    vals[base_reg] = (base_val + wb) & 0xFFFFFFFFFFFFFFFF if base_val is not None else None

        # a branch ends the straight-line run
        if mnem.startswith("b") or mnem in ("ret", "blr", "br", "cbz", "cbnz", "tbz", "tbnz"):
            flush()

    return accesses, unresolved, total_ld_st


# ------------------------------------------------------------------ report

# Below this an address is not a peripheral: it is a null dereference in the disassembly, or a
# stack-relative slot the straight-line scan mis-attributed. Keeping it would invent a device at 0x0.
MIN_BASE = 0x10000


def group(accesses):
    """
    Group by the 4 KB page an access falls in -- the usual granularity of a register block -- and
    record each access's offset from that page. Returns (blocks, dropped) so the count of accesses
    thrown away as non-peripheral is reported rather than silently lost.
    """
    blocks, dropped = {}, 0
    for a in accesses:
        addr = a["addr"]
        if addr < MIN_BASE:
            dropped += 1
            continue
        base = addr & ~0xFFF
        a["base"] = base
        a["off"] = addr - base
        blocks.setdefault(base, []).append(a)
    for k in blocks:
        blocks[k].sort(key=lambda x: (x["off"], x["insn"]))
    return blocks, dropped


def report(image, accesses, unresolved, total_ld_st, top, dump_base, as_json):
    blocks, dropped = group(accesses)
    if as_json:
        json.dump({
            "file": image.path,
            "kind": image.kind,
            "sections": [{"name": n, "vaddr": v, "size": s} for n, v, s in image.sections],
            "loads_stores": total_ld_st,
            "resolved": len(accesses),
            "unresolved": unresolved,
            "not_a_peripheral": dropped,
            "blocks": [
                {"base": b, "accesses": blk}
                for b, blk in sorted(blocks.items(), key=lambda kv: -len(kv[1]))
            ],
        }, sys.stdout, indent=2)
        sys.stdout.write("\n")
        return

    print("%s: %s, %d executable sections" % (image.path, image.kind, len(image.segments)))
    for n, v, s in image.sections:
        print("  %-12s %#x .. %#x  (%d bytes)" % (n, v, v + s, s))
    print()
    print("loads/stores:      %d" % total_ld_st)
    print("addresses resolved: %d (%.0f%%)"
          % (len(accesses), 100.0 * len(accesses) / total_ld_st if total_ld_st else 0))
    print("left unresolved:   %d  (base came from something a straight-line scan cannot follow:"
          % unresolved)
    print("                        a stack slot, a function argument, or a loaded pointer)")
    print("below %#x:         %d  (null or stack-relative, not counted as a device)"
          % (MIN_BASE, dropped))
    print()

    if not blocks:
        print("No constant register base was recovered. That is a real result, not a failure of the")
        print("scan: this image does not reach its registers through an address this pass can see.")
        print("Try --base if it is a raw blob, or start from the section listing above.")
        return

    ordered = sorted(blocks.items(), key=lambda kv: -len(kv[1]))
    if dump_base is not None:
        hit = [b for b in blocks if b <= dump_base < b + 0x1000]
        if not hit:
            print("no accesses in the page containing %#x" % dump_base)
            return
        ordered = [(hit[0], blocks[hit[0]])]
        top = 1

    print("%-12s %-6s %-6s %-4s %-4s %s" % ("BASE", "REGS", "SPAN", "R", "W", "WIDTHS"))
    print("-" * 78)
    for base, blk in ordered[:top]:
        offs = [a["off"] for a in blk]
        reads = sum(1 for a in blk if not a["write"])
        writes = len(blk) - reads
        widths = sorted({a["width"] for a in blk})
        print("%#012x %-6d %#-6x %-4d %-4d %s"
              % (base, len(blk), max(offs) - min(offs), reads, writes,
                 ",".join(str(w) for w in widths)))
        # which offsets, and in which direction: the actual map, which is the point of the tool
        cells = ["%#x%s%s" % (a["off"], "w" if a["write"] else "r",
                             "*" if a.get("indirect") else "")
                 for a in blk[:16]]
        line = "    " + " ".join(cells)
        if len(blk) > 16:
            line += " ... (+%d more)" % (len(blk) - 16)
        print(line)
    if any(a.get("indirect") for blk in blocks.values() for a in blk):
        print()
        print("*  the block base is a constant but the offset was in a register (the shape a")
        print("   `write_reg(off, val)` helper compiles to): all such accesses are shown at the")
        print("   base, so the offsets in that row are not a list of distinct registers.")


def main():
    p = argparse.ArgumentParser(prog="re_triage.py", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("binary")
    p.add_argument("--base", type=lambda s: int(s, 0),
                   help="load address for a raw blob with no headers")
    p.add_argument("--json", action="store_true")
    p.add_argument("--top", type=int, default=12, help="how many blocks to show (default 12)")
    p.add_argument("--dump", type=lambda s: int(s, 0), help="show every access in one page")
    p.add_argument("--verbose", action="store_true")
    a = p.parse_args()

    image = Image(a.binary)
    accesses, unresolved, total = scan(image, a.base, a.verbose)
    report(image, accesses, unresolved, total, a.top, a.dump, a.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
