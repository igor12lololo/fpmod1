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

def dump_bytes(data, sections, rva, length=64):
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

    print("=== SET_SPEED: Look for '8B DA' (2-byte MOV EBX, EDX) exactly after 48 8B F9 ===")
    for idx in range(len(text_data) - 20):
        # Standard prologue: 48 89 5C 24 XX 48 89 74 24 XX 57 48 83 EC 20 48 8B F9
        if (text_data[idx:idx+4] == bytes([0x48, 0x89, 0x5C, 0x24]) and
            text_data[idx+5] == 0x48 and text_data[idx+6] == 0x89 and text_data[idx+7] == 0x74 and
            text_data[idx+8] == 0x24 and text_data[idx+10] == 0x57 and
            text_data[idx+11] == 0x48 and text_data[idx+12] == 0x83 and text_data[idx+13] == 0xEC and
            text_data[idx+14] == 0x20 and text_data[idx+15] == 0x48 and text_data[idx+16] == 0x8B and
            text_data[idx+17] == 0xF9):
            # Next instruction: check for 8B DA (2 bytes) - NOT 48 8B DA or 48 8D
            if idx + 19 < len(text_data):
                if text_data[idx+18] == 0x8B and text_data[idx+19] == 0xDA:
                    rva = text_rva + idx
                    print(f"  SET_SPEED candidate at RVA 0x{rva:X}: {format_hex(text_data[idx:idx+40])}")

    print("\n=== PAUSE_ADD_REASON: Look for '48 8B D9' (3-byte MOV RBX, RCX) exactly after 48 8B F9 ===")
    for idx in range(len(text_data) - 22):
        if (text_data[idx:idx+4] == bytes([0x48, 0x89, 0x5C, 0x24]) and
            text_data[idx+5] == 0x48 and text_data[idx+6] == 0x89 and text_data[idx+7] == 0x74 and
            text_data[idx+8] == 0x24 and text_data[idx+10] == 0x57 and
            text_data[idx+11] == 0x48 and text_data[idx+12] == 0x83 and text_data[idx+13] == 0xEC and
            text_data[idx+14] == 0x20 and text_data[idx+15] == 0x48 and text_data[idx+16] == 0x8B and
            text_data[idx+17] == 0xF9):
            # Next: 48 8B D9 (3 bytes)
            if idx + 20 < len(text_data):
                if text_data[idx+18] == 0x48 and text_data[idx+19] == 0x8B and text_data[idx+20] == 0xD9:
                    rva = text_rva + idx
                    print(f"  PAUSE_ADD_REASON candidate at RVA 0x{rva:X}: {format_hex(text_data[idx:idx+40])}")

    print("\n=== SELECT_MAP: Look for old pattern 48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 48 8B F9 8B DA ===")
    # This is same as SET_SPEED - need to differentiate by body
    for idx in range(len(text_data) - 30):
        if (text_data[idx:idx+4] == bytes([0x48, 0x89, 0x5C, 0x24]) and
            text_data[idx+5] == 0x48 and text_data[idx+6] == 0x89 and text_data[idx+7] == 0x74 and
            text_data[idx+8] == 0x24 and text_data[idx+10] == 0x57 and
            text_data[idx+11] == 0x48 and text_data[idx+12] == 0x83 and text_data[idx+13] == 0xEC and
            text_data[idx+14] == 0x20 and text_data[idx+15] == 0x48 and text_data[idx+16] == 0x8B and
            text_data[idx+17] == 0xF9 and text_data[idx+18] == 0x8B and text_data[idx+19] == 0xDA):
            rva = text_rva + idx
            # Check body - SELECT_MAP should have map-related code
            body = format_hex(text_data[idx+20:idx+60])
            print(f"  SET_SPEED/SELECT_MAP at RVA 0x{rva:X}: body={body}")

    print("\n=== PAUSE_REMOVE_REASON: Look for standard prologue + different body ===")
    for idx in range(len(text_data) - 30):
        if (text_data[idx:idx+4] == bytes([0x48, 0x89, 0x5C, 0x24]) and
            text_data[idx+5] == 0x48 and text_data[idx+6] == 0x89 and text_data[idx+7] == 0x74 and
            text_data[idx+8] == 0x24 and text_data[idx+10] == 0x57 and
            text_data[idx+11] == 0x48 and text_data[idx+12] == 0x83 and text_data[idx+13] == 0xEC and
            text_data[idx+14] == 0x20 and text_data[idx+15] == 0x48 and text_data[idx+16] == 0x8B and
            text_data[idx+17] == 0xF9 and text_data[idx+18] == 0x48 and text_data[idx+19] == 0x8B and
            text_data[idx+20] == 0xF2):
            # Check next 3 bytes - if NOT 48 8B 09 then it's PAUSE_REMOVE or other
            b21 = text_data[idx+21] if idx+21 < len(text_data) else 0
            b22 = text_data[idx+22] if idx+22 < len(text_data) else 0
            b23 = text_data[idx+23] if idx+23 < len(text_data) else 0
            if not (b21 == 0x48 and b22 == 0x8B and b23 == 0x09):
                rva = text_rva + idx
                print(f"  PAUSE_REMOVE candidate at RVA 0x{rva:X}: next=({b21:02X} {b22:02X} {b23:02X}) {format_hex(text_data[idx:idx+50])}")

    print("\n=== NATIVE_SAVE: Look for 'E8' CALL pattern after prologue (old had E8 4B 4F C5 FF) ===")
    for idx in range(len(text_data) - 30):
        if (text_data[idx:idx+4] == bytes([0x48, 0x89, 0x5C, 0x24]) and
            text_data[idx+5] == 0x48 and text_data[idx+6] == 0x89 and text_data[idx+7] == 0x74 and
            text_data[idx+8] == 0x24 and text_data[idx+10] == 0x57 and
            text_data[idx+11] == 0x48 and text_data[idx+12] == 0x83 and text_data[idx+13] == 0xEC and
            text_data[idx+14] == 0x20 and text_data[idx+15] == 0x48 and text_data[idx+16] == 0x8B and
            text_data[idx+17] == 0xF9 and text_data[idx+18] == 0x48 and text_data[idx+19] == 0x8B and
            text_data[idx+20] == 0xF2 and text_data[idx+21] == 0x48 and text_data[idx+22] == 0x8B and
            text_data[idx+23] == 0x09):
            rva = text_rva + idx
            body = format_hex(text_data[idx+24:idx+80])
            print(f"  NATIVE_SAVE candidate at RVA 0x{rva:X}: {format_hex(text_data[idx:idx+40])} body={body}")

    print("\n=== NATIVE_LOAD: Look for 'E8' CALL at start (old started with E8 4B 4F C5 FF) ===")
    for idx in range(len(text_data) - 30):
        if text_data[idx] == 0xE8:
            # Check if this looks like a function start with prologue after
            rel32 = struct.unpack('<i', text_data[idx+1:idx+5])[0]
            target = text_rva + idx + 5 + rel32
            # Check if next bytes match prologue
            if idx + 10 < len(text_data):
                if text_data[idx+5:idx+9] == bytes([0x49, 0x8B, 0x0E]):  # MOV RCX, [R14]
                    rva = text_rva + idx
                    print(f"  NATIVE_LOAD candidate at RVA 0x{rva:X}: CALL->0x{target:X} {format_hex(text_data[idx:idx+40])}")

    print("\n=== Search for global time access (MOV RAX, [RIP+offset] to ~0x2B6A710 area) ===")
    time_access_pattern = bytes([0x48, 0x8B, 0x05])
    for idx in range(len(text_data) - 7):
        if text_data[idx:idx+3] == time_access_pattern:
            rel32 = struct.unpack('<i', text_data[idx+3:idx+7])[0]
            target_rva = text_rva + idx + 7 + rel32
            if 0x2B6A000 <= target_rva <= 0x2B6B000:
                rva = text_rva + idx
                print(f"  Time access at Code RVA 0x{rva:X} -> Data RVA 0x{target_rva:X}: {format_hex(text_data[idx:idx+16])}")

    print("\n=== Search for global time access to NEW location (0x2B5A000-0x2B5B000) ===")
    for idx in range(len(text_data) - 7):
        if text_data[idx:idx+3] == time_access_pattern:
            rel32 = struct.unpack('<i', text_data[idx+3:idx+7])[0]
            target_rva = text_rva + idx + 7 + rel32
            if 0x2B5A000 <= target_rva <= 0x2B5B000:
                rva = text_rva + idx
                print(f"  NEW Time access at Code RVA 0x{rva:X} -> Data RVA 0x{target_rva:X}: {format_hex(text_data[idx:idx+16])}")

    # Check what's at the old global time location 0x2B6A710
    print("\n=== Check old global time location 0x2B6A710 ===")
    old_time_bytes = dump_bytes(data, sections, 0x2B6A710, 32)
    if old_time_bytes:
        print(f"  0x2B6A710: {format_hex(old_time_bytes, 32)}")
        # Read as double
        d = struct.unpack('<d', old_time_bytes[:8])[0]
        print(f"  As double: {d}")

    # Check new candidates around 0x2B5Axxx
    print("\n=== Check new global time candidates ===")
    candidates = [0x2B5AEE8, 0x2B5AF10, 0x2B5AF38, 0x2B5AFD0, 0x2B5A8D8, 0x2B5A9F0, 0x2B5AA80, 0x2B5AB10, 0x2B5ABA0, 0x2B5AF48, 0x2B5AF80, 0x2B5AFE0, 0x2B5B018]
    for rva in candidates:
        bytes_data = dump_bytes(data, sections, rva, 16)
        if bytes_data:
            d = struct.unpack('<d', bytes_data[:8])[0]
            f = struct.unpack('<f', bytes_data[:4])[0]
            print(f"  0x{rva:X}: double={d}, float={f}, hex={format_hex(bytes_data, 16)}")

    mm.close()
    f.close()