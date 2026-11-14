#!/usr/bin/env python3
import sys
import mmap
import struct

def scan_file_fast(filepath, patterns):
    """Fast pattern scan using bytes.find for non-wildcard prefixes"""
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
            opt_header_size = struct.unpack('<H', coff[16:18])[0]
            print(f"Machine: 0x{machine:X}, Sections: {num_sections}, OptHeaderSize: {opt_header_size}")

            # Optional header
            opt_offset = e_lfanew + 24
            opt_magic = struct.unpack('<H', data[opt_offset:opt_offset+2])[0]
            print(f"Optional header magic: 0x{opt_magic:X}")
            if opt_magic != 0x20b:
                print("Not PE32+ (64-bit)")
                return

            # Standard fields (28 bytes for PE32+)
            std_fields = data[opt_offset+2:opt_offset+30]
            size_of_code = struct.unpack('<I', std_fields[4:8])[0]
            entry_point = struct.unpack('<I', std_fields[16:20])[0]
            base_of_code = struct.unpack('<I', std_fields[20:24])[0]

            # NT optional fields - image base at offset 24 from start of optional header
            image_base = struct.unpack('<Q', data[opt_offset+24:opt_offset+32])[0]

            print(f"ImageBase: 0x{image_base:X}")
            print(f"BaseOfCode: 0x{base_of_code:X} (RVA)")
            print(f"SizeOfCode: {size_of_code}")
            print(f"EntryPoint: 0x{entry_point:X}")
            print()

            # Parse section headers to find .text section
            section_header_offset = opt_offset + opt_header_size
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

                if b'.text' in sec_name or (characteristics & 0x20000020):
                    text_rva = v_rva
                    text_size = v_size
                    text_raw_ptr = raw_ptr
                    print(f"  -> Using as code section")

            if text_rva == 0:
                text_rva = base_of_code
                text_size = size_of_code
                print(f"Using fallback code section: RVA=0x{text_rva:X}, Size={text_size}")

            print()

            # Scan code section in file (much faster)
            code_start_raw = text_raw_ptr
            code_end_raw = text_raw_ptr + text_size
            code_data = data[code_start_raw:code_end_raw]

            print(f"Scanning code section: file offset 0x{code_start_raw:X}-0x{code_end_raw:X} ({len(code_data)} bytes)")
            print()

            for name, pattern_str, expected_rva in patterns:
                # Parse pattern to find longest non-wildcard prefix for fast search
                tokens = pattern_str.split()
                # Find first non-wildcard byte sequence
                prefix_bytes = []
                prefix_start = 0
                in_wildcard = True
                for i, tok in enumerate(tokens):
                    if tok != '?' and tok != '??':
                        if in_wildcard:
                            prefix_start = i
                            in_wildcard = False
                        prefix_bytes.append(int(tok, 16))
                    else:
                        if not in_wildcard:
                            break

                if not prefix_bytes:
                    print(f"{name}: pattern has no fixed prefix, skipping fast search")
                    continue

                prefix = bytes(prefix_bytes)
                # Search for prefix in code section
                found = False
                search_start = 0
                while True:
                    idx = code_data.find(prefix, search_start)
                    if idx == -1:
                        break

                    # Verify full pattern at this location
                    match = True
                    for j, tok in enumerate(tokens):
                        if tok != '?' and tok != '??':
                            if idx + j >= len(code_data) or code_data[idx + j] != int(tok, 16):
                                match = False
                                break

                    if match:
                        rva = text_rva + idx
                        status = "MATCH" if rva == expected_rva else "DIFFERENT"
                        print(f"{name}: expected RVA 0x{expected_rva:X} -> found at RVA 0x{rva:X} {status}")
                        found = True
                        break

                    search_start = idx + 1

                if not found:
                    print(f"{name}: expected RVA 0x{expected_rva:X} -> NOT FOUND in code section")

            # Also scan for global pointer references (LEA RAX, [RIP+offset])
            print("\n--- Scanning for global pointer patterns ---")
            lea_prefix = bytes([0x48, 0x8D, 0x05])
            print("Scanning for LEA RAX, [RIP+offset] (global pointer refs)...")
            count = 0
            for idx in range(0, len(code_data) - 7, 1):
                if code_data[idx:idx+3] == lea_prefix:
                    rel32 = struct.unpack('<i', code_data[idx+3:idx+7])[0]
                    target_rva = text_rva + idx + 7 + rel32
                    if 0x2000000 < target_rva < 0x5000000:
                        print(f"  Code RVA 0x{text_rva+idx:X} -> Data RVA 0x{target_rva:X}")
                        count += 1
                        if count > 30:
                            break

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python pattern_scanner_fast.py <Frostpunk.exe>")
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

    scan_file_fast(sys.argv[1], patterns)