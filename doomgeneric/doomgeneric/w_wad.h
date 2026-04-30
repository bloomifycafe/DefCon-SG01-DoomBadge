//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	WAD I/O functions.
//


#ifndef __W_WAD__
#define __W_WAD__

#include <stdio.h>

#include "doomtype.h"
#include "d_mode.h"

#include "w_file.h"


//
// TYPES
//

//
// WADFILE I/O related stuff.
//

typedef struct lumpinfo_s lumpinfo_t;

// ESP32-C6 PORT: lumpinfo_t shrunk from 28 → 8 bytes.
// At 1264 lumps that saves ~25 KiB of .bss → grows the DOOM zone by
// the same amount. The name / position / size fields are derived on
// demand from the WAD directory in flash (the WAD itself is mmap'd,
// so the directory bytes are addressable). The wad_file pointer is
// a single global since this port only ever opens one WAD.
struct lumpinfo_s
{
    void       *cache;
    lumpinfo_t *next;   // hash chain
};


extern lumpinfo_t *lumpinfo;
extern unsigned int numlumps;

// Pointer into flash to the WAD's directory: 16 bytes per entry, layout
// { int filepos; int size; char name[8]; } little-endian. NULL until
// W_AddFile runs. Single global because we only mount one WAD.
extern const byte    *wad_directory;
extern wad_file_t    *wad_file_global;

static inline int lumpinfo_index(const lumpinfo_t *l)
{
    return (int)(l - lumpinfo);
}
static inline const char *lumpinfo_name(const lumpinfo_t *l)
{
    return (const char *)(wad_directory + (size_t)lumpinfo_index(l) * 16 + 8);
}
static inline int lumpinfo_position(const lumpinfo_t *l)
{
    const byte *p = wad_directory + (size_t)lumpinfo_index(l) * 16;
    return (int)((unsigned)p[0] | ((unsigned)p[1] << 8)
                 | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24));
}
static inline int lumpinfo_size(const lumpinfo_t *l)
{
    const byte *p = wad_directory + (size_t)lumpinfo_index(l) * 16 + 4;
    return (int)((unsigned)p[0] | ((unsigned)p[1] << 8)
                 | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24));
}

wad_file_t *W_AddFile (char *filename);

int	W_CheckNumForName (char* name);
int	W_GetNumForName (char* name);

int	W_LumpLength (unsigned int lump);
void    W_ReadLump (unsigned int lump, void *dest);

void*	W_CacheLumpNum (int lump, int tag);
void*	W_CacheLumpName (char* name, int tag);

void    W_GenerateHashTable(void);

extern unsigned int W_LumpNameHash(const char *s);

void    W_ReleaseLumpNum(int lump);
void    W_ReleaseLumpName(char *name);

void W_CheckCorrectIWAD(GameMission_t mission);

#endif
