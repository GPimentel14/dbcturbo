/* blast.h -- interface for blast.c
 * Copyright (C) 2003, 2012 Mark Adler
 * version 1.2, 24 Oct 2012
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the author be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 *
 * Mark Adler    madler@alumni.caltech.edu
 */

/*
 * blast() decompresses the PKWare Data Compression Library (DCL) compressed
 * data format.  It provides functionality similar to the explode() function of
 * the PKWare library, hence the name "blast".
 *
 * This decompressor is based on the excellent format description provided by
 * Ben Rudiak-Gould in comp.compression on August 13, 2001.
 *
 * The input to blast() provided by the in() function may be of any length.
 * blast() returns zero on success, otherwise a negative value.  A return code
 * of -1 means that the literal/length code is incorrect.  -2 means that the
 * distance code is incorrect.  -3 means that the distance was out of bounds.
 * A return of -4 or -5 means that the compressed data is corrupted.
 * A return value of 1 means that there is an output error, and 2 means
 * there is an input error (e.g., truncated input).
 */

#ifndef BLAST_H
#define BLAST_H

#include <stdint.h>

#define MAXBITS 13      /* maximum code length */
#define MAXWIN  4096    /* maximum window size */

/* Chunk size for I/O operations - 64KB provides a good balance */
#define CHUNK   65536

/* DBC file format offsets */
#define HEADER_OFFSET  8   /* bytes to skip in the DBC file header */
#define CRC_OFFSET     4   /* bytes for CRC32 after the header */

/* Maximum error message length */
#define MAX_ERR 511

/*
 * Input function type: reads compressed data into *buf and returns the number
 * of bytes available. Return 0 on end of input.
 */
typedef unsigned (*blast_in)(void *how, unsigned char **buf);

/*
 * Output function type: writes len bytes from buf. Returns non-zero on error.
 */
typedef int (*blast_out)(void *how, unsigned char *buf, unsigned len);

/*
 * Decompress input to output using the provided callback functions.
 * infun/inhow: input callback and opaque data pointer
 * outfun/outhow: output callback and opaque data pointer
 * Returns 0 on success, non-zero on error.
 */
int blast(blast_in infun, void *inhow, blast_out outfun, void *outhow);

#endif /* BLAST_H */
