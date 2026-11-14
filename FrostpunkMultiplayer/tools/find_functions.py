#!/usr/bin/env python3
import sys
import mmap
import struct

def read_pe_sections(filepath):
    """Read PE file and return section info and data"""
    f = open(filepath, 'rb')
    mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    data = mm

    dos_header = data[:64]
    e_lfanew = struct.unpack('<I', dos_header[60:64])[0]
    coff = data[e_lfanew+4:e_lfanew+24]
    num_sections = struct.unpack('<H', coff[2:4])[0]
    opt_header_size = struct.unpack('<H', coff[16:18])[0]

    opt_offset = e_lfanew + 24
    section_header_offset = opt_offset + opt_header_size

    sections = {}
    for i in range(num_sections):
        sec_offset = section_header_offset + i * 40
        sec_name = data[sec_offset:sec_offset+8].rstrip(b'\0').decode('ascii', errors='ignore')
        v_rva = struct.unpack('<I', data[sec_offset+12:sec_offset+16])[0]
        v_size = struct.unpack('<I', data[sec_offset+8:sec_offset+12])[0]
        raw_ptr = struct.unpack('<I', data[sec_offset+20:sec_offset+24])[0]
        sections[sec_name] = {'rva': v_rva, 'size': v_size, 'raw': raw_ptr}

    return data, sections, f, mm

def rva_to_raw(rva, sections):
    """Convert RVA to file offset"""
    for name, sec in sections.items():
        if sec['rva'] <= rva < sec['rva'] + sec['size']:
            return sec['raw'] + (rva - sec['rva'])
    return None

def dump_bytes(data, sections, rva, length=64):
    """Dump bytes at RVA"""
    raw = rva_to_raw(rva, sections)
    if raw is None or raw + length > len(data):
        return None
    return data[raw:raw+length]

def format_hex(bytes_data, max_len=64):
    if not bytes_data:
        return "NONE"
    return ' '.join(f'{b:02X}' for b in bytes_data[:max_len])

if __name__ == '__main__':
    filepath = "C:/Program Files (x86)/Steam/steamapps/common/Frostpunk/Frostpunk.exe"
    data, sections, f, mm = read_pe_sections(filepath)

    text_sec = sections.get('.text', {})
    text_rva = text_sec['rva']
    text_raw = text_sec['raw']
    text_size = text_sec['size']
    text_data = data[text_raw:text_raw+text_size]

    print("=== Searching for SET_SPEED - look for '8B DA' (MOV EBX, EDX) after standard prologue ===")
    # Standard prologue: 48 89 5C 24 XX 48 89 74 24 XX 57 48 83 EC 20 48 8B F9
    # Then SET_SPEED has: 8B DA
    for idx in range(len(text_data) - 30):
        if (text_data[idx:idx+4] == bytes([0x48, 0x89, 0x5C, 0x24]) and
            text_data[idx+5] == 0x48 and text_data[idx+6] == 0x89 and text_data[idx+7] == 0x74 and
            text_data[idx+8] == 0x24 and text_data[idx+10] == 0x57 and
            text_data[idx+11] == 0x48 and text_data[idx+12] == 0x83 and text_data[idx+13] == 0xEC and
            text_data[idx+14] == 0x20 and text_data[idx+15] == 0x48 and text_data[idx+16] == 0x8B and
            text_data[idx+17] == 0xF9):
            # Check what comes after
            if idx + 18 < len(text_data):
                b1 = text_data[idx+18]
                b2 = text_data[idx+19]
                rva = text_rva + idx
                print(f"  Candidate at RVA 0x{rva:X}: next bytes = {b1:02X} {b2:02X} - {format_hex(text_data[idx:idx+32])}")

    print("\n=== Searching for PAUSE_ADD_REASON - look for '48 8B D9' (MOV RBX, RCX) after standard prologue ===")
    for idx in range(len(text_data) - 30):
        if (text_data[idx:idx+4] == bytes([0x48, 0x89, 0x5C, 0x24]) and
            text_data[idx+5] == 0x48 and text_data[idx+6] == 0x89 and text_data[idx+7] == 0x74 and
            text_data[idx+8] == 0x24 and text_data[idx+10] == 0x57 and
            text_data[idx+11] == 0x48 and text_data[idx+12] == 0x83 and text_data[idx+13] == 0xEC and
            text_data[idx+14] == 0x20 and text_data[idx+15] == 0x48 and text_data[idx+16] == 0x8B and
            text_data[idx+17] == 0xF9):
            # Check for 48 8B D9 at offset 18
            if idx + 21 < len(text_data):
                if text_data[idx+18] == 0x48 and text_data[idx+19] == 0x8B and text_data[idx+20] == 0xD9:
                    rva = text_rva + idx
                    print(f"  PAUSE_ADD_REASON candidate at RVA 0x{rva:X}: {format_hex(text_data[idx:idx+40])}")

    print("\n=== Searching for SELECT_MAP - look for unique pattern ===")
    # Old had: 48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 48 8B F9 8B DA
    # Same as SET_SPEED? Let's check both
    for idx in range(len(text_data) - 30):
        if (text_data[idx:idx+4] == bytes([0x48, 0x89, 0x5C, 0x24]) and
            text_data[idx+5] == 0x48 and text_data[idx+6] == 0x89 and text_data[idx+7] == 0x74 and
            text_data[idx+8] == 0x24 and text_data[idx+10] == 0x57 and
            text_data[idx+11] == 0x48 and text_data[idx+12] == 0x83 and text_data[idx+13] == 0xEC and
            text_data[idx+14] == 0x20 and text_data[idx+15] == 0x48 and text_data[idx+16] == 0x8B and
            text_data[idx+17] == 0xF9 and text_data[idx+18] == 0x8B and text_data[idx+19] == 0xDA):
            rva = text_rva + idx
            print(f"  SET_SPEED/SELECT_MAP candidate at RVA 0x{rva:X}: {format_hex(text_data[idx:idx+40])}")

    print("\n=== Searching for CHANGE_RESOURCE - look for '48 8B 09' (MOV RCX, [RCX]) after prologue ===")
    for idx in range(len(text_data) - 30):
        if (text_data[idx:idx+4] == bytes([0x48, 0x89, 0x5C, 0x24]) and
            text_data[idx+5] == 0x48 and text_data[idx+6] == 0x89 and text_data[idx+7] == 0x74 and
            text_data[idx+8] == 0x24 and text_data[idx+10] == 0x57 and
            text_data[idx+11] == 0x48 and text_data[idx+12] == 0x83 and text_data[idx+13] == 0xEC and
            text_data[idx+14] == 0x20 and text_data[idx+15] == 0x48 and text_data[idx+16] == 0x8B and
            text_data[idx+17] == 0xF9 and text_data[idx+18] == 0x48 and text_data[idx+19] == 0x8B and
            text_data[idx+20] == 0xF2):
            # Check for 48 8B 09 at offset 21
            if idx + 24 < len(text_data):
                if text_data[idx+21] == 0x48 and text_data[idx+22] == 0x8B and text_data[idx+23] == 0x09:
                    rva = text_rva + idx
                    print(f"  CHANGE_RESOURCE candidate at RVA 0x{rva:X}: {format_hex(text_data[idx:idx+40])}")

    print("\n=== Searching for NATIVE_SAVE/LOAD - look for standard prologue + specific patterns ===")
    # Both old had same prologue but different bodies
    for idx in range(len(text_data) - 30):
        if (text_data[idx:idx+4] == bytes([0x48, 0x89, 0x5C, 0x24]) and
            text_data[idx+5] == 0x48 and text_data[idx+6] == 0x89 and text_data[idx+7] == 0x74 and
            text_data[idx+8] == 0x24 and text_data[idx+10] == 0x57 and
            text_data[idx+11] == 0x48 and text_data[idx+12] == 0x83 and text_data[idx+13] == 0xEC and
            text_data[idx+14] == 0x20 and text_data[idx+15] == 0x48 and text_data[idx+16] == 0x8B and
            text_data[idx+17] == 0xF9 and text_data[idx+18] == 0x48 and text_data[idx+19] == 0x8B and
            text_data[idx+20] == 0xF2):
            rva = text_rva + idx
            # Print first 64 bytes to differentiate
            print(f"  NATIVE candidate at RVA 0x{rva:X}: {format_hex(text_data[idx:idx+64])}")

    print("\n=== Searching for PAUSE_REMOVE_REASON - look for standard prologue + specific body ===")
    for idx in range(len(text_data) - 30):
        if (text_data[idx:idx+4] == bytes([0x48, 0x89, 0x5C, 0x24]) and
            text_data[idx+5] == 0x48 and text_data[idx+6] == 0x89 and text_data[idx+7] == 0x74 and
            text_data[idx+8] == 0x24 and text_data[idx+10] == 0x57 and
            text_data[idx+11] == 0x48 and text_data[idx+12] == 0x83 and text_data[idx+13] == 0xEC and
            text_data[idx+14] == 0x20 and text_data[idx+15] == 0x48 and text_data[idx+16] == 0x8B and
            text_data[idx+17] == 0xF9 and text_data[idx+18] == 0x48 and text_data[idx+19] == 0x8B and
            text_data[idx+20] == 0xF2):
            # This matches CHANGE_RESOURCE too, need to differentiate
            rva = text_rva + idx
            # Check what's at offset 21
            b21 = text_data[idx+21] if idx+21 < len(text_data) else 0
            b22 = text_data[idx+22] if idx+22 < len(text_data) else 0
            b23 = text_data[idx+23] if idx+23 < len(text_data) else 0
            if b21 == 0x48 and b22 == 0x8B and b23 == 0x09:
                print(f"  CHANGE_RESOURCE/NATIVE at RVA 0x{rva:X}: {format_hex(text_data[idx:idx+64])}")
            else:
                print(f"  PAUSE_REMOVE/OTHER at RVA 0x{rva:X}: next=({b21:02X} {b22:02X} {b23:02X}) {format_hex(text_data[idx:idx+64])}")

    # Check the known function at 0xE55880 in detail
    print("\n=== Detailed analysis of function at 0xE55880 ===")
    func_bytes = dump_bytes(data, sections, 0xE55880, 200)
    if func_bytes:
        print(f"  {format_hex(func_bytes, 200)}")

    mm.close()
    f.close()