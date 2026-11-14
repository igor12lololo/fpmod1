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

def dump_bytes(data, sections, rva, length=128):
    """Dump bytes at RVA"""
    raw = rva_to_raw(rva, sections)
    if raw is None or raw + length > len(data):
        return None
    return data[raw:raw+length]

def find_function_at_rva(data, sections, rva, max_len=200):
    """Get function bytes and try to identify unique pattern"""
    raw = rva_to_raw(rva, sections)
    if raw is None:
        return None
    # Get function bytes (up to max_len or next function)
    func_bytes = data[raw:raw+max_len]
    return func_bytes

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

    # Check the found functions at new locations
    print("=== Analyzing found function at 0xE55880 ===")
    func_bytes = find_function_at_rva(data, sections, 0xE55880, 200)
    if func_bytes:
        print(f"0xE55880: {format_hex(func_bytes, 128)}")

    print("\n=== Analyzing found function at 0xDCDA00 (ENDLESS_START) ===")
    func_bytes = find_function_at_rva(data, sections, 0xDCDA00, 200)
    if func_bytes:
        print(f"0xDCDA00: {format_hex(func_bytes, 128)}")

    # Search for SET_SPEED pattern more specifically
    print("\n=== Searching for SET_SPEED (unique pattern) ===")
    # Old 1.6.1 pattern: 48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 48 8B F9 8B DA
    # Let's search for the unique part: "8B DA" at the end (MOV EBX, EDX)
    set_speed_prefix = bytes([0x48, 0x89, 0x5C, 0x24])
    for idx in range(len(text_data) - 20):
        if text_data[idx:idx+4] == set_speed_prefix:
            # Check for the full pattern with wildcards
            if (text_data[idx+5] == 0x48 and text_data[idx+6] == 0x89 and text_data[idx+7] == 0x74 and
                text_data[idx+8] == 0x24 and text_data[idx+10] == 0x57 and
                text_data[idx+11] == 0x48 and text_data[idx+12] == 0x83 and text_data[idx+13] == 0xEC and
                text_data[idx+14] == 0x20 and text_data[idx+15] == 0x48 and text_data[idx+16] == 0x8B and
                text_data[idx+17] == 0xF9 and text_data[idx+18] == 0x8B and text_data[idx+19] == 0xDA):
                rva = text_rva + idx
                print(f"  SET_SPEED candidate at RVA 0x{rva:X}: {format_hex(text_data[idx:idx+64])}")

    # Search for PAUSE_ADD_REASON
    print("\n=== Searching for PAUSE_ADD_REASON (unique pattern) ===")
    # Old pattern: 48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 48 8B F9 48 8B F2 48 8B D9
    # Unique: ends with 48 8B D9 (MOV RBX, RCX)
    for idx in range(len(text_data) - 24):
        if (text_data[idx:idx+4] == bytes([0x48, 0x89, 0x5C, 0x24]) and
            text_data[idx+5] == 0x48 and text_data[idx+6] == 0x89 and text_data[idx+7] == 0x74 and
            text_data[idx+8] == 0x24 and text_data[idx+10] == 0x57 and
            text_data[idx+11] == 0x48 and text_data[idx+12] == 0x83 and text_data[idx+13] == 0xEC and
            text_data[idx+14] == 0x20 and text_data[idx+15] == 0x48 and text_data[idx+16] == 0x8B and
            text_data[idx+17] == 0xF9 and text_data[idx+18] == 0x48 and text_data[idx+19] == 0x8B and
            text_data[idx+20] == 0xF2 and text_data[idx+21] == 0x48 and text_data[idx+22] == 0x8B and
            text_data[idx+23] == 0xD9):
            rva = text_rva + idx
            print(f"  PAUSE_ADD_REASON candidate at RVA 0x{rva:X}: {format_hex(text_data[idx:idx+64])}")

    # Search for SELECT_MAP
    print("\n=== Searching for SELECT_MAP (unique pattern) ===")
    # Old pattern: 48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 48 8B F9 8B DA
    # Same as SET_SPEED? Let's check the old RVA content
    func_bytes = find_function_at_rva(data, sections, 0x1A8B880, 128)
    if func_bytes:
        print(f"  Old SELECT_MAP @ 0x1A8B880: {format_hex(func_bytes, 64)}")

    # Search for SELECT_MAP - look for "E8 31 50 39 FF" pattern from old code
    # That's CALL instruction - let's search for it
    call_pattern = bytes([0xE8, 0x31, 0x50, 0x39, 0xFF])
    for idx in range(len(text_data) - 5):
        if text_data[idx:idx+5] == call_pattern:
            rva = text_rva + idx
            print(f"  CALL pattern at RVA 0x{rva:X}: {format_hex(text_data[idx-16:idx+48])}")

    # Search for CHANGE_RESOURCE - old RVA content
    print("\n=== Old CHANGE_RESOURCE @ 0x168E340 ===")
    func_bytes = find_function_at_rva(data, sections, 0x168E340, 128)
    if func_bytes:
        print(f"  {format_hex(func_bytes, 64)}")

    # Search for PAUSE_ADD_REASON - old RVA content
    print("\n=== Old PAUSE_ADD_REASON @ 0xF6C530 ===")
    func_bytes = find_function_at_rva(data, sections, 0xF6C530, 128)
    if func_bytes:
        print(f"  {format_hex(func_bytes, 64)}")

    # Search for PAUSE_REMOVE_REASON - old RVA content
    print("\n=== Old PAUSE_REMOVE_REASON @ 0xF6CA80 ===")
    func_bytes = find_function_at_rva(data, sections, 0xF6CA80, 128)
    if func_bytes:
        print(f"  {format_hex(func_bytes, 64)}")

    # Search for NATIVE_SAVE - old RVA content
    print("\n=== Old NATIVE_SAVE @ 0x11B5F80 ===")
    func_bytes = find_function_at_rva(data, sections, 0x11B5F80, 128)
    if func_bytes:
        print(f"  {format_hex(func_bytes, 64)}")

    # Search for NATIVE_LOAD - old RVA content
    print("\n=== Old NATIVE_LOAD @ 0x11B5E30 ===")
    func_bytes = find_function_at_rva(data, sections, 0x11B5E30, 128)
    if func_bytes:
        print(f"  {format_hex(func_bytes, 64)}")

    # Look for unique function signatures by searching for specific instruction sequences
    print("\n=== Searching for unique function signatures ===")

    # Pattern for functions that access global time (likely around 0x2B6A710)
    # Look for MOV instruction loading from [RIP+0x2B6A710]
    time_access_pattern = bytes([0x48, 0x8B, 0x05])  # MOV RAX, [RIP+rel32]
    for idx in range(len(text_data) - 7):
        if text_data[idx:idx+3] == time_access_pattern:
            rel32 = struct.unpack('<i', text_data[idx+3:idx+7])[0]
            target_rva = text_rva + idx + 7 + rel32
            if 0x2B6A000 <= target_rva <= 0x2B6B000:
                rva = text_rva + idx
                print(f"  Global time access at Code RVA 0x{rva:X} -> Data RVA 0x{target_rva:X}: {format_hex(text_data[idx:idx+16])}")

    mm.close()
    f.close()