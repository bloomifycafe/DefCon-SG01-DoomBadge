#!/usr/bin/env python3
"""
wad_audit.py — read-only WAD analyzer.

Parse the lump directory of an IWAD/PWAD and print a category-by-category
size breakdown. Used to plan trim_wad.py decisions: before cutting we
look at the actual byte budgets in the user's WAD.

Usage:
    python3 tools/wad_audit.py path/to/DOOM.WAD
    python3 tools/wad_audit.py path/to/DOOM.WAD --by-lump   # also list every lump

The categorization mirrors the strip categories in trim_wad.py so the
output directly answers "how much would --strip music save."
"""
import argparse
import struct
import sys

CATEGORY_ORDER = [
    "header_pad",
    "maps_e1", "maps_e2", "maps_e3", "maps_e4", "maps_other",
    "sprites", "patches", "flats", "textures",
    "music", "sfx", "speaker", "sndcfg",
    "screens", "intermission", "demos", "endoom",
    "colormap_palette", "menu_hud_misc", "other",
]

MAP_SUB_LUMPS = {
    "THINGS", "LINEDEFS", "SIDEDEFS", "VERTEXES",
    "SEGS", "SSECTORS", "NODES", "SECTORS",
    "REJECT", "BLOCKMAP",
}


def parse_wad(path):
    """Returns (sig, list of (name, ofs, size, idx))."""
    with open(path, "rb") as f:
        data = f.read()
    sig = data[:4]
    if sig not in (b"IWAD", b"PWAD"):
        sys.exit("error: %r is not a WAD (sig=%r)" % (path, sig))
    numlumps, infotableofs = struct.unpack("<II", data[4:12])
    lumps = []
    for i in range(numlumps):
        e = data[infotableofs + i * 16: infotableofs + (i + 1) * 16]
        ofs, sz, name8 = struct.unpack("<II8s", e)
        name = name8.rstrip(b"\x00").decode("ascii", "replace").upper()
        lumps.append((name, ofs, sz, i))
    return sig, lumps, len(data)


def categorize(lumps):
    """Return dict of category -> list of (name, size, idx).

    Map lumps are tagged by walking the directory: an `E?M?` (or `MAPnn`)
    header is followed by exactly the 10 sub-lumps in MAP_SUB_LUMPS. We
    track which episode we're in to bucket maps_e1..maps_e4."""
    cat = {c: [] for c in CATEGORY_ORDER}

    in_section = None  # "S", "F", "P" when between *_START and *_END
    in_map_episode = None
    map_sub_left = 0

    for (name, ofs, sz, idx) in lumps:
        # Section start/end markers themselves are 0-byte, route to "other"
        if name in ("S_START", "S_END", "SS_START", "SS_END",
                    "F_START", "F_END", "FF_START", "FF_END",
                    "F1_START", "F1_END", "F2_START", "F2_END",
                    "F3_START", "F3_END",
                    "P_START", "P_END", "PP_START", "PP_END",
                    "P1_START", "P1_END", "P2_START", "P2_END",
                    "P3_START", "P3_END"):
            cat["other"].append((name, sz, idx))
            if name.endswith("_START"):
                if name.startswith("S"):
                    in_section = "S"
                elif name.startswith("F"):
                    in_section = "F"
                elif name.startswith("P"):
                    in_section = "P"
            elif name.endswith("_END"):
                in_section = None
            continue

        if in_section == "S":
            cat["sprites"].append((name, sz, idx))
            continue
        if in_section == "F":
            cat["flats"].append((name, sz, idx))
            continue
        if in_section == "P":
            cat["patches"].append((name, sz, idx))
            continue

        # Map sub-lumps (after we saw an E?M? or MAPnn header)
        if map_sub_left > 0 and name in MAP_SUB_LUMPS:
            cat["maps_e%d" % in_map_episode if in_map_episode else "maps_other"].append((name, sz, idx))
            map_sub_left -= 1
            if map_sub_left == 0:
                in_map_episode = None
            continue
        # If we expected sub-lumps but the next lump isn't one, abort the
        # walk for that map (vanilla WADs are well-formed; PWADs may not be).
        if map_sub_left > 0:
            map_sub_left = 0
            in_map_episode = None

        # Map header detection
        if (len(name) == 4 and name[0] == "E" and name[2] == "M"
                and name[1].isdigit() and name[3].isdigit()):
            episode = int(name[1])
            in_map_episode = episode
            map_sub_left = len(MAP_SUB_LUMPS)
            key = "maps_e%d" % episode if 1 <= episode <= 4 else "maps_other"
            cat[key].append((name, sz, idx))
            continue
        if name.startswith("MAP") and len(name) == 5 and name[3:5].isdigit():
            in_map_episode = None
            map_sub_left = len(MAP_SUB_LUMPS)
            cat["maps_other"].append((name, sz, idx))
            continue

        # Music: D_<name>, but NOT DEMO[1234], DMXGUS, DSXX, DPXX
        if (name.startswith("D_") and not name.startswith("DS")
                and not name.startswith("DP") and name != "DMXGUS"):
            cat["music"].append((name, sz, idx))
            continue
        if name.startswith("DS") and len(name) > 2:
            cat["sfx"].append((name, sz, idx))
            continue
        if name.startswith("DP") and len(name) > 2:
            cat["speaker"].append((name, sz, idx))
            continue
        if name in ("DMXGUS", "GENMIDI"):
            cat["sndcfg"].append((name, sz, idx))
            continue
        if name.startswith("DEMO") and len(name) == 5 and name[4].isdigit():
            cat["demos"].append((name, sz, idx))
            continue
        if name == "ENDOOM":
            cat["endoom"].append((name, sz, idx))
            continue
        if name in ("HELP", "HELP1", "HELP2", "CREDIT", "TITLEPIC",
                    "VICTORY2", "PFUB1", "PFUB2",
                    "END0", "END1", "END2", "END3", "END4", "END5", "END6"):
            cat["screens"].append((name, sz, idx))
            continue
        if name in ("INTERPIC", "BOSSBACK") or name.startswith("WIMAP") \
                or name.startswith("WILV") or name.startswith("WIA") \
                or name.startswith("WIF") or name.startswith("WIB") \
                or name.startswith("WIP") or name.startswith("WIOST") \
                or name.startswith("WIOSTS") or name.startswith("WICOLON") \
                or name in ("WIMINUS", "WIPCNT", "WINUM0", "WINUM1",
                            "WINUM2", "WINUM3", "WINUM4", "WINUM5",
                            "WINUM6", "WINUM7", "WINUM8", "WINUM9",
                            "WIKILRS", "WIVCTMS", "WISCRT2", "WIENTER",
                            "WIFRGS", "WITIME", "WIPAR", "WISUCKS",
                            "WIH0", "WIH1", "WIH2", "WIH3"):
            cat["intermission"].append((name, sz, idx))
            continue
        if name in ("TEXTURE1", "TEXTURE2", "PNAMES"):
            cat["textures"].append((name, sz, idx))
            continue
        if name in ("COLORMAP", "PLAYPAL"):
            cat["colormap_palette"].append((name, sz, idx))
            continue
        # Status bar, menu, HUD font, gun sprites in :stbar... pulled out
        # of S_/P_ sections via vanilla layout we approximate here:
        if (name.startswith("ST") or name.startswith("M_")
                or name.startswith("STBAR") or name.startswith("STG")
                or name.startswith("STT") or name.startswith("STC")
                or name.startswith("STK") or name.startswith("STF")
                or name.startswith("STY") or name.startswith("STA")
                or name.startswith("STP") or name.startswith("STD")
                or name.startswith("STIM") or name.startswith("STMI")
                or name.startswith("STIN") or name.startswith("AMM")
                or name.startswith("HU_") or name == "DBIGFONT"):
            cat["menu_hud_misc"].append((name, sz, idx))
            continue

        cat["other"].append((name, sz, idx))

    return cat


def fmt_kib(n):
    return "%d B" % n if n < 1024 else "%.1f KiB" % (n / 1024.0)


def fmt_mib(n):
    if n < 1024:
        return "%d B" % n
    if n < 1024 * 1024:
        return "%.1f KiB" % (n / 1024.0)
    return "%.2f MiB" % (n / (1024.0 * 1024.0))


CATEGORY_LABELS = {
    "header_pad": "WAD header + dir",
    "maps_e1": "Maps E1 (E1M1-E1M9)",
    "maps_e2": "Maps E2 (E2M1-E2M9)",
    "maps_e3": "Maps E3 (E3M1-E3M9)",
    "maps_e4": "Maps E4 (E4M1-E4M9)  [drop for E1+E2+E3 build]",
    "maps_other": "Other maps (MAPnn etc.)",
    "sprites": "Sprites (S_START..S_END)",
    "patches": "Wall patches (P_START..P_END)",
    "flats": "Flats (F_START..F_END)",
    "textures": "Texture defs (TEXTURE1/2, PNAMES)",
    "music": "Music (D_*)  [strippable: sound off]",
    "sfx": "SFX (DS*)  [strippable: sound off]",
    "speaker": "PC speaker (DP*)  [strippable: sound off]",
    "sndcfg": "Sound config (DMXGUS, GENMIDI)  [strippable]",
    "screens": "Screens (HELP/CREDIT/TITLEPIC/etc.)",
    "intermission": "Intermission graphics (WI*, INTERPIC, BOSSBACK)",
    "demos": "Built-in demos (DEMO1-4)  [strippable]",
    "endoom": "ENDOOM terminal art  [strippable]",
    "colormap_palette": "COLORMAP + PLAYPAL  [keep]",
    "menu_hud_misc": "Menu / HUD / status bar graphics",
    "other": "Other / markers / unknown",
}


STRIPPABLE = {"music", "sfx", "speaker", "sndcfg", "demos", "endoom",
              "screens", "maps_e4"}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("wad")
    ap.add_argument("--by-lump", action="store_true",
                    help="Also dump every lump, sorted by size descending.")
    ap.add_argument("--top", type=int, default=20,
                    help="With --by-lump, how many to show (default 20).")
    args = ap.parse_args()

    sig, lumps, total = parse_wad(args.wad)
    cat = categorize(lumps)

    print("WAD: %s  (%s, %d lumps, %s on disk)"
          % (args.wad, sig.decode(), len(lumps), fmt_mib(total)))
    print()

    cat_total = 0
    strippable_total = 0
    rows = []
    for c in CATEGORY_ORDER:
        items = cat[c]
        if not items:
            continue
        size = sum(sz for (_, sz, _) in items)
        cat_total += size
        if c in STRIPPABLE:
            strippable_total += size
        rows.append((c, len(items), size))

    rows.sort(key=lambda r: -r[2])
    name_w = max(len(CATEGORY_LABELS.get(c, c)) for c, _, _ in rows)
    fmt = "  %-{w}s  %5d lumps   %10s   %5.1f%%".format(w=name_w)
    print("Category breakdown (sorted by size):")
    for c, n, size in rows:
        pct = 100.0 * size / total if total else 0.0
        marker = "  *" if c in STRIPPABLE else "   "
        print(fmt % (CATEGORY_LABELS.get(c, c), n, fmt_mib(size), pct) + marker)
    print()
    print("  * = category that trim_wad.py can drop without breaking the renderer")
    print()
    print("Lump bytes accounted for: %s of %s (%.1f%%)"
          % (fmt_mib(cat_total), fmt_mib(total),
             100.0 * cat_total / total if total else 0.0))
    print("If you strip every starred category: save %s -> WAD becomes ~%s"
          % (fmt_mib(strippable_total), fmt_mib(total - strippable_total)))
    print()
    print("Badge target after Arc 0 firmware shrink: 7.0 MiB wad partition.")
    target = 7 * 1024 * 1024
    after = total - strippable_total
    if after <= target:
        print("=> Stripping all starred categories FITS the target with %s headroom."
              % fmt_mib(target - after))
    else:
        print("=> Even with all starred categories stripped, WAD is %s OVER target."
              % fmt_mib(after - target))
        print("   Next levers: drop maps_e3 too, or per-lump sprite/texture pruning.")

    if args.by_lump:
        all_lumps = [(name, sz, idx) for (name, _, sz, idx) in lumps]
        all_lumps.sort(key=lambda r: -r[1])
        print()
        print("Top %d lumps by size:" % args.top)
        for name, sz, idx in all_lumps[:args.top]:
            print("  %-12s  %10s   (lump #%d)" % (name, fmt_mib(sz), idx))


if __name__ == "__main__":
    main()
