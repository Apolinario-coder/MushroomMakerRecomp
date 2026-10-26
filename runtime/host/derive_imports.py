"""Read imported symbol names from the user's converted main ELF."""
from pathlib import Path
import struct
import sys

def extract(path):
    data = path.read_bytes()
    offset = 0x1000
    mod = data.find(b'MOD0', offset, offset+0x1000)
    if mod < 0:
        raise ValueError('The executable has no MOD0 header.')
    pos = mod + struct.unpack_from('<i', data, mod+4)[0]
    dynamic = {}
    while True:
        tag, value = struct.unpack_from('<QQ', data, pos)
        pos += 16
        if not tag:
            break
        dynamic.setdefault(tag, value)
    count = (dynamic[5]-dynamic[6])//24
    symbols = []
    for i in range(count):
        name, info, other, section, value, size = struct.unpack_from('<IBBHQQ', data, offset+dynamic[6]+24*i)
        start = offset+dynamic[5]+name
        text = data[start:data.index(b'\0', start)].decode('latin-1')
        if text and not section:
            symbols.append(text)
    return sorted(set(symbols))

if __name__ == '__main__':
    Path(sys.argv[2]).write_text('\n'.join(extract(Path(sys.argv[1])))+'\n')
