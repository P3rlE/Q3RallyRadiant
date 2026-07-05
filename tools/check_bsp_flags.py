#!/usr/bin/env python3
"""
check_bsp_flags.py  –  Q3Rally BSP surface flag inspector
Usage: python3 check_bsp_flags.py <path/to/map.bsp>

Scans the BSP Textures lump and prints all shader entries that carry
non-zero surface flags. Specifically highlights Q3Rally physics flags.
"""

import struct
import sys

Q3RALLY_FLAGS = {
    0x00000002: "SURF_SLICK",
    0x00080000: "SURF_GRASS",
    0x00100000: "SURF_ASPHALT",
    0x00200000: "SURF_WET",
    0x00400000: "SURF_SNOW",
    0x00800000: "SURF_GRAVEL",
    0x01000000: "SURF_ICE",
    0x02000000: "SURF_DIRT",
    0x04000000: "SURF_METAL",
    0x08000000: "SURF_SAND",
}

def flag_names(sf):
    names = [name for bit, name in Q3RALLY_FLAGS.items() if sf & bit]
    return ", ".join(names) if names else "(none known)"

def check_bsp(path):
    with open(path, "rb") as f:
        data = f.read()

    magic = data[0:4]
    version = struct.unpack_from("<I", data, 4)[0]
    print(f"BSP: {path}")
    print(f"  Magic: {magic}  Version: {version}")
    if magic != b"IBSP" or version != 46:
        print("  WARNING: not a standard Q3 BSP (IBSP v46)")

    # Textures lump = lump index 1
    tex_off, tex_len = struct.unpack_from("<II", data, 8 + 1 * 8)
    num_tex = tex_len // 72
    print(f"  Textures lump: offset={tex_off}  length={tex_len}  entries={num_tex}\n")

    physics_flags = (0x00080000 | 0x00100000 | 0x00200000 | 0x00400000 |
                     0x00800000 | 0x01000000 | 0x02000000 | 0x04000000 | 0x08000000)

    found_physics = []
    for i in range(num_tex):
        e = tex_off + i * 72
        name = data[e:e+64].split(b'\x00')[0].decode("latin1")
        sf   = struct.unpack_from("<I", data, e + 64)[0]
        cf   = struct.unpack_from("<I", data, e + 68)[0]

        if sf != 0:
            print(f"  [{i:3d}] {name:<50s}  surfFlags=0x{sf:08X}  ({flag_names(sf)})")
        if sf & physics_flags:
            found_physics.append((i, name, sf))

    print()
    if found_physics:
        print(f"✅ Q3Rally-Physics-Flags gefunden in {len(found_physics)} Shader-Eintrag/Einträgen:")
        for i, name, sf in found_physics:
            print(f"   [{i}] {name}  →  {flag_names(sf)}")
    else:
        print("❌ KEINE Q3Rally-Physics-Flags im BSP gefunden!")
        print("   Mögliche Ursachen:")
        print("   1. q3map2 wurde mit einem alten Build ohne game_q3rally kompiliert")
        print("   2. q3map2 wurde ohne -game q3rally aufgerufen")
        print("   3. Die Flags wurden vom Shader überschrieben")
        print("   4. q3map2 liest die Face-Flags aus der .map nicht korrekt ein")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 check_bsp_flags.py <map.bsp>")
        sys.exit(1)
    check_bsp(sys.argv[1])
