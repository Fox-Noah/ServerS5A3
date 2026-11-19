"""Build the v208 START_MAP receiver candidate without replacing the client."""

import argparse
import hashlib
from pathlib import Path
import struct

import pefile

V208_SHA256 = "5ce2ce3f01dacd05a6ce542d088911cc4fceb0f44ffa85dfa3a0886339e7cb5c"
FINGERPRINTS = {
    0x02001F81: "6a0068808dff016a1d8bcee80f235200",
    0x01FF8D80: "558bec6aff6845570f0464a1000000005081ec",
    0x037E82E0: "558beca1d09a7f058a088b550840ff0dd49a7f05",
}


def verify(path: Path) -> None:
    image = path.read_bytes()
    if hashlib.sha256(image).hexdigest() != V208_SHA256:
        raise ValueError("START_MAP adapter requires the exact v208 client")
    pe = pefile.PE(data=image, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    if pe.FILE_HEADER.Machine != 0x14C or base != 0x00400000:
        raise ValueError("Unexpected client architecture or preferred image base")
    for address, fingerprint in FINGERPRINTS.items():
        expected = bytes.fromhex(fingerprint)
        if pe.get_data(address - base, len(expected)) != expected:
            raise ValueError(f"Unexpected native code at VA 0x{address:08x}")

PAYLOAD_BASE = 0x06F90000
REGISTRATION_POINTER = 0x02001F84
ORIGINAL_CALLBACK = 0x01FF8D80
SPAWN_PATCH = 0x02628EDD
ORIGINAL_SPAWN = bytes.fromhex("8b068b9034040000")


def require(condition, description):
    if not condition:
        raise ValueError(description)


def align(value, boundary):
    return (value + boundary - 1) // boundary * boundary


def build(source, payload):
    verify(source)
    image = source.read_bytes()
    original = pefile.PE(data=image)
    library = pefile.PE(str(payload))
    base = original.OPTIONAL_HEADER.ImageBase
    require(library.OPTIONAL_HEADER.ImageBase == PAYLOAD_BASE, "Wrong payload base")
    require(library.FILE_HEADER.Machine == 0x14C, "Payload must be x86")
    require(all(not library.OPTIONAL_HEADER.DATA_DIRECTORY[index].Size
                for index in (1, 4, 5, 9, 12)), "Payload must be fixed and import-free")
    require(len(library.sections) == 2, "Expected code and data payload sections")
    exports = {item.name: PAYLOAD_BASE + item.address
               for item in library.DIRECTORY_ENTRY_EXPORT.symbols}
    callback = exports[b"MonsterTuningStartMapV208"]
    code = library.sections[0]
    require(code.VirtualAddress <= callback - PAYLOAD_BASE < code.VirtualAddress + code.SizeOfRawData,
            "Receiver export is outside payload code")
    patch_offset = original.get_offset_from_rva(REGISTRATION_POINTER - base)
    out = bytearray(image)
    require(struct.unpack_from("<I", out, patch_offset)[0] == ORIGINAL_CALLBACK,
            "START_MAP registration is different")
    struct.pack_into("<I", out, patch_offset, callback)

    spawn = exports[b"MonsterTuningSpawnV208"]
    require(code.VirtualAddress <= spawn - PAYLOAD_BASE < code.VirtualAddress + code.SizeOfRawData,
            "Spawn export is outside payload code")
    spawn_offset = original.get_offset_from_rva(SPAWN_PATCH - base)
    require(out[spawn_offset:spawn_offset + len(ORIGINAL_SPAWN)] == ORIGINAL_SPAWN,
            "Monster spawn code is different")
    spawn_jump = b"\xe9" + struct.pack("<i", spawn - SPAWN_PATCH - 5) + b"\x90" * 3
    out[spawn_offset:spawn_offset + len(spawn_jump)] = spawn_jump

    # Three contiguous RX sections can share one header. Their mapped bytes
    # and per-page protections are preserved.
    sections = list(original.sections)
    start = next(i for i, section in enumerate(sections)
                 if section.Name.rstrip(b"\0") == b".mailrg")
    merged = sections[start:start + 3]
    require([s.Name.rstrip(b"\0") for s in merged] == [b".mailrg", b".emright", b".wk855"],
            "Unexpected source section layout")
    require(all(s.Characteristics == 0x60000020 and s.Misc_VirtualSize <= s.SizeOfRawData
                for s in merged), "Section protections or padding differ")
    for first, second in zip(merged, merged[1:]):
        require(first.VirtualAddress + first.SizeOfRawData == second.VirtualAddress and
                first.PointerToRawData + first.SizeOfRawData == second.PointerToRawData,
                "Source sections are not contiguous")
    headers = [bytearray(s.__pack__()) for s in sections]
    merged_size = sum(s.SizeOfRawData for s in merged)
    struct.pack_into("<I", headers[start], 8, merged_size)
    struct.pack_into("<I", headers[start], 16, merged_size)
    del headers[start + 1:start + 3]

    opt = original.OPTIONAL_HEADER
    first_payload_rva = PAYLOAD_BASE - base + code.VirtualAddress
    last = sections[-1]
    require(last.Name.rstrip(b"\0") == b".ctnull" and
            last.VirtualAddress + last.SizeOfRawData == opt.SizeOfImage,
            "Unexpected final source section")
    require(first_payload_rva >= opt.SizeOfImage, "Payload overlaps source image")
    # The Windows image loader expects adjacent aligned virtual section ranges.
    # Extend the final source section with zero-filled virtual padding.
    struct.pack_into("<I", headers[-1], 8, first_payload_rva - last.VirtualAddress)
    added_code = added_data = 0
    image_end = first_payload_rva
    for descriptor, name, flags in zip(library.sections, (b".mtcode", b".mtdata"),
                                       (0x60000020, 0xc0000040)):
        require(descriptor.Characteristics == flags, "Unexpected payload section protections")
        rva = PAYLOAD_BASE - base + descriptor.VirtualAddress
        require(rva == image_end and rva % opt.SectionAlignment == 0, "Payload sections overlap")
        raw = align(len(out), opt.FileAlignment)
        content = descriptor.get_data()
        size = align(len(content), opt.FileAlignment)
        out.extend(bytes(raw - len(out)))
        out.extend(content)
        out.extend(bytes(size - len(content)))
        headers.append(struct.pack("<8sIIIIIIHHI", name, descriptor.Misc_VirtualSize,
                                   rva, size, raw, 0, 0, 0, 0, flags))
        image_end = align(rva + max(size, descriptor.Misc_VirtualSize), opt.SectionAlignment)
        if flags & 0x20:
            added_code += size
        else:
            added_data += size
    require(len(headers) == len(sections), "Unexpected section-header count")
    for index, header in enumerate(headers):
        offset = sections[0].get_file_offset() + index * 40
        out[offset:offset + 40] = header
    offset = opt.get_file_offset()
    struct.pack_into("<II", out, offset + 4, opt.SizeOfCode + added_code,
                     opt.SizeOfInitializedData + added_data)
    struct.pack_into("<I", out, offset + 56, image_end)
    struct.pack_into("<I", out, offset + 64, 0)
    patched = pefile.PE(data=bytes(out))
    struct.pack_into("<I", out, offset + 64, patched.generate_checksum())
    for section in sections:
        expected = bytearray(section.get_data())
        local = patch_offset - section.PointerToRawData
        if 0 <= local <= len(expected) - 4:
            struct.pack_into("<I", expected, local, callback)
        local = spawn_offset - section.PointerToRawData
        if 0 <= local <= len(expected) - len(spawn_jump):
            expected[local:local + len(spawn_jump)] = spawn_jump
        require(patched.get_data(section.VirtualAddress, len(expected)) == expected,
                f"Unexpected mapped change in {section.Name!r}")
    for section in library.sections:
        rva = PAYLOAD_BASE - base + section.VirtualAddress
        require(patched.get_data(rva, section.SizeOfRawData) == section.get_data(),
                "Payload section failed readback")
    return out


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", type=Path, required=True)
    parser.add_argument("--payload", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    data = build(args.base, args.payload)
    with args.output.open("xb") as output:
        output.write(data)
