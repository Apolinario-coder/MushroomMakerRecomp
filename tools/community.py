"""Validate a downloaded course bundle; no server or database code."""
import base64
import struct
LIMITS = {"data": 0x5C000, "thumbnail": 512 * 1024, "replay": 512 * 1024}

def validate(bundle):
    if not isinstance(bundle, dict) or set(bundle) != {"title", "author", "files"}:
        raise ValueError("Expected title, author and files")
    for key, maximum in (("title", 64), ("author", 64)):
        value = bundle[key]
        if not isinstance(value, str) or not value.strip() or len(value) > maximum or any(ord(c) < 32 for c in value):
            raise ValueError("Invalid " + key)
    files = bundle["files"]
    if not isinstance(files, dict) or not {"data", "thumbnail"} <= files.keys() or files.keys() - LIMITS.keys():
        raise ValueError("Course data and thumbnail required; only replay is optional")
    result = {}
    for name, value in files.items():
        if not isinstance(value, str):
            raise ValueError("Invalid base64 file")
        try:
            raw = base64.b64decode(value, validate=True)
        except (ValueError, TypeError) as error:
            raise ValueError("Invalid base64 file") from error
        if not raw or len(raw) > LIMITS[name]:
            raise ValueError("Invalid file size: " + name)
        if base64.b64encode(raw).decode("ascii") != value:
            raise ValueError("Expected canonical base64")
        result[name] = raw
    data = result["data"]
    # The local 1.0.0 build writes flags=0; later documented files also use flags=1.
    if len(data) != 0x5C000 or struct.unpack_from("<IH", data) != (1, 16) or struct.unpack_from("<H", data, 6)[0] not in (0, 1) or data[12:16] != b"SCDL":
        raise ValueError("Expected an encrypted SMM2 .bcd (0x5C000 bytes, SCDL header)")
    # shortcut: encrypted objects are not validated; keep uploads curated until a semantic validator is available.
    return result
