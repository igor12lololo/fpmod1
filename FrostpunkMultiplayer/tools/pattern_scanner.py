#!/usr/bin/env python3
import sys
import mmap
import struct
import re

def parse_pattern(pattern_str):
    """Parse pattern string like '48 89 5C 24 ? 48 89 74 24 ?' to bytes with wildcards"""
    bytes_list = []
    for token in pattern_str.split():
        if token == '?' or token == '??':
            bytes_list.append(None)  # wildcard
        else:
            bytes_list.append(int(token, 16))
    return bytes_list

def match_pattern(data, offset, pattern):
    """Match pattern at offset, None = wildcard"""
    if offset + len(pattern) > len(data):
        return False
    for i, b in enumerate(pattern):
        if b is not None and data[offset + i] != b:
            return False
    return True

def scan_file(filepath, patterns):
    """Scan file for patterns"""
    print(f"Scanning: {filepath}")
    with open(filepath, 'rb') as f:
        with mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as mm:
            data = mm
            print(f"File size: {len(data)} bytes")

            # Parse PE header properly
            dos_header = data[:64]
            if dos_header[:2] != b'MZ':
                print("Not a valid MZ header")
                return
            e_lfanew = struct.unpack('<I', dos_header[60:64])[0]
            print(f"e_lfanew: 0x{e_lfanew:X}")

            if e_lfanew >= len(data):
                print("Invalid e_lfanew")
                return

            pe_sig = data[e_lfanew:e_lfanew+4]
            if pe_sig != b'PE\0\0':
                print(f"Invalid PE signature: {pe_sig}")
                return

            # COFF header (20 bytes)
            coff = data[e_lfanew+4:e_lfanew+24]
            machine = struct.unpack('<H', coff[0:2])[0]
            num_sections = struct.unpack('<H', coff[2:4])[0]
            print(f"Machine: 0x{machine:X}, Sections: {num_sections}")

            # Optional header
            opt_offset = e_lfanew + 24
            opt_magic = struct.unpack('<H', data[opt_offset:opt_offset+2])[0]
            print(f"Optional header magic: 0x{opt_magic:X}")
            if opt_magic != 0x20b:  # PE32+
                print("Not PE32+ (64-bit)")
                return

            # Standard fields (28 bytes for PE32+)
            # 0-1: magic, 2-3: linker version, 4-7: size of code, 8-11: size of initialized data,
            # 12-15: size of uninit data, 16-19: entry point, 20-23: base of code
            std_fields = data[opt_offset+2:opt_offset+30]
            size_of_code = struct.unpack('<I', std_fields[4:8])[0]
            size_of_initialized_data = struct.unpack('<I', std_fields[8:12])[0]
            entry_point = struct.unpack('<I', std_fields[16:20])[0]
            base_of_code = struct.unpack('<I', std_fields[20:24])[0]

            # NT optional fields - image base at offset 24 from start of optional header
            image_base = struct.unpack('<Q', data[opt_offset+24:opt_offset+32])[0]
            section_alignment = struct.unpack('<I', data[opt_offset+32:opt_offset+36])[0]
            file_alignment = struct.unpack('<I', data[opt_offset+36:opt_offset+40])[0]

            print(f"ImageBase: 0x{image_base:X}")
            print(f"BaseOfCode: 0x{base_of_code:X} (RVA)")
            print(f"SizeOfCode: {size_of_code}")
            print(f"SizeOfInitializedData: {size_of_initialized_data}")
            print(f"EntryPoint: 0x{entry_point:X}")
            print(f"SectionAlignment: 0x{section_alignment:X}")
            print(f"FileAlignment: 0x{file_alignment:X}")
            print()

            # Parse section headers to find .text section
            section_header_offset = opt_offset + 224  # standard PE32+ optional header size
            text_rva = 0
            text_size = 0
            text_raw_ptr = 0

            for i in range(num_sections):
                sec_offset = section_header_offset + i * 40
                sec_name = data[sec_offset:sec_offset+8].rstrip(b'\0')
                v_rva = struct.unpack('<I', data[sec_offset+12:sec_offset+16])[0]
                v_size = struct.unpack('<I', data[sec_offset+8:sec_offset+12])[0]
                raw_ptr = struct.unpack('<I', data[sec_offset+20:sec_offset+24])[0]
                raw_size = struct.unpack('<I', data[sec_offset+16:sec_offset+20])[0]
                characteristics = struct.unpack('<I', data[sec_offset+36:sec_offset+40])[0]

                print(f"Section {i}: {sec_name.decode('ascii', errors='ignore')} RVA=0x{v_rva:X} VSZ=0x{v_size:X} Raw=0x{raw_ptr:X} RSZ=0x{raw_size:X} Char=0x{characteristics:X}")

                if b'.text' in sec_name or (characteristics & 0x20000020):  # CODE | EXECUTE_READ
                    text_rva = v_rva
                    text_size = v_size
                    text_raw_ptr = raw_ptr
                    print(f"  -> Using as code section")

            if text_rva == 0:
                # Fallback to optional header values
                text_rva = base_of_code
                text_size = size_of_code
                print(f"Using fallback code section: RVA=0x{text_rva:X}, Size={text_size}")

            print()

            # Scan code section - use RVA to file offset conversion
            # Find the .text section raw offset
            text_raw_offset = 0
            for i in range(num_sections):
                sec_offset = section_header_offset + i * 40
                sec_name = data[sec_offset:sec_offset+8].rstrip(b'\0')
                v_rva = struct.unpack('<I', data[sec_offset+12:sec_offset+16])[0]
                raw_ptr = struct.unpack('<I', data[sec_offset+20:sec_offset+24])[0]
                if v_rva == text_rva:
                    text_raw_offset = raw_ptr
                    break

            if text_raw_offset == 0:
                text_raw_offset = text_rva  # fallback

            print(f"Code section: RVA 0x{text_rva:X}-0x{text_rva+text_size:X}, Raw offset: 0x{text_raw_offset:X}")

            # Scan only the .text section in file
            code_start_raw = text_raw_offset
            code_end_raw = text_raw_offset + text_size

            for name, pattern_str, expected_rva in patterns:
                pattern = parse_pattern(pattern_str)
                found = False
                # Scan in file (much faster than RVA scan)
                for offset in range(code_start_raw, code_end_raw - len(pattern)):
                    if match_pattern(data, offset, pattern):
                        # Convert file offset back to RVA
                        rva = text_rva + (offset - text_raw_offset)
                        status = "MATCH" if rva == expected_rva else "DIFFERENT"
                        print(f"{name}: expected RVA 0x{expected_rva:X} -> found at RVA 0x{rva:X} {status}")
                        found = True
                        break
                if not found:
                    print(f"{name}: expected RVA 0x{expected_rva:X} -> NOT FOUND in code section")

            # Also scan for data references (global pointers)
            print("\n--- Scanning for global pointer patterns ---")
            lea_pattern = [0x48, 0x8D, 0x05, None, None, None, None]
            print("Scanning for LEA RAX, [RIP+offset] (global pointer refs)...")
            count = 0
            for offset in range(code_start, min(code_end, code_start + 0x200000)):
                if match_pattern(data, offset, lea_pattern):
                    rel32 = struct.unpack('<i', data[offset+3:offset+7])[0]
                    target_rva = offset + 7 + rel32
                    if 0x2000000 < target_rva < 0x5000000:
                        print(f"  Code RVA 0x{offset:X} -> Data RVA 0x{target_rva:X}")
                        count += 1
                        if count > 30:
                            break

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python pattern_scanner.py <Frostpunk.exe>")
        sys.exit(1)

    patterns = [
        ("RVA_SET_SPEED",        "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 48 8B F9 8B DA", 0x11BD080),
        ("RVA_CHANGE_RESOURCE",  "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 48 8B F9 48 8B F2", 0x168E340),
        ("RVA_PAUSE_ADD_REASON", "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 48 8B F9 48 8B F2 48 8B D9", 0xF6C530),
        ("RVA_PAUSE_REMOVE_REASON","48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 48 8B F9 48 8B F2", 0xF6CA80),
        ("RVA_NATIVE_SAVE",      "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 48 8B F9 48 8B F2", 0x11B5F80),
        ("RVA_NATIVE_LOAD",      "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 48 8B F9 48 8B F2", 0x11B5E30),
        ("RVA_SELECT_MAP",       "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 48 8B F9 8B DA", 0x1A8B880),
        ("RVA_ENDLESS_START",    "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 48 8B F9", 0x1A8ADE0),
    ]

    scan_file(sys.argv[1], patterns)