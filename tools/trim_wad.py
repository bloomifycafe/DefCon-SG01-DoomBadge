#!/usr/bin/env python3
"""
trim_wad.py — produce a trimmed IWAD for the badge build.

Reads a source WAD, drops lumps in user-selected categories, rebuilds the
directory and writes a new IWAD. NEVER touches sprites, flats, wall patches
or texture defs by default — those cuts crash the renderer in ways that
are hard to debug. Use --strip to opt into specific categories.

Usage:
    python3 tools/trim_wad.py DOOM.WAD DOOM_TRIM.WAD \\
        --strip music,sfx,speaker,sndcfg,demos,endoom,e4
    python3 tools/trim_wad.py DOOM.WAD DOOM_TRIM.WAD --strip all
    python3 tools/trim_wad.py DOOM.WAD --list-strippable

Strippable categories:
    music         D_* music lumps (sound is OFF on the badge)
    sfx           DS* SFX lumps
    speaker       DP* PC speaker lumps
    sndcfg        DMXGUS, GENMIDI
    demos         DEMO1..DEMO4
    endoom        ENDOOM terminal screen (I_Endoom is no-op)
    screens       HELP/HELP1/HELP2/CREDIT/VICTORY2/PFUB1/PFUB2/END0..END6
    intermission  WI*, INTERPIC, BOSSBACK (intermission scene graphics)
                  REQUIRES gating WI_Drawer in firmware — see Arc 3.
    titlepic      TITLEPIC only (replaced with a 200-byte placeholder)
    fullscreens   ALL 320x200 fullscreen art lumps (TITLEPIC, INTERPIC,
                  BOSSBACK, ENDPIC, HELP/HELP1/HELP2, CREDIT, VICTORY2,
                  PFUB1/PFUB2) — *content* replaced with a transparent
                  1289-byte placeholder, lump NAME preserved so DOOM's
                  V_DrawPatch finds it and draws nothing instead of
                  crashing in W_CacheLumpName. Typical save: 500-700
                  KiB on Ultimate Doom. Cosmetic loss only.
    e4            All E4M? maps + their 10 sub-lumps each
    e3            All E3M? maps + their 10 sub-lumps each (last-resort)

The categorisation matches tools/wad_audit.py exactly so the audit's
"save: X.YZ MiB" line maps 1:1 onto what trim_wad.py will actually drop.

Per-lump pruning (--prune-unused):
    Walks every kept map's SIDEDEFS + SECTORS to build the set of
    referenced textures and flats. Expands texture references through
    TEXTURE1/TEXTURE2/PNAMES into the set of used patch lumps. Expands
    animation groups (NUKAGE1..NUKAGE3 etc.) and switch pairs
    (SW1xxx ↔ SW2xxx) so dynamic references don't get cut. Then drops
    every lump in P_START..P_END / F_START..F_END that nothing
    references. Saves ~0.5–1 MiB on Ultimate Doom when E4 is gone.
"""
import argparse
import os
import struct
import sys

MAP_SUB_LUMPS = {
    "THINGS", "LINEDEFS", "SIDEDEFS", "VERTEXES",
    "SEGS", "SSECTORS", "NODES", "SECTORS",
    "REJECT", "BLOCKMAP",
}

# DOOM animated flat/texture groups. Source: chocolate-doom p_spec.c
# animdefs[]. Format: (is_texture, last_name, first_name). The engine
# cycles through every lump from `first_name` to `last_name` in WAD
# directory order, so if any one is referenced we must keep them all.
ANIM_GROUPS = [
    # flats
    (False, "NUKAGE3",  "NUKAGE1"),
    (False, "FWATER4",  "FWATER1"),
    (False, "SWATER4",  "SWATER1"),
    (False, "LAVA4",    "LAVA1"),
    (False, "BLOOD3",   "BLOOD1"),
    # textures
    (True,  "BLODGR4",  "BLODGR1"),
    (True,  "SLADRIP3", "SLADRIP1"),
    (True,  "BLODRIP4", "BLODRIP1"),
    (True,  "FIREWALL", "FIREWALA"),
    (True,  "GSTFONT3", "GSTFONT1"),
    (True,  "FIRELAVA", "FIRELAV3"),
    (True,  "FIREMAG3", "FIREMAG1"),
    (True,  "FIREBLU2", "FIREBLU1"),
    (True,  "ROCKRED3", "ROCKRED1"),
]

# DOOM switch pairs. Source: chocolate-doom p_switch.c alphSwitchList up
# to gamemode == doom (Ultimate). Each entry's two strings are the OFF
# and ON wall textures; activating the switch swaps them, so both must
# survive if either is referenced.
SWITCH_PAIRS = [
    ("SW1BRCOM", "SW2BRCOM"),
    ("SW1BRN1",  "SW2BRN1"),
    ("SW1BRN2",  "SW2BRN2"),
    ("SW1BRNGN", "SW2BRNGN"),
    ("SW1BROWN", "SW2BROWN"),
    ("SW1COMM",  "SW2COMM"),
    ("SW1COMP",  "SW2COMP"),
    ("SW1DIRT",  "SW2DIRT"),
    ("SW1EXIT",  "SW2EXIT"),
    ("SW1GRAY",  "SW2GRAY"),
    ("SW1GRAY1", "SW2GRAY1"),
    ("SW1METAL", "SW2METAL"),
    ("SW1PIPE",  "SW2PIPE"),
    ("SW1SLAD",  "SW2SLAD"),
    ("SW1STARG", "SW2STARG"),
    ("SW1STON1", "SW2STON1"),
    ("SW1STON2", "SW2STON2"),
    ("SW1STONE", "SW2STONE"),
    ("SW1STRTN", "SW2STRTN"),
    # DOOM 1 (registered + retail) additions
    ("SW1BLUE",  "SW2BLUE"),
    ("SW1CMT",   "SW2CMT"),
    ("SW1GARG",  "SW2GARG"),
    ("SW1GSTON", "SW2GSTON"),
    ("SW1HOT",   "SW2HOT"),
    ("SW1LION",  "SW2LION"),
    ("SW1SATYR", "SW2SATYR"),
    ("SW1SKIN",  "SW2SKIN"),
    ("SW1VINE",  "SW2VINE"),
    ("SW1WOOD",  "SW2WOOD"),
]

# Textures we must always keep even if no SIDEDEF references them, because
# the engine looks them up by hardcoded name.
ALWAYS_KEEP_TEXTURES = {"SKY1", "SKY2", "SKY3"}  # SKY4 is E4-only
# Flats that the engine treats specially; F_SKY1 is the sky sentinel.
ALWAYS_KEEP_FLATS = {"F_SKY1"}

# Small intermission lumps that render the post-level scoreboard's text
# and digits — collectively ~21 KiB. Substituting them with transparent
# placeholders makes the scoreboard invisible (kills %, time, par, etc.
# all draw as 1×1 transparent), which made the level-end transition
# feel completely broken. Costs are tiny so we keep them verbatim and
# only substitute the bulk-byte intermission lumps (WIMAP* backgrounds,
# WILV* level-name banners, WIA* animations).
INTERMISSION_KEEP_NAMES = {
    # Digits + percent + colon + minus
    "WINUM0", "WINUM1", "WINUM2", "WINUM3", "WINUM4",
    "WINUM5", "WINUM6", "WINUM7", "WINUM8", "WINUM9",
    "WIPCNT", "WICOLON", "WIMINUS",
    # Labels
    "WITIME", "WIPAR", "WIENTER",
    "WIKILRS", "WIVCTMS", "WIFRGS", "WISUCKS",
    "WISCRT2",     # "secret" header
    # "you are here" pointer-attached labels (used in episode finale)
    "WIOSTK", "WIOSTI", "WIOSTS", "WIOSTF", "WIF",
    # Singleplayer/coop result-line graphics
    "WIP1", "WIP2", "WIP3", "WIP4",
    "WIBP1", "WIBP2", "WIBP3", "WIBP4",
    # World maps — the per-episode background showing the player's
    # progress through the levels. 66 KiB each. Keep WIMAP0 (E1
    # "Knee-Deep in the Dead") and WIMAP1 (E2 "Shores of Hell"); the
    # 132 KiB cost fits a 576 KiB-app + 7552 KiB-wad partition layout
    # without further binary shrinks. WIMAP2 (E3 "Inferno") would
    # need a third 66 KiB which only fits if the binary shrinks below
    # 512 KiB — not currently achievable, so E3 lands on a
    # placeholder background. Worth revisiting once the bake table
    # gets a single-patch compaction pass (102 of 287 textures could
    # halve their collump bytes).
    "WIMAP0", "WIMAP1",
}

ALL_STRIPPABLE = [
    "music", "sfx", "speaker", "sndcfg",
    "demos", "endoom", "screens", "intermission", "titlepic",
    "fullscreens",
    "e4", "e3",
]

# Categories the trim safely DROPS entirely — DOOM never V_DrawPatch's
# these so missing lumps don't crash the renderer.
DROPPABLE_CATS = {"music", "sfx", "speaker", "sndcfg",
                  "demos", "endoom", "e4", "e3"}

# Categories whose lumps DOOM tries to draw via V_DrawPatch /
# W_CacheLumpName. Dropping these crashes; instead we substitute the
# CONTENTS with a 13-byte 1x1 transparent placeholder — the lump name
# is preserved, the drawing call finds the lump, and renders nothing.
SUBSTITUTE_CATS = {"fullscreens", "intermission", "screens", "titlepic"}

# Lumps that are 320x200 fullscreen art — V_DrawPatch'd as the entire
# screen. Substituting them with a transparent placeholder is safe
# (DOOM still finds the lump and the screen just renders blank).
# WIMAP0/1/2 are intermission background "world maps", one per episode.
# NOTE: TITLEPIC and VICTORY2 are deliberately NOT here — the title
# screen and end-of-episode victory splash are visible-on-boot /
# end-of-run artifacts we keep by default. Use --strip titlepic or
# --keep-lumps to override.
FULLSCREEN_LUMPS = {
    "INTERPIC", "BOSSBACK", "ENDPIC",
    "HELP", "HELP1", "HELP2", "CREDIT",
    "PFUB1", "PFUB2",
    # WIMAP0/1/2 (per-episode world maps) intentionally NOT here —
    # they're moved to INTERMISSION_KEEP_NAMES so the player can see
    # progression on the post-level scoreboard. ~200 KiB cost; the
    # app partition slot in partitions.csv was tightened accordingly.
}


def keep_indices_set(keep_indices):
    """Memoised set view of keep_indices for O(1) `in` checks."""
    if not hasattr(keep_indices_set, "_cache"):
        keep_indices_set._cache = (None, None)
    cached_id, cached_set = keep_indices_set._cache
    if cached_id == id(keep_indices):
        return cached_set
    s = set(keep_indices)
    keep_indices_set._cache = (id(keep_indices), s)
    return s


def parse_wad(path):
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
        lumps.append((name, ofs, sz))
    return sig, data, lumps


def categorize_for_strip(lumps):
    """Return list parallel to `lumps`, each entry is the strippable
    category name (or None to keep). Section markers and section-bounded
    lumps (sprites/flats/patches) are NEVER strippable here."""
    cats = [None] * len(lumps)
    in_section = None
    in_map_episode = None
    map_sub_left = 0

    for i, (name, _, _) in enumerate(lumps):
        # Section markers: route to "keep", track depth
        if name.endswith("_START") and (
                name.startswith("S") or name.startswith("F")
                or name.startswith("P") or name in ("SS_START", "FF_START",
                                                     "PP_START", "F1_START",
                                                     "F2_START", "F3_START",
                                                     "P1_START", "P2_START",
                                                     "P3_START")):
            if name[0] == "S":
                in_section = "S"
            elif name[0] == "F":
                in_section = "F"
            elif name[0] == "P":
                in_section = "P"
            continue
        if name.endswith("_END") and in_section:
            in_section = None
            continue

        if in_section:
            # Inside sprite/flat/patch sections: KEEP unconditionally.
            continue

        # Map sub-lumps: stripped iff their map header was stripped.
        if map_sub_left > 0 and name in MAP_SUB_LUMPS:
            if in_map_episode == 4:
                cats[i] = "e4"
            elif in_map_episode == 3:
                cats[i] = "e3"
            map_sub_left -= 1
            if map_sub_left == 0:
                in_map_episode = None
            continue
        if map_sub_left > 0:
            map_sub_left = 0
            in_map_episode = None

        if (len(name) == 4 and name[0] == "E" and name[2] == "M"
                and name[1].isdigit() and name[3].isdigit()):
            episode = int(name[1])
            in_map_episode = episode
            map_sub_left = len(MAP_SUB_LUMPS)
            if episode == 4:
                cats[i] = "e4"
            elif episode == 3:
                cats[i] = "e3"
            continue

        # Audio
        if (name.startswith("D_") and not name.startswith("DS")
                and not name.startswith("DP") and name != "DMXGUS"):
            cats[i] = "music"
            continue
        if name.startswith("DS") and len(name) > 2:
            cats[i] = "sfx"
            continue
        if name.startswith("DP") and len(name) > 2:
            cats[i] = "speaker"
            continue
        if name in ("DMXGUS", "GENMIDI"):
            cats[i] = "sndcfg"
            continue
        if name.startswith("DEMO") and len(name) == 5 and name[4].isdigit():
            cats[i] = "demos"
            continue
        if name == "ENDOOM":
            cats[i] = "endoom"
            continue
        if name in ("HELP", "HELP1", "HELP2", "CREDIT", "VICTORY2",
                    "PFUB1", "PFUB2",
                    "END0", "END1", "END2", "END3", "END4", "END5", "END6"):
            cats[i] = "screens"
            continue
        if name in FULLSCREEN_LUMPS:
            cats[i] = "fullscreens"
            continue
        # Intermission cat: only the BIG byte-cost ones get strippable.
        # Digits/labels/percent stay in the WAD verbatim so the scoreboard
        # at level-end actually renders something readable; without them
        # the score area is fully transparent and the level transition
        # looks like a freeze.
        if name in INTERMISSION_KEEP_NAMES:
            pass    # leave cats[i] = None → kept verbatim
        elif (name in ("INTERPIC", "BOSSBACK")
                or name.startswith("WIMAP")
                or name.startswith("WILV")
                or name.startswith("WIA")
                or name.startswith("WIH")):
            cats[i] = "intermission"
            continue

    return cats


def fmt_mib(n):
    if n < 1024:
        return "%d B" % n
    if n < 1024 * 1024:
        return "%.1f KiB" % (n / 1024.0)
    return "%.2f MiB" % (n / (1024.0 * 1024.0))


def lump_bytes(data, name, lumps):
    """Return the bytes of the LAST lump matching name, or None."""
    last = None
    for (n, ofs, sz) in lumps:
        if n == name:
            last = (ofs, sz)
    if last is None:
        return None
    ofs, sz = last
    return data[ofs:ofs + sz]


def collect_section_lumps(lumps, marker_starts, marker_ends):
    """Return ordered list of (idx, name) inside *_START.._END sections.
    Tracks every nested section (e.g. P1_START inside P_START) so the
    full set of patches/flats is captured."""
    out = []
    depth = 0
    for i, (name, _, _) in enumerate(lumps):
        if name in marker_starts:
            depth += 1
            continue
        if name in marker_ends:
            depth = max(0, depth - 1)
            continue
        if depth > 0:
            out.append((i, name))
    return out


def expand_textures_for_animations(used_textures):
    """If any frame of an animation group is used, add all frames in the
    group. Lump names between `first` and `last` aren't enumerable from
    the animation table alone — the engine relies on WAD directory
    order. We approximate by adding every name that *could* be in the
    group based on the canonical DOOM lump names."""
    canonical = {
        # texture animations — explicit name lists from DOOM source
        ("BLODGR1", "BLODGR4"):   ["BLODGR1", "BLODGR2", "BLODGR3", "BLODGR4"],
        ("SLADRIP1", "SLADRIP3"): ["SLADRIP1", "SLADRIP2", "SLADRIP3"],
        ("BLODRIP1", "BLODRIP4"): ["BLODRIP1", "BLODRIP2", "BLODRIP3", "BLODRIP4"],
        ("FIREWALA", "FIREWALL"): ["FIREWALA", "FIREWALB", "FIREWALL"],
        ("GSTFONT1", "GSTFONT3"): ["GSTFONT1", "GSTFONT2", "GSTFONT3"],
        ("FIRELAV3", "FIRELAVA"): ["FIRELAV3", "FIRELAVA"],
        ("FIREMAG1", "FIREMAG3"): ["FIREMAG1", "FIREMAG2", "FIREMAG3"],
        ("FIREBLU1", "FIREBLU2"): ["FIREBLU1", "FIREBLU2"],
        ("ROCKRED1", "ROCKRED3"): ["ROCKRED1", "ROCKRED2", "ROCKRED3"],
    }
    expanded = set(used_textures)
    for (is_tex, last, first) in ANIM_GROUPS:
        if not is_tex:
            continue
        members = canonical.get((first, last), [first, last])
        if any(m in expanded for m in members):
            expanded.update(members)
    return expanded


def expand_flats_for_animations(used_flats):
    canonical = {
        ("NUKAGE1", "NUKAGE3"): ["NUKAGE1", "NUKAGE2", "NUKAGE3"],
        ("FWATER1", "FWATER4"): ["FWATER1", "FWATER2", "FWATER3", "FWATER4"],
        ("SWATER1", "SWATER4"): ["SWATER1", "SWATER2", "SWATER3", "SWATER4"],
        ("LAVA1",   "LAVA4"):   ["LAVA1", "LAVA2", "LAVA3", "LAVA4"],
        ("BLOOD1",  "BLOOD3"):  ["BLOOD1", "BLOOD2", "BLOOD3"],
    }
    expanded = set(used_flats)
    for (is_tex, last, first) in ANIM_GROUPS:
        if is_tex:
            continue
        members = canonical.get((first, last), [first, last])
        if any(m in expanded for m in members):
            expanded.update(members)
    return expanded


def expand_textures_for_switches(used_textures):
    expanded = set(used_textures)
    for (a, b) in SWITCH_PAIRS:
        if a in expanded or b in expanded:
            expanded.add(a)
            expanded.add(b)
    return expanded


def get_kept_map_indices(lumps, cats, strip):
    """Return list of (header_idx, episode, map_num) for every map header
    we are keeping, plus the indices of its 10 sub-lumps."""
    kept = []
    for i, (name, _, _) in enumerate(lumps):
        if (len(name) == 4 and name[0] == "E" and name[2] == "M"
                and name[1].isdigit() and name[3].isdigit()):
            cat = cats[i]
            if cat is not None and cat in strip:
                continue
            kept.append((i, int(name[1]), int(name[3])))
    return kept


def collect_used_assets(data, lumps, cats, strip):
    """Walk every kept map and collect the set of texture names and
    flat names actually referenced."""
    used_textures = set()
    used_flats = set()
    n = len(lumps)

    for (hdr_idx, _, _) in get_kept_map_indices(lumps, cats, strip):
        # Find SIDEDEFS and SECTORS in the next 10 lumps after the header.
        for j in range(hdr_idx + 1, min(hdr_idx + 11, n)):
            sub_name, sub_ofs, sub_sz = lumps[j]
            if sub_name == "SIDEDEFS":
                # 30 bytes per sidedef: i16 tex_x, i16 tex_y, c8 upper,
                # c8 lower, c8 middle, i16 sector
                if sub_sz % 30 != 0:
                    continue
                payload = data[sub_ofs:sub_ofs + sub_sz]
                for k in range(0, sub_sz, 30):
                    upper = payload[k+4:k+12].rstrip(b"\x00").decode(
                        "ascii", "replace").upper()
                    lower = payload[k+12:k+20].rstrip(b"\x00").decode(
                        "ascii", "replace").upper()
                    middle = payload[k+20:k+28].rstrip(b"\x00").decode(
                        "ascii", "replace").upper()
                    for t in (upper, lower, middle):
                        if t and t != "-":
                            used_textures.add(t)
            elif sub_name == "SECTORS":
                # 26 bytes per sector: i16 floor_h, i16 ceil_h, c8 floor,
                # c8 ceil, i16 light, i16 special, i16 tag
                if sub_sz % 26 != 0:
                    continue
                payload = data[sub_ofs:sub_ofs + sub_sz]
                for k in range(0, sub_sz, 26):
                    floor = payload[k+4:k+12].rstrip(b"\x00").decode(
                        "ascii", "replace").upper()
                    ceil = payload[k+12:k+20].rstrip(b"\x00").decode(
                        "ascii", "replace").upper()
                    if floor:
                        used_flats.add(floor)
                    if ceil:
                        used_flats.add(ceil)
    return used_textures, used_flats


def parse_pnames_and_textures(data, lumps):
    """Return (pnames_list, texture_dict).
    texture_dict maps texture_name -> list of patch indices into pnames."""
    pnames_blob = lump_bytes(data, "PNAMES", lumps)
    if pnames_blob is None:
        return [], {}
    nummappatches = struct.unpack("<I", pnames_blob[:4])[0]
    pnames = []
    for i in range(nummappatches):
        e = pnames_blob[4 + i*8: 4 + (i+1)*8]
        pnames.append(e.rstrip(b"\x00").decode("ascii", "replace").upper())

    texture_dict = {}
    for tex_lump in ("TEXTURE1", "TEXTURE2"):
        blob = lump_bytes(data, tex_lump, lumps)
        if blob is None:
            continue
        numtex = struct.unpack("<I", blob[:4])[0]
        tex_offsets = struct.unpack("<%di" % numtex, blob[4:4 + numtex*4])
        for off in tex_offsets:
            td = blob[off:off+22]
            if len(td) < 22:
                continue
            name = td[:8].rstrip(b"\x00").decode("ascii", "replace").upper()
            patchcount = struct.unpack("<h", td[20:22])[0]
            patches = []
            for pi in range(patchcount):
                pof = off + 22 + pi * 10
                pe = blob[pof:pof+6]
                if len(pe) < 6:
                    break
                _orig_x, _orig_y, patch_idx = struct.unpack("<hhh", pe)
                if 0 <= patch_idx < len(pnames):
                    patches.append(patch_idx)
            texture_dict[name] = patches
    return pnames, texture_dict


def pack_sidedefs_for_map(data, lumps, hdr_idx, n):
    """Returns dict {lump_idx: replacement_payload} for SIDEDEFS +
    LINEDEFS of the map starting at hdr_idx. SIDEDEFS rows are
    deduplicated, LINEDEFS sidenum fields are remapped to point at
    the new compact indices. Sidenum -1 (0xFFFF, "no sidedef") is
    preserved verbatim."""
    sidedefs_idx = None
    linedefs_idx = None
    for j in range(hdr_idx + 1, min(hdr_idx + 11, n)):
        nm = lumps[j][0]
        if nm == "SIDEDEFS": sidedefs_idx = j
        elif nm == "LINEDEFS": linedefs_idx = j
    if sidedefs_idx is None or linedefs_idx is None:
        return {}
    sd_name, sd_ofs, sd_sz = lumps[sidedefs_idx]
    ld_name, ld_ofs, ld_sz = lumps[linedefs_idx]
    if sd_sz == 0 or sd_sz % 30 != 0 or ld_sz == 0 or ld_sz % 14 != 0:
        return {}
    sd_payload = data[sd_ofs:sd_ofs + sd_sz]
    ld_payload = data[ld_ofs:ld_ofs + ld_sz]

    # Build orig_idx → new_idx, emitting unique rows in order of first
    # appearance.
    seen = {}                  # row_bytes → new_idx
    orig_to_new = {}           # orig_idx → new_idx
    packed_rows = bytearray()
    nrows = sd_sz // 30
    for i in range(nrows):
        row = bytes(sd_payload[i*30:(i+1)*30])
        if row in seen:
            orig_to_new[i] = seen[row]
        else:
            new_idx = len(seen)
            seen[row] = new_idx
            orig_to_new[i] = new_idx
            packed_rows.extend(row)

    # Rewrite LINEDEFS: bytes 10-12 = sidenum[0], 12-14 = sidenum[1].
    # 0xFFFF means "no sidedef" — preserve.
    new_ld = bytearray(ld_payload)
    nlines = ld_sz // 14
    for i in range(nlines):
        for slot in (10, 12):
            base = i*14 + slot
            old = struct.unpack_from("<h", new_ld, base)[0]
            if old == -1:
                continue
            if 0 <= old < nrows:
                struct.pack_into("<h", new_ld, base, orig_to_new[old])
            # else leave alone (corrupt input)
    return {sidedefs_idx: bytes(packed_rows), linedefs_idx: bytes(new_ld)}


def pack_blockmap_for_map(data, lumps, hdr_idx, n):
    """Returns {lump_idx: new_payload} for BLOCKMAP of this map, with
    duplicate blocklists merged and the cell-offset table rewritten to
    point at the canonical copy of each blocklist. Returns empty if
    the map has no BLOCKMAP or it's malformed."""
    bm_idx = None
    for j in range(hdr_idx + 1, min(hdr_idx + 11, n)):
        if lumps[j][0] == "BLOCKMAP":
            bm_idx = j; break
    if bm_idx is None:
        return {}
    nm, ofs, sz = lumps[bm_idx]
    if sz < 8: return {}
    bm = data[ofs:ofs+sz]
    bx, by, bw, bh = struct.unpack_from("<hhhh", bm, 0)
    ncells = bw * bh
    if ncells <= 0 or 8 + ncells*2 > sz:
        return {}
    cell_offsets = list(struct.unpack_from("<%dH" % ncells, bm, 8))

    # Read each unique blocklist content (in shorts, including the
    # 0x0000 prefix and 0xFFFF terminator).
    content_by_off = {}
    for co in set(cell_offsets):
        end = co * 2
        cur = end
        while cur + 2 <= sz:
            v = struct.unpack_from("<H", bm, cur)[0]
            cur += 2
            if v == 0xFFFF: break
        content_by_off[co] = bm[end:cur]

    # Group offsets by content; pick one canonical offset per content
    # (the smallest one for determinism).
    by_content = {}
    for off, content in content_by_off.items():
        by_content.setdefault(content, []).append(off)

    # Build new blockmap: header + cell_offsets[ncells] + emitted
    # blocklists. We emit each unique content once.
    out = bytearray()
    out.extend(struct.pack("<hhhh", bx, by, bw, bh))
    cell_ofs_pos = len(out)
    out.extend(b"\x00" * (ncells * 2))   # placeholder cell offsets

    canonical_offset = {}                 # content_bytes → new word offset
    for content in by_content.keys():
        canonical_offset[content] = len(out) // 2
        out.extend(content)

    # Map from old content offset → canonical word offset, then rewrite
    # cell offsets table.
    old_to_new = {}
    for content, olds in by_content.items():
        for o in olds:
            old_to_new[o] = canonical_offset[content]
    for i, co in enumerate(cell_offsets):
        struct.pack_into("<H", out, cell_ofs_pos + i*2, old_to_new[co])

    return {bm_idx: bytes(out)}


def squash_patch(payload):
    """Lossless patch column dedup: vanilla DOOM patches store
    columnofs[width] then post data per column. Multiple columns with
    identical post data can share a single columnofs entry. Returns
    a re-encoded patch with shared columns, or None if input doesn't
    parse cleanly. Bytes-equivalent visually — DOOM's column drawer
    only reads columnofs[col] then the post stream.

    On vanilla DOOM IWADs this typically saves 5–15% on big wall-
    texture patches like W110_1, WALL24_1 (which are full-wall
    repeats of a few columns)."""
    if len(payload) < 8: return None
    try:
        width, height, lo, to = struct.unpack_from("<HHhh", payload, 0)
    except Exception:
        return None
    if width <= 0 or width > 4096:
        return None
    if 8 + width*4 > len(payload):
        return None
    col_offsets = struct.unpack_from("<%dI" % width, payload, 8)

    # For each column, read the full post stream until 0xFF terminator.
    col_streams = []
    for ofs in col_offsets:
        if ofs >= len(payload):
            return None
        cur = ofs
        guard = 0
        while cur < len(payload):
            tdelta = payload[cur]
            if tdelta == 0xFF:
                cur += 1
                break
            if cur + 1 >= len(payload):
                return None
            length = payload[cur + 1]
            # post: topdelta(1) length(1) pad(1) length-pixels pad(1)
            cur += 4 + length
            guard += 1
            if guard > 1024:
                return None
        if cur > len(payload):
            return None
        col_streams.append(bytes(payload[ofs:cur]))

    # Dedup column streams.
    seen = {}                  # stream_bytes → new offset
    new_col_offsets = [0] * width
    body = bytearray()
    body_base = 8 + width * 4
    for i, stream in enumerate(col_streams):
        if stream in seen:
            new_col_offsets[i] = seen[stream]
        else:
            off = body_base + len(body)
            seen[stream] = off
            new_col_offsets[i] = off
            body.extend(stream)

    out = bytearray()
    out.extend(struct.pack("<HHhh", width, height, lo, to))
    out.extend(struct.pack("<%dI" % width, *new_col_offsets))
    out.extend(body)
    if len(out) >= len(payload):
        return None     # no win, keep original
    return bytes(out)


def parse_strip(s):
    if s == "all":
        return set(ALL_STRIPPABLE)
    requested = set(t.strip() for t in s.split(",") if t.strip())
    bad = requested - set(ALL_STRIPPABLE)
    if bad:
        sys.exit("error: unknown strip categories: %s\n  available: %s"
                 % (", ".join(sorted(bad)), ", ".join(ALL_STRIPPABLE)))
    return requested


# A 13-byte 1x1 fully-transparent patch. Doom patch format: 1x1 with
# the single columnofs pointing at a 0xFF end-of-column marker. Because
# the patch is 1x1, V_DrawPatch's `x + width <= SCREENWIDTH` /
# `y + height <= SCREENHEIGHT` bounds check passes for any in-bounds
# (x, y) — so we can safely substitute this for ANY lump V_DrawPatch
# would render, including digit/animation lumps positioned at arbitrary
# screen coords. The transparent column draws nothing; the framebuffer
# retains its prior contents.
def make_tiny_placeholder():
    hdr = struct.pack("<HHhh", 1, 1, 0, 0)        # width=1 height=1
    columnofs = struct.pack("<I", 8 + 4)          # past header + columnofs
    end_marker = b"\xff"
    return hdr + columnofs + end_marker           # = 13 bytes


# A ~1.5 KiB 320x200 SOLID-BLACK patch for fullscreen lump substitution
# (TITLEPIC/INTERPIC/HELP1/etc.). Without this, fullscreen V_DrawPatch
# would draw a transparent 1x1 patch and the previous frame's pixels
# would bleed through, making the title screen look like a smeared
# mess. With a solid-black fill, every pixel of the screen is overwritten
# with palette index 0 (black) — clean and predictable.
#
# Encoding: every column shares the same single-post stream:
#   topdelta=0, length=200, pad, [200 zeros], pad, end-marker(0xFF)
# Total post stream = 1 + 1 + 1 + 200 + 1 + 1 = 204 bytes.
# Total patch = 8 (header) + 320*4 (columnofs) + 204 = 1492 bytes.
def make_fullscreen_black_placeholder():
    width, height = 320, 200
    hdr = struct.pack("<HHhh", width, height, 0, 0)
    post_stream_offset = 8 + width * 4
    columnofs = struct.pack("<%dI" % width, *([post_stream_offset] * width))
    # post: topdelta(1) length(1) pad(1) length-pixels pad(1) end(0xFF)
    post = bytes([0, height, 0]) + b"\x00" * height + bytes([0, 0xFF])
    return hdr + columnofs + post


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", help="Input IWAD")
    ap.add_argument("dest", nargs="?", help="Output IWAD path")
    ap.add_argument("--strip", default="",
                    help='Comma-separated categories, or "all"')
    ap.add_argument("--list-strippable", action="store_true",
                    help="List strip categories and exit")
    ap.add_argument("--prune-unused", action="store_true",
                    help="Drop patches/flats not referenced by any kept map")
    ap.add_argument("--keep-patches", default="",
                    help="Comma-separated patch lump names to force-keep "
                         "(useful if --prune-unused breaks rendering)")
    ap.add_argument("--keep-lumps", default="",
                    help="Comma-separated lump names to force-keep "
                         "verbatim, overriding any --strip / placeholder "
                         "substitution. e.g. --keep-lumps TITLEPIC,VICTORY2")
    ap.add_argument("--pack", action="store_true",
                    help="WadPtr-style lossless packing: dedup SIDEDEFS "
                         "rows + BLOCKMAP blocklists per map, and share "
                         "directory entries for identical lumps. "
                         "Output stays a standard WAD readable by any "
                         "source port. Typical save: 0.5–1 MiB.")
    ap.add_argument("--squash-patches", action="store_true",
                    help="Dedup identical columns within wall-texture "
                         "patches (P_*). Lossless, transparent to the "
                         "renderer. Saves ~50–150 KiB.")
    ap.add_argument("--dry-run", action="store_true",
                    help="Compute the trim but do not write the output WAD")
    args = ap.parse_args()

    if args.list_strippable:
        print("Strippable categories:")
        for c in ALL_STRIPPABLE:
            print("  %s" % c)
        return

    if not args.dest and not args.dry_run:
        sys.exit("error: need an output path (or pass --dry-run)")

    strip = parse_strip(args.strip) if args.strip else set()

    sig, data, lumps = parse_wad(args.source)
    cats = categorize_for_strip(lumps)

    # Per-category policy: DROPPABLE_CATS get cut entirely;
    # SUBSTITUTE_CATS get their content replaced with the tiny
    # transparent placeholder (lump name preserved so V_DrawPatch
    # callers don't I_Error on W_CacheLumpName).
    keep_lumps = set()
    if args.keep_lumps:
        keep_lumps = {s.strip().upper() for s in args.keep_lumps.split(",")
                      if s.strip()}

    placeholder_indices = set()
    keep_indices = []
    dropped_by_cat = {}
    placeholder_savings_by_cat = {}
    for i, ((name, ofs, sz), c) in enumerate(zip(lumps, cats)):
        if c is None or c not in strip:
            keep_indices.append(i)
            continue
        # --keep-lumps escape hatch overrides any strip/substitution.
        if name in keep_lumps:
            keep_indices.append(i)
            continue
        if c in DROPPABLE_CATS:
            dropped_by_cat.setdefault(c, []).append((name, sz))
            continue
        if c in SUBSTITUTE_CATS:
            placeholder_indices.add(i)
            placeholder_savings_by_cat.setdefault(c, []).append((name, sz))
            keep_indices.append(i)
            continue
        # Future-proof: any new category not in either list defaults to
        # drop, but warn so the developer notices.
        sys.stderr.write("warning: category %r has no policy — dropping\n" % c)
        dropped_by_cat.setdefault(c, []).append((name, sz))

    # ── Per-lump prune of unused patches/flats ────────────────────────
    drop_patches = set()
    drop_flats = set()
    used_patches = set()
    used_flats_expanded = set()
    if args.prune_unused:
        used_textures, used_flats = collect_used_assets(
            data, lumps, cats, strip)
        used_textures = expand_textures_for_animations(used_textures)
        used_textures = expand_textures_for_switches(used_textures)
        used_textures |= ALWAYS_KEEP_TEXTURES
        used_flats_expanded = expand_flats_for_animations(used_flats)
        used_flats_expanded |= ALWAYS_KEEP_FLATS

        pnames, texture_dict = parse_pnames_and_textures(data, lumps)
        for tex_name in used_textures:
            for pi in texture_dict.get(tex_name, []):
                if 0 <= pi < len(pnames):
                    used_patches.add(pnames[pi])

        # Force-keep any patch the user listed (escape hatch).
        if args.keep_patches:
            for p in args.keep_patches.split(","):
                p = p.strip().upper()
                if p:
                    used_patches.add(p)

        # Walk P_/F_ sections and drop anything not in the used set.
        section_starts = {"P_START", "PP_START", "P1_START", "P2_START", "P3_START"}
        section_ends = {"P_END", "PP_END", "P1_END", "P2_END", "P3_END"}
        patch_lumps = collect_section_lumps(lumps, section_starts, section_ends)
        flat_section_starts = {"F_START", "FF_START", "F1_START", "F2_START", "F3_START"}
        flat_section_ends = {"F_END", "FF_END", "F1_END", "F2_END", "F3_END"}
        flat_lumps = collect_section_lumps(lumps, flat_section_starts, flat_section_ends)

        # Skip already-strippable categories — those entries are already gone.
        already_dropped = set()
        for cat_items in dropped_by_cat.values():
            for n, _ in cat_items:
                already_dropped.add(n)

        new_keep = []
        for i in keep_indices:
            name, _, sz = lumps[i]
            in_patch_section = any(j == i for j, _ in patch_lumps)
            in_flat_section = any(j == i for j, _ in flat_lumps)
            if in_patch_section and name and name not in used_patches:
                drop_patches.add((name, sz))
                continue
            if in_flat_section and name and name not in used_flats_expanded:
                drop_flats.add((name, sz))
                continue
            new_keep.append(i)
        keep_indices = new_keep

    # Build the output: header (12 B), then lump data back-to-back, then
    # the directory.
    # ── --pack: per-map SIDEDEFS + BLOCKMAP transformations ─────────
    transformed = {}    # lump_idx → replacement bytes
    pack_savings = {"sidedefs": 0, "blockmap": 0}
    if args.pack:
        n_lumps = len(lumps)
        for i in keep_indices:
            name = lumps[i][0]
            if (len(name) == 4 and name[0] == "E" and name[2] == "M"
                    and name[1].isdigit() and name[3].isdigit()):
                sd_pack = pack_sidedefs_for_map(data, lumps, i, n_lumps)
                for k, v in sd_pack.items():
                    if k in transformed: continue
                    orig_sz = lumps[k][2]
                    pack_savings["sidedefs"] += orig_sz - len(v)
                    transformed[k] = v
                bm_pack = pack_blockmap_for_map(data, lumps, i, n_lumps)
                for k, v in bm_pack.items():
                    if k in transformed: continue
                    orig_sz = lumps[k][2]
                    pack_savings["blockmap"] += orig_sz - len(v)
                    transformed[k] = v

    # ── --squash-patches: column dedup within P_*-section patches ───
    squash_savings = 0
    if args.squash_patches:
        patch_starts = {"P_START", "PP_START", "P1_START", "P2_START", "P3_START"}
        patch_ends = {"P_END", "PP_END", "P1_END", "P2_END", "P3_END"}
        depth = 0
        for i, (name, ofs, sz) in enumerate(lumps):
            if name in patch_starts: depth += 1; continue
            if name in patch_ends:   depth = max(0, depth-1); continue
            if depth <= 0 or sz < 8: continue
            if i not in keep_indices_set(keep_indices):
                continue
            if i in transformed:     continue
            payload = data[ofs:ofs + sz]
            sq = squash_patch(payload)
            if sq is not None:
                squash_savings += sz - len(sq)
                transformed[i] = sq

    # ── Writer with content-hash dedup (WadPtr-style lump merging) ──
    out_chunks = [b"\x00" * 12]      # placeholder for IWAD header
    out_directory = []
    cursor = 12
    # Two placeholder forms:
    #   tiny (13 B)  — 1x1 transparent. Safe at any (x,y); used for
    #                  intermission animations / digit lumps drawn at
    #                  arbitrary positions.
    #   black (1492 B) — 320x200 solid-black fill. Used for fullscreen
    #                    lumps that V_DrawPatch's at (0, 0) — TITLEPIC,
    #                    INTERPIC, HELP1, CREDIT, ENDPIC, etc. Without
    #                    a full-screen overwrite, the title cycle's
    #                    transparent placeholder leaves the previous
    #                    frame visible underneath, smearing the menu
    #                    on top of itself.
    # Single tiny (13-byte) transparent placeholder used for every
    # substituted lump. The user prefers transparent over solid-black:
    # title cycle pages then "show through" to whatever's on the
    # framebuffer. Side effect: transitions between fullscreen pages
    # leave the previous page visible. Trade-off accepted.
    tiny_placeholder = make_tiny_placeholder() if placeholder_indices else None
    fullscreen_idx_set = set()           # kept for the report
    for i in placeholder_indices:
        if lumps[i][0] in FULLSCREEN_LUMPS or lumps[i][0] == "TITLEPIC":
            fullscreen_idx_set.add(i)
    seen_payload = {}                # bytes → cursor offset
    dedup_saved = 0
    for i in keep_indices:
        name, ofs, sz = lumps[i]
        if i in placeholder_indices:
            payload = tiny_placeholder
        elif i in transformed:
            payload = transformed[i]
        else:
            payload = data[ofs:ofs + sz]
        sz_out = len(payload)
        if sz_out > 0 and payload in seen_payload:
            # Lump merging: directory entry points at existing copy.
            shared_ofs = seen_payload[payload]
            out_directory.append((shared_ofs, sz_out, name))
            dedup_saved += sz_out
            continue
        out_chunks.append(payload)
        out_directory.append((cursor, sz_out, name))
        if sz_out > 0:
            seen_payload[payload] = cursor
        cursor += sz_out

    infotableofs = cursor
    for ofs_out, sz_out, name in out_directory:
        name_bytes = name.encode("ascii", "replace")[:8].ljust(8, b"\x00")
        out_chunks.append(struct.pack("<II", ofs_out, sz_out) + name_bytes)

    # Patch in the real header
    out_chunks[0] = sig + struct.pack("<II", len(out_directory), infotableofs)

    out_total = sum(len(c) for c in out_chunks)
    src_size = len(data)

    print("Source: %s  (%s, %d lumps, %s)" %
          (args.source, sig.decode(), len(lumps), fmt_mib(src_size)))
    if strip:
        for c in sorted(strip):
            items = dropped_by_cat.get(c, [])
            n = len(items)
            sz = sum(s for _, s in items)
            print("  drop %-13s  %4d lumps   -%s" % (c, n, fmt_mib(sz)))
    if placeholder_savings_by_cat:
        for c in sorted(placeholder_savings_by_cat):
            items = placeholder_savings_by_cat[c]
            total_orig = sum(s for _, s in items)
            total_new = len(tiny_placeholder) * len(items)
            print("  sub  %-13s  %4d lumps   -%s" %
                  (c, len(items), fmt_mib(total_orig - total_new)))
    if args.pack:
        print("  pack sidedefs                    -%s" %
              fmt_mib(pack_savings["sidedefs"]))
        print("  pack blockmap                    -%s" %
              fmt_mib(pack_savings["blockmap"]))
    if args.squash_patches:
        print("  squash patch columns             -%s" %
              fmt_mib(squash_savings))
    if dedup_saved:
        print("  lump merge (shared offsets)      -%s" %
              fmt_mib(dedup_saved))
    if args.prune_unused:
        patch_bytes = sum(s for _, s in drop_patches)
        flat_bytes = sum(s for _, s in drop_flats)
        print("  prune unused patches  %4d lumps   -%s" %
              (len(drop_patches), fmt_mib(patch_bytes)))
        print("  prune unused flats    %4d lumps   -%s" %
              (len(drop_flats), fmt_mib(flat_bytes)))
    print("Result:  %d lumps, %s   (saved %s, %.1f%% smaller)" %
          (len(out_directory), fmt_mib(out_total),
           fmt_mib(src_size - out_total),
           100.0 * (src_size - out_total) / src_size))

    if args.dry_run:
        return

    # Refuse to overwrite without -f? The user's iteration loop expects
    # repeated overwrites — leave it permissive but warn if the dest
    # equals the source.
    if os.path.abspath(args.source) == os.path.abspath(args.dest):
        sys.exit("error: refusing to write output over the source WAD")
    with open(args.dest, "wb") as f:
        for c in out_chunks:
            f.write(c)
    print("Wrote %s (%s)" % (args.dest, fmt_mib(out_total)))


if __name__ == "__main__":
    main()
