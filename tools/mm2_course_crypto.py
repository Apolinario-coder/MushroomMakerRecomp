"""SMM2 file integrity and version checks; public ENL/SEAD file format.

https://github.com/kinnay/Nintendo-File-Formats/tree/master/src/games/smm2
"""
import struct
import zlib
import secrets
import io
from cryptography.exceptions import InvalidSignature

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.cmac import CMAC
import mariover_keytables as tables


def keys(seed, table):
    state = list(struct.unpack('<4I', seed))
    if not any(state):
        raise ValueError('Estado criptográfico inválido')
    def rand(limit):
        x, y, z, w = state
        t = (x ^ (x << 11)) & 0xffffffff
        value = (w ^ (w >> 19) ^ t ^ (t >> 8)) & 0xffffffff
        state[:] = y, z, w, value
        return (value * limit) >> 32
    result = []
    for _ in range(2):
        key = bytearray()
        for _ in range(4):
            word = 0
            for _ in range(4):
                byte = (table[rand(len(table))] >> (rand(4) * 8)) & 255
                word = (word << 8) | byte
            key.extend(struct.pack('<I', word))
        result.append(bytes(key))
    return result


def decrypt(data, table, header=0):
    aes, mac = keys(data[-32:-16], table)
    cipher = Cipher(algorithms.AES(aes), modes.CBC(data[-48:-32])).decryptor()
    plain = cipher.update(data[header:-48]) + cipher.finalize()
    check = CMAC(algorithms.AES(mac))
    check.update(plain)
    try:
        check.verify(data[-16:])
    except InvalidSignature:
        raise ValueError('Integridade criptográfica da fase/miniatura inválida') from None
    return plain


def thumbnail(jpeg):
    from PIL import Image
    with Image.open(io.BytesIO(jpeg)) as image:
        if image.format != 'JPEG' or image.width > 2048 or image.height > 2048:
            raise ValueError('Miniatura inválida: esperado JPEG de até 2048×2048')
        image.load()
        if image.size != (640, 360) or len(jpeg) > 0x1bfd0:
            scaled = image.convert('RGB').resize((640, 360), Image.Resampling.LANCZOS)
            for quality in (80, 65, 50):
                output = io.BytesIO()
                scaled.save(output, format='JPEG', quality=quality)
                jpeg = output.getvalue()
                if len(jpeg) <= 0x1bfd0:
                    break
    if len(jpeg) > 0x1bfd0:
        raise ValueError('Miniatura excede o tamanho BTL')
    plain = jpeg.ljust(0x1bfd0, b'\0')
    seed, iv = secrets.token_bytes(16), secrets.token_bytes(16)
    aes, mac = keys(seed, tables.btl)
    cipher = Cipher(algorithms.AES(aes), modes.CBC(iv)).encryptor()
    encrypted = cipher.update(plain) + cipher.finalize()
    check = CMAC(algorithms.AES(mac))
    check.update(plain)
    return encrypted + iv + seed + check.finalize()


def check_course(data, game_version="1.0.0"):
    versions = {"1.0.0": (1, 0), "3.0.3": (0x3f, 5)}
    if game_version not in versions:
        raise ValueError("Versão do runtime desconhecida")
    if len(data) != 0x5c000 or struct.unpack_from('<IH', data) != (1, 16) or data[12:16] != b'SCDL':
        raise ValueError('Dados da fase inválidos: esperado arquivo BCD/SCDL')
    if struct.unpack_from('<H', data, 6)[0] not in (0, 1):
        raise ValueError('Formato BCD desconhecido')
    plain = decrypt(data, tables.bcd, 16)
    if zlib.crc32(plain) != struct.unpack_from('<I', data, 8)[0]:
        raise ValueError('CRC da fase inválido')
    physics = struct.unpack_from('<I', plain, 0x14)[0]
    clear_version = struct.unpack_from('<I', plain, 0x30)[0]
    physics_mask, clear_max = versions[game_version]
    if physics & ~physics_mask or clear_version > clear_max:
        raise ValueError(f"Fase requer versão de física posterior à suportada por {game_version}; não será aberta neste port.")
    if plain[0xf1:0xf3] not in (b'M1', b'M3', b'MW', b'WU', b'3W'):
        raise ValueError('Estilo de jogo desconhecido')
    # shortcut: bounds are checked, but object semantics require validation against the lifted game.
    limits = {0x1c: 2600, 0x20: 300, 0x24: 5, 0x28: 200, 0x2c: 10,
              0x30: 10, 0x34: 10, 0x3c: 4000, 0x40: 1500, 0x44: 300}
    for area in (0x200, 0x2e0e0):
        for offset, maximum in limits.items():
            if struct.unpack_from('<I', plain, area + offset)[0] > maximum:
                raise ValueError('Contagem de objetos da fase inválida')
    return plain
