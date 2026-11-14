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

if __name__ == '__main__':
    filepath = "C:/Program Files (x86)/Steam/steamapps/common/Frostpunk/Frostpunk.exe"
    data, sections, f, mm = read_pe_sections(filepath)

    print("Sections:")
    for name, sec in sections.items():
        print(f"  {name}: RVA 0x{sec['rva']:X}-0x{sec['rva']+sec['size']:X}, Raw 0x{sec['raw']:X}-0x{sec['raw']+sec['size']:X}")

    # Check found function RVAs
    found_rvas = {
        "RVA_CHANGE_RESOURCE": 0xE55880,
        "RVA_PAUSE_REMOVE_REASON": 0xE55880,
        "RVA_NATIVE_SAVE": 0xE55880,
        "RVA_NATIVE_LOAD": 0xE55880,
        "RVA_ENDLESS_START": 0xDCDA00,
    }

    print("\n--- Found function bytes ---")
    for name, rva in found_rvas.items():
        bytes_data = dump_bytes(data, sections, rva, 64)
        if bytes_data:
            hex_str = ' '.join(f'{b:02X}' for b in bytes_data)
            print(f"{name} @ RVA 0x{rva:X}: {hex_str}")
        else:
            print(f"{name} @ RVA 0x{rva:X}: NOT FOUND IN SECTIONS")

    # Check old 1.6.1 RVAs to see what's there now
    old_rvas = {
        "OLD_SET_SPEED": 0x11BD080,
        "OLD_CHANGE_RESOURCE": 0x168E340,
        "OLD_PAUSE_ADD_REASON": 0xF6C530,
        "OLD_PAUSE_REMOVE_REASON": 0xF6CA80,
        "OLD_NATIVE_SAVE": 0x11B5F80,
        "OLD_NATIVE_LOAD": 0x11B5E30,
        "OLD_SELECT_MAP": 0x1A8B880,
        "OLD_ENDLESS_START": 0x1A8ADE0,
        "OLD_GLOBAL_TIME": 0x2B6A710,
    }

    print("\n--- Old 1.6.1 RVA contents ---")
    for name, rva in old_rvas.items():
        bytes_data = dump_bytes(data, sections, rva, 32)
        if bytes_data:
            hex_str = ' '.join(f'{b:02X}' for b in bytes_data)
            print(f"{name} @ RVA 0x{rva:X}: {hex_str}")
        else:
            print(f"{name} @ RVA 0x{rva:X}: NOT FOUND IN SECTIONS")

    # Search for global time pattern - look for references to .data section
    print("\n--- Searching for global time pointer (around 0x2B6A710 area) ---")
    text_sec = sections.get('.text', {})
    data_sec = sections.get('.data', {})

    if text_sec and data_sec:
        text_rva = text_sec['rva']
        text_raw = text_sec['raw']
        text_size = text_sec['size']
        text_data = data[text_raw:text_raw+text_size]

        # Look for LEA instructions pointing to .data section range
        data_start = data_sec['rva']
        data_end = data_sec['rva'] + data_sec['size']
        print(f".data section: RVA 0x{data_start:X} - 0x{data_end:X}")

        # Search for LEA RAX, [RIP+offset] where target is in .data
        lea_prefix = bytes([0x48, 0x8D, 0x05])
        for idx in range(len(text_data) - 7):
            if text_data[idx:idx+3] == lea_prefix:
                rel32 = struct.unpack('<i', text_data[idx+3:idx+7])[0]
                target_rva = text_rva + idx + 7 + rel32
                if data_start <= target_rva < data_end:
                    # Check if target is near old global time 0x2B6A710
                    if abs(target_rva - 0x2B6A710) < 0x10000:  # within 64KB
                        print(f"  Code RVA 0x{text_rva+idx:X} -> Data RVA 0x{target_rva:X} (diff from 0x2B6A710: {target_rva - 0x2B6A710:+X})")

                    # Also check for global time specifically - look for the exact offset 0x2B6A710
                    if target_rva == 0x2B6A710:
                        print(f"  *** FOUND EXACT GLOBAL TIME AT 0x2B6A710 *** Code RVA 0x{text_rva+idx:X}")

    # Also scan for the actual global time variable by looking at .data section directly
    print("\n--- Scanning .data section for time-related values ---")
    if data_sec:
        data_raw = data_sec['raw']
        data_size = data_sec['size']
        data_bytes = data[data_raw:data_raw+data_size]

        # Look for values that could be game time (float/double around reasonable values)
        # Game time is likely a float or double representing seconds/ticks
        for offset in range(0, data_size - 8, 4):
            # Read as float
            f = struct.unpack('<f', data_bytes[offset:offset+4])[0]
            # Read as double
            d = struct.unpack('<d', data_bytes[offset:offset+8])[0] if offset + 8 <= data_size else 0

            # Game time typically starts small and grows - look for reasonable float values
            if 0 < f < 1000000 and f == f:  # not NaN
                rva = data_sec['rva'] + offset
                # Only print if near old global time
                if abs(rva - 0x2B6A710) < 0x1000:
                    print(f"  Float at RVA 0x{rva:X} (offset 0x{offset:X}): {f}")

    mm.close()
    f.close()