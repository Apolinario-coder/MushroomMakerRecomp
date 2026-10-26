"""Fetch and validate one course for the original Course World worker."""
import argparse
import json
from pathlib import Path
import struct

import mariover
from mm2_course_crypto import check_course
from native_catalogue import cache_thumbnail, atomic_write, HEADER


def fetch(ident, data_id, cache, game_version="1.0.0"):
    ident = mariover.code(ident)
    metadata_path=cache/'metadata'/(str(data_id)+'.json')
    try:
        if metadata_path.stat().st_size>65536:raise ValueError('Cached metadata too large')
        metadata=json.loads(metadata_path.read_text(encoding='utf-8'))
        if not isinstance(metadata,dict):raise ValueError('Invalid cached metadata')
    except (OSError,ValueError):
        metadata=mariover.request(mariover.BASE, 'level_info/' + ident)
    if metadata.get('course_id') != ident or metadata.get('data_id') != data_id:
        raise ValueError('A API retornou outra fase')
    path = cache / f'{data_id}.bcd'
    data = path.read_bytes() if path.exists() else mariover.request(
        mariover.BASE, 'level_data/' + ident, maximum=0x5c000, binary=True)
    plain = check_course(data, game_version)
    if not path.exists():
        atomic_write(path,data)
    cache_thumbnail(metadata, cache)
    # The original worker checks the plaintext CRC before starting a course.
    bcd = data[:16] + plain + data[-48:]
    image = (cache / f'{data_id}.jpg').read_bytes()
    if len(image) != 0x1c000 or image[:2] != b'\xff\xd8':
        raise ValueError('Miniatura da fase indisponível')
    result = bytearray(0x78050)
    result[:0x1c000] = image
    result[0x1c000:0x78000] = bcd
    struct.pack_into('<Q', result, 0x78000, data_id)
    return bytes(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--code', required=True)
    parser.add_argument('--data-id', required=True, type=int)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument("--game-version", choices=("1.0.0", "3.0.3"), default="1.0.0")
    args = parser.parse_args()
    try:
        cache = Path(__file__).resolve().parents[1] / 'build/cache/mariover'
        cache.mkdir(parents=True, exist_ok=True)
        body = fetch(args.code, args.data_id, cache, args.game_version)
        packet = HEADER.pack(b'M2DL', 1, len(body), 0, b'') + body
    except Exception as exc:
        packet = HEADER.pack(b'M2DL', 1, 0, 1, str(exc).encode('utf-8')[:511])
    temporary = args.output.with_suffix('.partial')
    temporary.write_bytes(packet)
    temporary.replace(args.output)


if __name__ == '__main__':
    main()
