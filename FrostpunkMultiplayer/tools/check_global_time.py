#!/usr/bin/env python3
import sys
import mmap
import struct

def read_pe_sections(filepath):
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
    for name, sec in sections.items():
        if sec['rva'] <= rva < sec['rva'] + sec['size']:
            return sec['raw'] + (rva - sec['rva'])
    return None

def dump_bytes(data, sections, rva, length=32):
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

    # Check the main global time candidates
    candidates = [
        0x2B5ABF0,  # Most referenced
        0x2B5A938,
        0x2B5A948,
        0x2B5AF38,
        0x2B5A618,
        0x2B5A838,
        0x2B5A7D0,
        0x2B5A030,
        0x2B5A358,
        0x2B5A370,
        0x2B5A388,
        0x2B5A398,
        0x2B5A3B8,
        0x2B5A3A8,
        0x2B5A3D8,
        0x2B5A3C0,
        0x2B5A3D0,
        0x2B5A3C8,
        0x2B5A3B0,
        0x2B5A3E0,
        0x2B5A148,
    ]

    print("=== Global time candidates ===")
    for rva in candidates:
        bytes_data = dump_bytes(data, sections, rva, 16)
        if bytes_data:
            d = struct.unpack('<d', bytes_data[:8])[0]
            f = struct.unpack('<f', bytes_data[:4])[0]
            print(f"  0x{rva:X}: double={d}, float={f}, hex={format_hex(bytes_data, 16)}")

    # Also check old location
    print("\n=== Old global time 0x2B6A710 ===")
    old = dump_bytes(data, sections, 0x2B6A710, 16)
    if old:
        d = struct.unpack('<d', old[:8])[0]
        print(f"  0x2B6A710: double={d}, hex={format_hex(old, 16)}")

    # Now search for SET_SPEED - it's a simple function that takes speed mode
    # Look for function that doesn't have complex body after prologue
    text_sec = sections.get('.text', {})
    text_rva = text_sec['rva']
    text_raw = text_sec['raw']
    text_size = text_sec['size']
    text_data = data[text_raw:text_raw+text_size]

    print("\n=== SET_SPEED: Search for '8B DA' (MOV EBX, EDX) after prologue with short body ===")
    for idx in range(len(text_data) - 20):
        if (text_data[idx:idx+4] == bytes([0x48, 0x89, 0x5C, 0x24]) and
            text_data[idx+5] == 0x48 and text_data[idx+6] == 0x89 and text_data[idx+7] == 0x74 and
            text_data[idx+8] == 0x24 and text_data[idx+10] == 0x57 and
            text_data[idx+11] == 0x48 and text_data[idx+12] == 0x83 and text_data[idx+13] == 0xEC and
            text_data[idx+14] == 0x20 and text_data[idx+15] == 0x48 and text_data[idx+16] == 0x8B and
            text_data[idx+17] == 0xF9):
            # Check for 8B DA immediately after
            if idx + 19 < len(text_data) and text_data[idx+18] == 0x8B and text_data[idx+19] == 0xDA:
                rva = text_rva + idx
                # Check function size - should be small
                body = text_data[idx+20:idx+60]
                print(f"  SET_SPEED candidate at RVA 0x{rva:X}: {format_hex(text_data[idx:idx+20])} | body={format_hex(body)}")

    print("\n=== PAUSE_ADD_REASON: Search for '48 8B D9' after prologue ===")
    for idx in range(len(text_data) - 22):
        if (text_data[idx:idx+4] == bytes([0x48, 0x89, 0x5C, 0x24]) and
            text_data[idx+5] == 0x48 and text_data[idx+6] == 0x89 and text_data[idx+7] == 0x74 and
            text_data[idx+8] == 0x24 and text_data[idx+10] == 0x57 and
            text_data[idx+11] == 0x48 and text_data[idx+12] == 0x83 and text_data[idx+13] == 0xEC and
            text_data[idx+14] == 0x20 and text_data[idx+15] == 0x48 and text_data[idx+16] == 0x8B and
            text_data[idx+17] == 0xF9):
            if idx + 20 < len(text_data) and text_data[idx+18] == 0x48 and text_data[idx+19] == 0x8B and text_data[idx+20] == 0xD9:
                rva = text_rva + idx
                body = text_data[idx+21:idx+60]
                print(f"  PAUSE_ADD_REASON candidate at RVA 0x{rva:X}: {format_hex(text_data[idx:idx+21])} | body={format_hex(body)}")

    # Check the ENDLESS_START function at 0xDCDA00 more closely
    print("\n=== ENDLESS_START at 0xDCDA00 ===")
    end_bytes = dump_bytes(data, sections, 0xDCDA00, 128)
    if end_bytes:
        print(f"  {format_hex(end_bytes, 128)}")

    # Check if there's a function at old SET_SPEED location that has the right signature
    print("\n=== Checking old SET_SPEED location 0x11BD080 ===")
    old_set = dump_bytes(data, sections, 0x11BD080, 64)
    if old_set:
        print(f"  {format_hex(old_set, 64)}")

    # Search for all functions with standard prologue + simple body (few instructions then RET)
    print("\n=== Small functions with standard prologue ===")
    for idx in range(len(text_data) - 30):
        if (text_data[idx:idx+4] == bytes([0x48, 0x89, 0x5C, 0x24]) and
            text_data[idx+5] == 0x48 and text_data[idx+6] == 0x89 and text_data[idx+7] == 0x74 and
            text_data[idx+8] == 0x24 and text_data[idx+10] == 0x57 and
            text_data[idx+11] == 0x48 and text_data[idx+12] == 0x83 and text_data[idx+13] == 0xEC and
            text_data[idx+14] == 0x20 and text_data[idx+15] == 0x48 and text_data[idx+16] == 0x8B and
            text_data[idx+17] == 0xF9):
            # Check next instruction and look for RET soon after
            rva = text_rva + idx
            next_bytes = text_data[idx+18:idx+40]
            # Look for RET (C3) within 30 bytes
            ret_pos = -1
            for i, b in enumerate(next_bytes):
                if b == 0xC3:
                    ret_pos = i
                    break
            if ret_pos >= 0 and ret_pos < 30:
                print(f"  Small func at RVA 0x{rva:X}: len={ret_pos+1}, next={format_hex(next_bytes[:ret_pos+1])}")

    mm.close()
    f.close()