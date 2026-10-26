#!/usr/bin/env python3
"""
nso2elf.py - Converte um NSO0 (Nintendo Switch Object) do Horizon OS
num ELF64 ARM64 valido, descomprimindo as secoes (LZ4 block).

Dependencia: nenhuma (LZ4 block implementado em Python puro).

Uso:
    python nso2elf.py <arquivo.nso> <saida.elf>
"""
import struct
import sys
import os


# ---------------------------------------------------------------- LZ4 block
def lz4_block_decompress(src: bytes, dst_size: int) -> bytes:
    dst = bytearray()
    i, n = 0, len(src)
    while i < n:
        token = src[i]; i += 1
        # literais
        lit = token >> 4
        if lit == 15:
            while True:
                b = src[i]; i += 1
                lit += b
                if b != 255:
                    break
        dst += src[i:i + lit]
        i += lit
        if i >= n:
            break
        # offset (2 bytes LE)
        offset = src[i] | (src[i + 1] << 8); i += 2
        mlen = token & 0x0F
        if mlen == 15:
            while True:
                b = src[i]; i += 1
                mlen += b
                if b != 255:
                    break
        mlen += 4
        start = len(dst) - offset
        if start < 0:
            raise ValueError("LZ4: offset invalido (%d > %d)" % (offset, len(dst)))
        for j in range(mlen):
            dst.append(dst[start + j])
    if dst_size and len(dst) != dst_size:
        # tamanho divergente nao e fatal; reportamos
        pass
    return bytes(dst)


# ------------------------------------------------------------------- NSO
NSO_MAGIC = b"NSO0"
SECTIONS = ("text", "rodata", "data")


def parse_nso(path: str):
    data = open(path, "rb").read()
    if data[:4] != NSO_MAGIC:
        raise ValueError("nao e um NSO0: %r" % data[:4])
    version, _reserved, flags = struct.unpack_from("<III", data, 0x04)

    info = {}
    for idx, name in enumerate(SECTIONS):
        base = 0x10 + idx * 0x10
        file_off, mem_off, size, _r = struct.unpack_from("<IIII", data, base)
        info[name] = {"file_off": file_off, "mem_off": mem_off, "size": size}
    bss_size = struct.unpack_from("<I", data, 0x3C)[0]
    mod0_off = struct.unpack_from("<I", data, 0x48)[0]

    # limite de cada secao comprimida no arquivo = offset da proxima secao
    order = [s for s in SECTIONS]
    file_end = len(data)
    for idx in range(len(order) - 1, -1, -1):
        name = order[idx]
        s = info[name]
        compressed = bool(flags & (1 << idx))
        if compressed:
            comp_end = info[order[idx + 1]]["file_off"] if idx + 1 < len(order) else file_end
            comp_len = comp_end - s["file_off"]
            raw = data[s["file_off"]:s["file_off"] + comp_len]
            s["bytes"] = lz4_block_decompress(raw, s["size"])
            s["stored"] = "lz4"
        else:
            s["bytes"] = data[s["file_off"]:s["file_off"] + s["size"]]
            s["stored"] = "raw"
    return {"data": data, "flags": flags, "sections": info,
            "bss_size": bss_size, "mod0_off": mod0_off}


# ------------------------------------------------------------------- ELF
EM_AARCH64 = 183
ET_EXEC = 2
PT_LOAD = 1
PT_DYNAMIC = 2
SHT_PROGBITS = 1
SHT_SYMTAB = 2
SHT_STRTAB = 3
SHT_DYNAMIC = 6
SHT_NOBITS = 8
SHF_WRITE = 0x1
SHF_ALLOC = 0x2
SHF_EXECINSTR = 0x4
PF_X, PF_W, PF_R = 0x1, 0x2, 0x4


def align_up(x, a):
    return (x + a - 1) & ~(a - 1)


def build_elf(nso, base=0x1000):
    """VA = endereco relativo ao modulo; offset no arquivo = base + VA (o FILE_OFF do runtime).

    hostrt.c le o arquivo inteiro a partir de FILE_OFF para GUEST_BASE, e sdklift_rt.c copia
    cada PT_LOAD com memcpy(img + p_vaddr, file + p_offset, ...): as duas convencoes coincidem
    quando p_offset = FILE_OFF + p_vaddr.
    """
    sec = nso["sections"]
    text, rodata, data = sec["text"], sec["rodata"], sec["data"]

    t_va, r_va, d_va = text["mem_off"], rodata["mem_off"], data["mem_off"]
    r_va = max(r_va, align_up(t_va + text["size"], 0x1000))
    d_va = max(d_va, align_up(r_va + rodata["size"], 0x1000))

    bss = nso["bss_size"] if 0 <= nso["bss_size"] < (256 << 20) else 0

    # imagem em enderecos de modulo, para achar o MOD0 e a .dynamic (que vive na secao .data)
    mem = bytearray(max(d_va + data["size"], d_va + bss))
    mem[t_va:t_va + text["size"]] = text["bytes"]
    mem[r_va:r_va + rodata["size"]] = rodata["bytes"]
    mem[d_va:d_va + data["size"]] = data["bytes"]

    # MOD0: o u32 em text+4 e o endereco (de modulo) do MOD0; os campos em +4/+8/+0xC/+0x10/+0x14
    # (dynamic, bss inicio/fim, eh_frame_hdr inicio/fim) sao offsets RELATIVOS ao proprio MOD0.
    mod0 = struct.unpack_from("<I", text["bytes"], 4)[0]
    if mem[mod0:mod0 + 4] != b"MOD0":
        raise ValueError("MOD0 nao encontrado em %#x" % mod0)
    dyn = mod0 + struct.unpack_from("<i", mem, mod0 + 4)[0]
    n = 0
    while struct.unpack_from("<q", mem, dyn + n * 16)[0] != 0:
        n += 1
    dyn_size = (n + 1) * 16
    bss_end = mod0 + struct.unpack_from("<i", mem, mod0 + 0xC)[0]
    d_memsz = max(data["size"], bss_end - d_va) if bss_end > d_va else data["size"] + bss

    shstr = b".text\0.rodata\0.data\0.bss\0.dynamic\0.shstrtab\0"
    name_off = {}
    o = 0
    for nm in (".text", ".rodata", ".data", ".bss", ".dynamic", ".shstrtab"):
        name_off[nm] = o
        o += len(nm) + 1

    # offsets do arquivo == base + VA (mantem p_offset % 0x1000 == p_vaddr % 0x1000)
    t_off, r_off, d_off = base + t_va, base + r_va, base + d_va
    shstr_off = align_up(d_off + data["size"], 8)
    shoff = align_up(shstr_off + len(shstr), 8)
    shnum = 7  # null + 5 secoes + shstrtab
    phnum = 4

    buf = bytearray(shoff + shnum * 64)
    buf[t_off:t_off + text["size"]] = text["bytes"]
    buf[r_off:r_off + rodata["size"]] = rodata["bytes"]
    buf[d_off:d_off + data["size"]] = data["bytes"]
    buf[shstr_off:shstr_off + len(shstr)] = shstr

    # ELF header
    ident = bytearray(16)
    ident[0:4] = b"\x7fELF"
    ident[4] = 2      # ELFCLASS64
    ident[5] = 1      # ELFDATA2LSB
    ident[6] = 1      # EV_CURRENT
    ident[7] = 0      # System V
    struct.pack_into("<16sHHIQQQIHHHHHH", buf, 0,
                     bytes(ident), ET_EXEC, EM_AARCH64, 1, 0,
                     0x40, shoff, 0, 64, 56, phnum, 64, shnum, 5)

    # program headers (o 4o, PT_DYNAMIC, e o que sdklift_rt.c le para relocar o SDK)
    ph = 0x40
    struct.pack_into("<IIQQQQQQ", buf, ph,
                     PT_LOAD, PF_R | PF_X, t_off, t_va, t_va,
                     text["size"], text["size"], 0x1000)
    struct.pack_into("<IIQQQQQQ", buf, ph + 56,
                     PT_LOAD, PF_R, r_off, r_va, r_va,
                     rodata["size"], rodata["size"], 0x1000)
    struct.pack_into("<IIQQQQQQ", buf, ph + 112,
                     PT_LOAD, PF_R | PF_W, d_off, d_va, d_va,
                     data["size"], d_memsz, 0x1000)
    struct.pack_into("<IIQQQQQQ", buf, ph + 168,
                     PT_DYNAMIC, PF_R | PF_W, base + dyn, dyn, dyn,
                     dyn_size, dyn_size, 8)

    def sh(i, name, typ, flags, addr, off, size, align_):
        struct.pack_into("<IIQQQQIIQQ", buf, shoff + i * 64,
                         name_off[name], typ, flags, addr, off, size,
                         0, 0, align_, 0)

    sh(0, ".text", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, t_va, t_off, text["size"], 0x1000)
    sh(1, ".rodata", SHT_PROGBITS, SHF_ALLOC, r_va, r_off, rodata["size"], 0x1000)
    sh(2, ".data", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, d_va, d_off, data["size"], 0x1000)
    sh(3, ".bss", SHT_NOBITS, SHF_ALLOC | SHF_WRITE, d_va + d_memsz - bss, d_off + data["size"], bss, 0x1000)
    sh(4, ".dynamic", SHT_DYNAMIC, SHF_ALLOC | SHF_WRITE, dyn, base + dyn, dyn_size, 8)
    sh(5, ".shstrtab", SHT_STRTAB, 0, 0, shstr_off, len(shstr), 1)

    return bytes(buf)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 1
    src, dst = sys.argv[1], sys.argv[2]
    nso = parse_nso(src)
    sec = nso["sections"]
    print("flags=0x%02X" % nso["flags"])
    for nm in SECTIONS:
        s = sec[nm]
        ok = "OK" if len(s["bytes"]) == s["size"] else "DIVERGENTE!"
        print("  %-7s file_off=%-10d mem_off=%-10d size=%-10d %-4s decoded=%-10d %s" %
              (nm, s["file_off"], s["mem_off"], s["size"], s["stored"],
               len(s["bytes"]), ok))
    print("  bss size=%d mod0_off=0x%X" % (nso["bss_size"], nso["mod0_off"]))
    elf = build_elf(nso)
    open(dst, "wb").write(elf)
    print("escrito: %s (%d bytes)" % (dst, len(elf)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
