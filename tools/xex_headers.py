"""Dump XEX2 header metadata for triage (read-only; prints metadata only, never code).

Usage:
    py tools/xex_headers.py <path to default.xex>

Why this exists: 360tools' xex_info.py mislabels several optional headers on WWE 2K14's
default.xex (e.g. entry point 0x600, 99 MB stack), so this tool follows Xenia's own rules.

Sources (Xenia Canary, canary_experimental branch):
  - Header keys and struct layouts: src/xenia/kernel/util/xex2_info.h
  - Inline-vs-offset rule:          src/xenia/cpu/xex_module.cc, XexModule::GetOptHeader
      (key & 0xFF) == 0x00 -> the value itself
      (key & 0xFF) == 0x01 -> the value itself (pointer to the inline dword)
      otherwise           -> value is a file offset to the header data
"""

import struct
import sys

HEADER_KEYS = {
    0x000002FF: "RESOURCE_INFO",
    0x000003FF: "FILE_FORMAT_INFO",
    0x000005FF: "DELTA_PATCH_DESCRIPTOR",
    0x00000405: "BASE_REFERENCE",
    0x00004304: "DISC_PROFILE_ID",
    0x000080FF: "BOUNDING_PATH",
    0x00008105: "DEVICE_ID",
    0x00010001: "ORIGINAL_BASE_ADDRESS",
    0x00010100: "ENTRY_POINT",
    0x00010201: "IMAGE_BASE_ADDRESS",
    0x000103FF: "IMPORT_LIBRARIES",
    0x00018002: "CHECKSUM_TIMESTAMP",
    0x00018102: "ENABLED_FOR_CALLCAP",
    0x00018200: "ENABLED_FOR_FASTCAP",
    0x000183FF: "ORIGINAL_PE_NAME",
    0x000200FF: "STATIC_LIBRARIES",
    0x00020104: "TLS_INFO",
    0x00020200: "DEFAULT_STACK_SIZE",
    0x00020301: "DEFAULT_FILESYSTEM_CACHE_SIZE",
    0x00020401: "DEFAULT_HEAP_SIZE",
    0x00028002: "PAGE_HEAP_SIZE_AND_FLAGS",
    0x00030000: "SYSTEM_FLAGS",
    0x00030100: "SYSTEM_FLAGS_32",
    0x00030200: "SYSTEM_FLAGS_64",
    0x00040006: "EXECUTION_INFO",
    0x00040201: "TITLE_WORKSPACE_SIZE",
    0x00040310: "GAME_RATINGS",
    0x00040404: "LAN_KEY",
    0x000405FF: "XBOX360_LOGO",
    0x000406FF: "MULTIDISC_MEDIA_IDS",
    0x000407FF: "ALTERNATE_TITLE_IDS",
    0x00040801: "ADDITIONAL_TITLE_MEMORY",
    0x00E10402: "EXPORTS_BY_NAME",
}


def u32(data, off):
    return struct.unpack_from(">I", data, off)[0]


def version_str(v):
    # xex2_version bitfield: major:4 minor:4 build:16 qfe:8 (most significant first).
    return f"{v >> 28}.{(v >> 24) & 0xF}.{(v >> 8) & 0xFFFF}.{v & 0xFF}"


def sized_string(data, off):
    size = u32(data, off)
    return data[off + 4 : off + size].split(b"\0")[0].decode("ascii", "replace")


def main(path):
    data = open(path, "rb").read()
    magic = data[0:4]
    if magic != b"XEX2":
        sys.exit(f"Not an XEX2 file (magic {magic!r})")

    module_flags, pe_offset, _reserved, security_offset, header_count = struct.unpack_from(">IIIII", data, 4)
    image_size = u32(data, security_offset + 4)

    print(f"File:              {path}")
    print(f"File size:         {len(data):,} bytes")
    print(f"Module flags:      0x{module_flags:08X}")
    print(f"PE data offset:    0x{pe_offset:X}")
    print(f"Image size:        0x{image_size:X} ({image_size / (1024 * 1024):.1f} MB)  [security info +0x4]")
    print(f"Optional headers:  {header_count}")
    print()

    headers = {}
    print("  key         name                            raw value")
    for i in range(header_count):
        key, value = struct.unpack_from(">II", data, 24 + i * 8)
        headers[key] = value
        kind = "inline" if (key & 0xFF) in (0x00, 0x01) else "offset"
        print(f"  0x{key:08X}  {HEADER_KEYS.get(key, '(unknown)'):30}  0x{value:08X} ({kind})")
    print()

    def get(key):
        return headers.get(key)

    print("Triage summary")
    base = get(0x00010201)
    entry = get(0x00010100)
    print(f"  Image base address:  {f'0x{base:08X}' if base is not None else '(absent)'}")
    print(f"  Entry point:         {f'0x{entry:08X}' if entry is not None else '(absent)'}")
    if base is not None:
        print(f"  Image range:         0x{base:08X} - 0x{base + image_size:08X}")

    off = get(0x000183FF)
    if off is not None:
        print(f"  Original PE name:    {sized_string(data, off)}")

    off = get(0x00040006)
    if off is not None:
        media_id, ver, base_ver, title_id = struct.unpack_from(">IIII", data, off)
        platform, exe_type, disc_no, disc_count = struct.unpack_from(">BBBB", data, off + 0x10)
        savegame_id = u32(data, off + 0x14)
        print(f"  Title ID:            {title_id:08X}")
        print(f"  Media ID:            {media_id:08X}")
        print(f"  Version:             {version_str(ver)} (base {version_str(base_ver)})")
        print(f"  Disc:                {disc_no} of {disc_count}")
        print(f"  Platform / exe type: {platform} / {exe_type}")
        print(f"  Savegame ID:         {savegame_id:08X}")

    off = get(0x000103FF)
    if off is not None:
        string_table_size = u32(data, off + 4)
        lib_count = u32(data, off + 8)
        names = data[off + 12 : off + 12 + string_table_size].split(b"\0")
        names = [n.decode("ascii", "replace") for n in names if n]
        print(f"  Import libraries:    {lib_count}: {', '.join(names)}")

    off = get(0x000200FF)
    if off is not None:
        size = u32(data, off)
        print("  Static libraries (name, major.minor.build.qfe):")
        for p in range(off + 4, off + size, 16):
            name = data[p : p + 8].split(b"\0")[0].decode("ascii", "replace")
            major, minor, build, qfe = struct.unpack_from(">HHHH", data, p + 8)
            print(f"    {name:8} {major}.{minor}.{build}.{qfe & 0xFF}")

    for key, label in ((0x00020200, "Default stack size"), (0x00020401, "Default heap size"),
                       (0x00040201, "Title workspace size"), (0x00040801, "Additional title memory")):
        if get(key) is not None:
            print(f"  {label + ':':21}0x{get(key):X}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
