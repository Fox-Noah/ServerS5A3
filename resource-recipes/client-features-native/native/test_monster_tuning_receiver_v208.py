"""Execute the candidate receiver and rate lookup in a bounded x86 emulator."""

import argparse
import struct

import pefile
from unicorn import UC_ARCH_X86, UC_MODE_32, Uc
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX,
                               UC_X86_REG_EDX, UC_X86_REG_ESI, UC_X86_REG_EDI,
                               UC_X86_REG_EBP, UC_X86_REG_ESP, UC_X86_REG_EFLAGS,
                               UC_X86_REG_EIP)

from build_monster_tuning_v208 import ORIGINAL_CALLBACK, REGISTRATION_POINTER

CURSOR = 0x057F9AD0
SCRATCH = 0x20000000
STACK = SCRATCH + 0x8000
PACKET = SCRATCH + 0x100
RATES = SCRATCH + 0x5000
RETURN = SCRATCH + 0x7000
GENERAL_REGISTERS = (UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX,
                     UC_X86_REG_EDX, UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EBP)


def test(candidate, payload):
    pe = pefile.PE(candidate, fast_load=True)
    library = pefile.PE(payload)
    image_base = pe.OPTIONAL_HEADER.ImageBase
    exports = {item.name: library.OPTIONAL_HEADER.ImageBase + item.address
               for item in library.DIRECTORY_ENTRY_EXPORT.symbols}
    callback, = struct.unpack("<I", pe.get_data(REGISTRATION_POINTER - image_base, 4))
    assert callback == exports[b"MonsterTuningStartMapV208"]
    lookup = exports[b"CopyMonsterTuningRatesV208"]
    u = Uc(UC_ARCH_X86, UC_MODE_32)
    u.mem_map(image_base, pe.OPTIONAL_HEADER.SizeOfImage)
    for section in pe.sections:
        if section.Name.rstrip(b"\0") in (b".mtcode", b".mtdata"):
            u.mem_write(image_base + section.VirtualAddress, section.get_data())
    u.mem_map(SCRATCH, 0x10000)

    def receive(body, expected_native_length):
        u.mem_write(PACKET, body)
        u.mem_write(CURSOR, struct.pack("<Ii", PACKET, len(body)))
        u.mem_write(STACK, struct.pack("<I", RETURN))
        registers = {reg: 0x100 + index for index, reg in enumerate(GENERAL_REGISTERS)}
        registers.update({UC_X86_REG_ESP: STACK, UC_X86_REG_EFLAGS: 0x202})
        for reg, value in registers.items():
            u.reg_write(reg, value)
        u.emu_start(callback, ORIGINAL_CALLBACK, count=100_000)
        assert u.reg_read(UC_X86_REG_EIP) == ORIGINAL_CALLBACK
        assert all(u.reg_read(reg) == value for reg, value in registers.items())
        assert tuple(struct.unpack("<Ii", u.mem_read(CURSOR, 8))) == (PACKET, expected_native_length)

    def rates_at(level):
        u.reg_write(UC_X86_REG_ESP, STACK)
        u.mem_write(STACK, struct.pack("<III", RETURN, level, RATES))
        u.emu_start(lookup, RETURN, count=10_000)
        assert u.reg_read(UC_X86_REG_EIP) == RETURN
        return struct.unpack("<6d", u.mem_read(RATES, 48))

    base = bytes(16)
    scaled = (2.0, 3.0, 4.0, 0.5, 1.25, 0.0)
    neutral = (1.0,) * 6
    rows = struct.pack("<BB6dBB6d", 0, 87, *scaled, 88, 255, *neutral)
    extension = rows + struct.pack("<I4s", len(rows), b"MAT1")
    receive(base + extension, len(base))
    assert rates_at(87) == scaled and rates_at(88) == neutral
    receive(base, len(base))
    assert rates_at(87) == neutral
    receive(base + extension, len(base))
    malformed = base + rows + struct.pack("<I4s", len(rows) + 1, b"MAT1")
    receive(malformed, len(malformed))
    assert rates_at(87) == neutral


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate", required=True)
    parser.add_argument("--payload", required=True)
    args = parser.parse_args()
    test(args.candidate, args.payload)
    print("V208 START_MAP callback handoff and all six native rate lookups passed.")
