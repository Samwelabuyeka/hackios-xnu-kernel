#!/usr/bin/env python3
"""Print what a HakiOS MediaTek port needs to know from a phone's own device tree.

usage: tools/mtk-soc-args.py DEVICE-TREE.dtb        (or /sys/firmware/fdt)

Why a separate tool from soc-args.py: that one reads Qualcomm's nodes -- qcom,ipa, qcom,ipcc,
qcom,rpmh-rsc, the PAS ids -- because the kernel it feeds has Qualcomm drivers. A MediaTek phone has
none of those nodes. What it does have, and what the boot shim and a stage-1 loader need, is:

  console   the UART's address and clock, so the first line of output appears at all;
  memory    the RAM banks and the reserved regions the firmware keeps for itself -- getting these
            wrong means the kernel writes over TrustZone or the modem and the phone resets;
  display   the panel's geometry, and whether LK left a framebuffer lit;
  clocks    the top-clock-controller nodes, which is where a MediaTek port's real work is;
  storage   the UFS/eMMC host, so a flash driver knows where to begin.

It prints boot-args (mtk_*) and a summary for the port's README. It does not invent values: anything
the tree does not describe is reported as absent rather than guessed, because a guessed register
address in a boot loader is a bricked phone.

Only numbers and names are read out; no part of the tree is copied into the repository.
"""
import struct
import sys

RANGES = []


def key_of(path):
    """The dictionary key for a node path. The root's name is empty, so it would join to ''; '/' is
    the form every lookup in this file uses, and having one function decide it keeps the node
    insertion and the property insertion from disagreeing (which raised KeyError on the root)."""
    return '/'.join(path) or '/'


def nodes_of(blob):
    if blob[:4] != b'\xd0\x0d\xfe\xed':
        sys.exit('mtk-soc-args.py: not a flattened device tree')
    off_struct, off_strings = struct.unpack('>II', blob[8:16])
    size_strings, = struct.unpack('>I', blob[32:36])
    strings = blob[off_strings:off_strings + size_strings]
    out, path, at = {}, [], off_struct
    while True:
        if at + 4 > len(blob):
            sys.exit('mtk-soc-args.py: truncated device tree')
        token, = struct.unpack('>I', blob[at:at + 4])
        at += 4
        if token == 1:                                  # FDT_BEGIN_NODE
            end = blob.index(b'\0', at)
            path.append(blob[at:end].decode(errors='replace'))
            at = (end + 4) & ~3
            out[key_of(path)] = {}
        elif token == 2:                                # FDT_END_NODE
            if path:
                path.pop()
        elif token == 3:                                # FDT_PROP
            length, name_off = struct.unpack('>II', blob[at:at + 8])
            at += 8
            name = strings[name_off:strings.index(b'\0', name_off)].decode(errors='replace')
            out[key_of(path)][name] = blob[at:at + length]
            at = (at + length + 3) & ~3
        elif token == 9:                                # FDT_END
            return out
        elif token != 4:                                # 4 = FDT_NOP
            sys.exit(f'mtk-soc-args.py: unexpected token {token}')


def prop(node, name):
    return node.get(name)


def strings_of(value):
    if not value:
        return []
    return [s.decode(errors='replace') for s in value.split(b'\0') if s]


def find(nodes, compatible):
    """first node whose compatible list contains `compatible`"""
    for path, props in nodes.items():
        if compatible in strings_of(props.get('compatible')):
            return path, props
    return None, None


def find_all(nodes, compatible):
    return [(p, n) for p, n in nodes.items() if compatible in strings_of(n.get('compatible'))]


def find_leaf(nodes, leaf):
    for path, props in nodes.items():
        if path.rsplit('/', 1)[-1].split('@', 1)[0] == leaf:
            return path, props
    return None, None


def cells(nodes, path, name, dflt):
    """#address-cells / #size-cells of a node, defaulting to `dflt` (arm64 MediaTek trees use 2/2)"""
    props = nodes.get(path, {})
    v = props.get(name)
    return struct.unpack('>I', v)[0] if v and len(v) == 4 else dflt


def regs(nodes, path, node):
    """every (address, size) pair in a node's reg, using its parent's cell counts"""
    if 'reg' not in node:
        return RANGES
    parent = path.rsplit('/', 1)[0] or '/'
    ac = cells(nodes, parent, '#address-cells', 2)
    sc = cells(nodes, parent, '#size-cells', 2)
    raw = node['reg']
    out, stride = [], 4 * (ac + sc)
    for off in range(0, len(raw) - stride + 1, stride):
        addr = struct.unpack_from('>Q' if ac == 2 else '>I', raw, off)[0]
        size = struct.unpack_from('>Q' if sc == 2 else '>I', raw, off + 4 * ac)[0]
        out.append((addr, size))
    return out


def u32(node, name, index=0):
    v = node.get(name)
    if not v or len(v) < (index + 1) * 4:
        return None
    return struct.unpack_from('>I', v, index * 4)[0]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    nodes = nodes_of(open(sys.argv[1], 'rb').read())
    found, missing = [], []

    def say(arg, value):
        found.append((arg, value))

    root = nodes.get('/', {})

    # ---- identity ---------------------------------------------------------------------------
    model = strings_of(prop(root, 'model'))
    compat = strings_of(prop(root, 'compatible'))

    # ---- console ----------------------------------------------------------------------------
    # MediaTek names its UARTs mediatek,mt6577-uart (MT65xx through MT67xx) or ns16550a; the
    # register stride is 4 bytes, which `reg-shift = <2>` expresses.
    uart_path = None
    for name in ('mediatek,mt6577-uart', 'mediatek,mtk-uart', 'ns16550a', 'arm,pl011'):
        p, n = find(nodes, name)
        if p and p not in ('/', '/chosen'):
            uart_path, uart_node = p, n
            break
    else:
        uart_node = None
    if uart_node is not None:
        r = regs(nodes, uart_path, uart_node)
        if r:
            say('mtk_uart', f'0x{r[0][0]:x}')
            shift = u32(uart_node, 'reg-shift', 0)
            say('mtk_uart_shift', shift if shift is not None else 2)
        else:
            missing.append('uart reg')
        if 'clock-frequency' in uart_node:
            say('mtk_uart_clock', u32(uart_node, 'clock-frequency'))
    else:
        missing.append('uart')

    # ---- memory -----------------------------------------------------------------------------
    for path, node in nodes.items():
        if strings_of(prop(node, 'device_type')) == ['memory']:
            for addr, size in regs(nodes, path, node):
                say('mtk_mem', f'0x{addr:x}+0x{size:x}')
        if path.rsplit('/', 1)[-1] == 'memory' and 'reg' in node and \
                strings_of(prop(node, 'device_type')) != ['memory']:
            missing.append(f'{path} has reg but no device_type="memory"')
    if not any(a == 'mtk_mem' for a, _ in found):
        missing.append('memory banks (no device_type="memory" node)')

    # reserved-memory: the firmware's own regions. A port that ignores these overwrites TrustZone.
    rm_path, rm = find_leaf(nodes, 'reserved-memory')
    reserved = []
    if rm is not None:
        for path, node in nodes.items():
            if not path.startswith(rm_path + '/') or path == rm_path:
                continue
            for addr, size in regs(nodes, path, node):
                reserved.append((path.rsplit('/', 1)[-1], addr, size))
        for name, addr, size in reserved:
            say('mtk_rsv', f'{name}:0x{addr:x}+0x{size:x}')

    # ---- display ----------------------------------------------------------------------------
    fb = None
    p, n = find(nodes, 'simple-framebuffer')
    if p:
        r = regs(nodes, p, n)
        fb = {
            'base': r[0][0] if r else None,
            'width': u32(n, 'width'), 'height': u32(n, 'height'),
            'stride': u32(n, 'stride'),
            'format': (strings_of(prop(n, 'format')) or ['?'])[0],
        }
    chosen = nodes.get('/chosen', {})
    lk_fb = None
    if u32(chosen, 'atag,videolfb-fb_base_l') is not None or \
            u32(chosen, 'atag,videolfb-fb_base') is not None:
        lo = u32(chosen, 'atag,videolfb-fb_base_l') or 0
        hi = u32(chosen, 'atag,videolfb-fb_base_h') or 0
        base = (hi << 32) | lo if hi else (u32(chosen, 'atag,videolfb-fb_base') or lo)
        lk_fb = {
            'base': base,
            'vram': u32(chosen, 'atag,videolfb-vramSize'),
            'lcm': (strings_of(prop(chosen, 'atag,videolfb-lcmname')) or ['?'])[0],
        }
    if fb and fb['base']:
        say('mtk_fb', f'0x{fb["base"]:x}')
        if fb['width'] and fb['height']:
            say('mtk_fb_geom', f'{fb["width"]}x{fb["height"]}x{fb["stride"]}')
    elif lk_fb:
        say('mtk_fb', f'0x{lk_fb["base"]:x}')
        missing.append('framebuffer geometry (LK lights the panel but its tree carries no width/height; '
                       'pass fb=WxH on the command line until the LCM table is read)')
    else:
        missing.append('framebuffer')

    # ---- clocks, storage: what a port has to work through -----------------------------------
    tzc = find_all(nodes, 'mediatek,mt6779-mmsys') + find_all(nodes, 'mediatek,mt6779-dispsys')
    disp = find_all(nodes, 'mediatek,mt6779-disp')
    ufs = find_all(nodes, 'mediatek,ufs') + find_all(nodes, 'mediatek,mt8183-ufs')
    mmc = find_all(nodes, 'mediatek,mtk-sd') + find_all(nodes, 'mediatek,mt6779-mmc')
    topckgen = [n for n in nodes.items()
                if any('topckgen' in c for c in strings_of(n[1].get('compatible')))]
    mmsys = find_all(nodes, 'mediatek,mmsys')

    # ---- report -----------------------------------------------------------------------------
    for arg, value in found:
        print(f'{arg}={value}')
    print('', file=sys.stderr)
    print('mtk-soc-args.py: what this tree describes', file=sys.stderr)
    print(f'  model:      {model[0] if model else "(none)"}', file=sys.stderr)
    print(f'  compatible: {" ".join(compat) if compat else "(none)"}', file=sys.stderr)
    print(f'  reserved:   {len(reserved)} regions', file=sys.stderr)
    print(f'  topckgen:   {len(topckgen)} node(s)', file=sys.stderr)
    print(f'  mmsys/disp: {len(mmsys)} / {len(disp)} node(s)', file=sys.stderr)
    print(f'  ufs / mmc:  {len(ufs)} / {len(mmc)} node(s)', file=sys.stderr)
    if missing:
        print('  absent:     ' + '; '.join(missing), file=sys.stderr)
    if not found:
        sys.exit('mtk-soc-args.py: found none of the expected nodes; is this a MediaTek device tree?')


if __name__ == '__main__':
    main()
