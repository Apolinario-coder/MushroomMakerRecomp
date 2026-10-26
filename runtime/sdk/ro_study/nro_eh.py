#!/usr/bin/env python3
"""Exception tables of an NRO module (.eh_frame_hdr -> FDEs -> LSDA call-site tables).

  python nro_eh.py <file.nro> <base hex> [addr hex ...]   LSDA of the functions containing the addresses (all with -a)
  as a module: parse(path, base) -> list of dicts {start, end, lsda, personality, sites:[(start, len, lpad, action)],
               actions: {action index: [type filter, ...]}, types: {filter: type_info address}}
Addresses are guest addresses (file offset + base).
"""
import struct, sys


def uleb(d, o):
    r = s = 0
    while True:
        b = d[o]; o += 1
        r |= (b & 0x7f) << s; s += 7
        if not b & 0x80: return r, o


def sleb(d, o):
    r = s = 0
    while True:
        b = d[o]; o += 1
        r |= (b & 0x7f) << s; s += 7
        if not b & 0x80:
            if b & 0x40: r -= 1 << s
            return r, o


def enc(d, o, e, base=0):
    """read a DW_EH_PE encoded value at file offset o; returns (value as file offset/abs, next offset)"""
    if e == 0xff: return None, o
    fmt = e & 0x0f; app = e & 0x70
    start = o
    if fmt == 0x00: v, = struct.unpack_from('<Q', d, o); o += 8
    elif fmt == 0x01: v, o = uleb(d, o)
    elif fmt == 0x02: v, = struct.unpack_from('<H', d, o); o += 2
    elif fmt == 0x03: v, = struct.unpack_from('<I', d, o); o += 4
    elif fmt == 0x04: v, = struct.unpack_from('<Q', d, o); o += 8
    elif fmt == 0x09: v, o = sleb(d, o)
    elif fmt == 0x0a: v, = struct.unpack_from('<h', d, o); o += 2
    elif fmt == 0x0b: v, = struct.unpack_from('<i', d, o); o += 4
    elif fmt == 0x0c: v, = struct.unpack_from('<q', d, o); o += 8
    else: raise ValueError('encoding %x' % e)
    if v == 0 and fmt != 0x01: return 0, o
    if app == 0x10: v += start
    elif app == 0x30: v += base
    if e & 0x80: v = struct.unpack_from('<Q', d, v)[0] if 0 <= v < len(d) - 8 else 0     # indirect (unrelocated: file value)
    return v, o


def parse(path, base):
    d = open(path, 'rb').read()
    mod0 = struct.unpack_from('<I', d, 4)[0]
    hdr = mod0 + struct.unpack_from('<i', d, mod0 + 0x10)[0]
    ver, ptr_enc, cnt_enc, tab_enc = d[hdr:hdr + 4]
    o = hdr + 4
    eh_frame, o = enc(d, o, ptr_enc, hdr)
    count, o = enc(d, o, cnt_enc, hdr)
    out = []
    cies = {}
    for i in range(count):
        loc, o = enc(d, o, tab_enc, hdr)
        fde, o = enc(d, o, tab_enc, hdr)
        length, cid = struct.unpack_from('<Ii', d, fde)
        cie = fde + 4 - cid
        if cie not in cies:
            p = cie + 9
            aug = d[p:d.index(b'\0', p)].decode(); p += len(aug) + 1
            _, p = uleb(d, p); _, p = sleb(d, p); _, p = uleb(d, p)
            info = {'lsda_enc': 0xff, 'fde_enc': 0, 'personality': None}
            if aug.startswith('z'):
                _, p = uleb(d, p)
                for ch in aug[1:]:
                    if ch == 'L': info['lsda_enc'] = d[p]; p += 1
                    elif ch == 'R': info['fde_enc'] = d[p]; p += 1
                    elif ch == 'P':
                        pe = d[p]; p += 1
                        info['personality'], p = enc(d, p, pe)
            cies[cie] = info
        ci = cies[cie]
        p = fde + 8
        start, p = enc(d, p, ci['fde_enc'])
        size, p = enc(d, p, ci['fde_enc'] & 0x0f)
        _, p2 = uleb(d, p)
        lsda = None
        if ci['lsda_enc'] != 0xff:
            lsda, _ = enc(d, p2, ci['lsda_enc'])
        f = {'start': start + base, 'end': start + size + base, 'lsda': lsda + base if lsda else 0, 'sites': [], 'actions': {}, 'types': {}}
        if lsda:
            q = lsda
            lp_enc = d[q]; q += 1
            lp_start = start
            if lp_enc != 0xff: lp_start, q = enc(d, q, lp_enc)
            tt_enc = d[q]; q += 1
            tt = None
            if tt_enc != 0xff:
                off, q = uleb(d, q); tt = q + off
            cs_enc = d[q]; q += 1
            cs_len, q = uleb(d, q)
            cs_end = q + cs_len
            while q < cs_end:
                s, q = enc(d, q, cs_enc & 0x0f); l, q = enc(d, q, cs_enc & 0x0f); lp, q = enc(d, q, cs_enc & 0x0f); act, q = uleb(d, q)
                f['sites'].append((start + s + base, l, (lp_start + lp + base) if lp else 0, act))
                a = act
                if a and a not in f['actions']:
                    chain = []
                    ap = cs_end + a - 1
                    while True:
                        filt, n1 = sleb(d, ap)
                        nxt, _ = sleb(d, n1)
                        chain.append(filt)
                        if filt > 0 and tt is not None:
                            sz = {0x03: 4, 0x0b: 4, 0x00: 8, 0x04: 8, 0x0c: 8}.get(tt_enc & 0x0f, 4)
                            tp = tt - filt * sz
                            v, _ = enc(d, tp, tt_enc & 0x7f)                    # address of the GOT slot / type_info (file offset)
                            f['types'][filt] = (v + base if v else 0, bool(tt_enc & 0x80))
                        if nxt == 0: break
                        ap = n1 + nxt
                    f['actions'][a] = chain
        out.append(f)
    return out


if __name__ == '__main__':
    path, base = sys.argv[1], int(sys.argv[2], 16)
    fns = parse(path, base)
    with_lp = [f for f in fns if any(s[2] for s in f['sites'])]
    print('%d FDEs, %d with an LSDA, %d with landing pads, %d landing pads with a catch (action != 0)' % (
        len(fns), sum(1 for f in fns if f['lsda']), len(with_lp), sum(1 for f in fns for s in f['sites'] if s[2] and s[3])))
    for a in sys.argv[3:]:
        if a == '-a':
            sel = [f for f in fns if any(s[2] and s[3] for s in f['sites'])]
        else:
            x = int(a, 16)
            sel = [f for f in fns if f['start'] <= x < f['end']]
        for f in sel:
            print('function %x..%x lsda %x' % (f['start'], f['end'], f['lsda']))
            for s in f['sites']:
                print('   calls in %x..%x -> landing pad %x action %d %s' % (s[0], s[0] + s[1], s[2], s[3], f['actions'].get(s[3], '')))
            for k, v in f['types'].items():
                print('   type filter %d: %s at %x' % (k, 'GOT slot' if v[1] else 'type_info', v[0]))
