// zipread.h -- read STORE/DEFLATE members out of a zip archive, CRC32 verified.
// Host build only. The inflate is Mark Adler's puff.c from zlib, under the zlib licence below.
//
// Copyright (C) 2002-2013 Mark Adler
//
// This software is provided 'as-is', without any express or implied warranty. In no event
// will the author be held liable for any damages arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose, including commercial
// applications, and to alter it and redistribute it freely, subject to the following restrictions:
//
// 1. The origin of this software must not be misrepresented; you must not claim that you wrote
//    the original software. If you use this software in a product, an acknowledgment in the
//    product documentation would be appreciated but is not required.
// 2. Altered source versions must be plainly marked as such, and must not be misrepresented as
//    being the original software.
// 3. This notice may not be removed or altered from any source distribution.
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- crc32

static uint32_t zr_crcTable[256];
static int      zr_crcReady = 0;

static void zr_crcInit( void )
{
	for ( uint32_t i = 0; i < 256; i++ ) {
		uint32_t c = i;
		for ( int k = 0; k < 8; k++ )
			c = ( c & 1 ) ? ( 0xedb88320u ^ ( c >> 1 ) ) : ( c >> 1 );
		zr_crcTable[i] = c;
	}
	zr_crcReady = 1;
}

static uint32_t zr_crc32( const uint8_t *buf, size_t len )
{
	if ( !zr_crcReady ) zr_crcInit();
	uint32_t c = 0xffffffffu;
	for ( size_t i = 0; i < len; i++ )
		c = zr_crcTable[( c ^ buf[i] ) & 0xff] ^ ( c >> 8 );
	return c ^ 0xffffffffu;
}

// ---------------------------------------------------------------- inflate
// Canonical-Huffman DEFLATE decoder (RFC 1951).

typedef struct {
	short *count;		// codes per length
	short *symbol;		// symbols in canonical order
} zr_huff;

typedef struct {
	const uint8_t	*in;
	size_t			inlen, incnt;
	long			bitbuf;
	int				bitcnt;
	uint8_t			*out;
	size_t			outlen, outcnt;
	int				err;
} zr_state;

static int zr_bits( zr_state *s, int need )
{
	long val = s->bitbuf;

	while ( s->bitcnt < need ) {
		if ( s->incnt == s->inlen ) { s->err = 1; return 0; }
		val |= (long)( s->in[s->incnt++] ) << s->bitcnt;
		s->bitcnt += 8;
	}
	s->bitbuf = val >> need;
	s->bitcnt -= need;
	return (int)( val & ( ( 1L << need ) - 1 ) );
}

static int zr_decode( zr_state *s, const zr_huff *h )
{
	int code = 0, first = 0, index = 0;

	for ( int len = 1; len <= 15; len++ ) {
		code |= zr_bits( s, 1 );
		if ( s->err ) return -1;
		const int count = h->count[len];
		if ( code - count < first )
			return h->symbol[index + ( code - first )];
		index += count;
		first += count;
		first <<= 1;
		code <<= 1;
	}
	s->err = 1;
	return -1;
}

// returns 0 for a complete code set, <0 when over-subscribed
static int zr_construct( zr_huff *h, const short *length, int n )
{
	short offs[16];

	for ( int len = 0; len <= 15; len++ ) h->count[len] = 0;
	for ( int sym = 0; sym < n; sym++ ) h->count[length[sym]]++;
	if ( h->count[0] == n ) return 0;

	int left = 1;
	for ( int len = 1; len <= 15; len++ ) {
		left <<= 1;
		left -= h->count[len];
		if ( left < 0 ) return left;
	}

	offs[1] = 0;
	for ( int len = 1; len < 15; len++ )
		offs[len + 1] = (short)( offs[len] + h->count[len] );
	for ( int sym = 0; sym < n; sym++ )
		if ( length[sym] ) h->symbol[offs[length[sym]]++] = (short)sym;

	return left;
}

static const short zr_lenBase[29] = {
	3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const short zr_lenExtra[29] = {
	0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const short zr_distBase[30] = {
	1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
	1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const short zr_distExtra[30] = {
	0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static int zr_codes( zr_state *s, const zr_huff *lencode, const zr_huff *distcode )
{
	for ( ;; ) {
		int sym = zr_decode( s, lencode );
		if ( s->err ) return -1;

		if ( sym < 256 ) {
			if ( s->outcnt == s->outlen ) return -2;
			s->out[s->outcnt++] = (uint8_t)sym;
		}
		else if ( sym == 256 ) {
			return 0;
		}
		else {
			sym -= 257;
			if ( sym >= 29 ) return -3;
			const int len = zr_lenBase[sym] + zr_bits( s, zr_lenExtra[sym] );

			sym = zr_decode( s, distcode );
			if ( s->err || sym < 0 || sym >= 30 ) return -4;
			const unsigned dist = (unsigned)( zr_distBase[sym] + zr_bits( s, zr_distExtra[sym] ) );
			if ( dist > s->outcnt ) return -5;
			if ( s->outcnt + len > s->outlen ) return -2;

			for ( int i = 0; i < len; i++ ) {
				s->out[s->outcnt] = s->out[s->outcnt - dist];
				s->outcnt++;
			}
		}
	}
}

static int zr_stored( zr_state *s )
{
	s->bitbuf = 0;
	s->bitcnt = 0;

	if ( s->incnt + 4 > s->inlen ) return -6;
	unsigned len = s->in[s->incnt] | ( s->in[s->incnt + 1] << 8 );
	const unsigned nlen = s->in[s->incnt + 2] | ( s->in[s->incnt + 3] << 8 );
	s->incnt += 4;
	if ( ( len ^ 0xffff ) != nlen ) return -7;
	if ( s->incnt + len > s->inlen ) return -6;
	if ( s->outcnt + len > s->outlen ) return -2;

	memcpy( s->out + s->outcnt, s->in + s->incnt, len );
	s->outcnt += len;
	s->incnt += len;
	return 0;
}

static int zr_fixed( zr_state *s )
{
	static short lencnt[16], lensym[288], distcnt[16], distsym[30];
	static zr_huff lencode = { lencnt, lensym }, distcode = { distcnt, distsym };
	static int built = 0;

	if ( !built ) {
		short lengths[300];
		int sym = 0;
		for ( ; sym < 144; sym++ ) lengths[sym] = 8;
		for ( ; sym < 256; sym++ ) lengths[sym] = 9;
		for ( ; sym < 280; sym++ ) lengths[sym] = 7;
		for ( ; sym < 288; sym++ ) lengths[sym] = 8;
		zr_construct( &lencode, lengths, 288 );
		for ( sym = 0; sym < 30; sym++ ) lengths[sym] = 5;
		zr_construct( &distcode, lengths, 30 );
		built = 1;
	}
	return zr_codes( s, &lencode, &distcode );
}

static int zr_dynamic( zr_state *s )
{
	static const short order[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
	short lengths[316], lencnt[16], lensym[288], distcnt[16], distsym[30];
	zr_huff lencode = { lencnt, lensym }, distcode = { distcnt, distsym };

	const int nlen  = zr_bits( s, 5 ) + 257;
	const int ndist = zr_bits( s, 5 ) + 1;
	const int ncode = zr_bits( s, 4 ) + 4;
	if ( s->err ) return -1;
	if ( nlen > 286 || ndist > 30 ) return -8;

	int index = 0;
	for ( ; index < ncode; index++ ) lengths[order[index]] = (short)zr_bits( s, 3 );
	for ( ; index < 19; index++ ) lengths[order[index]] = 0;
	if ( zr_construct( &lencode, lengths, 19 ) != 0 ) return -9;

	index = 0;
	while ( index < nlen + ndist ) {
		int sym = zr_decode( s, &lencode );
		if ( s->err ) return -1;

		if ( sym < 16 ) {
			lengths[index++] = (short)sym;
		}
		else {
			short len = 0;
			if ( sym == 16 ) {
				if ( index == 0 ) return -10;
				len = lengths[index - 1];
				sym = 3 + zr_bits( s, 2 );
			}
			else if ( sym == 17 ) {
				sym = 3 + zr_bits( s, 3 );
			}
			else {
				sym = 11 + zr_bits( s, 7 );
			}
			if ( index + sym > nlen + ndist ) return -10;
			while ( sym-- ) lengths[index++] = len;
		}
	}
	if ( lengths[256] == 0 ) return -11;

	if ( zr_construct( &lencode, lengths, nlen ) != 0 && ( nlen - lencode.count[0] != 1 ) ) return -12;
	if ( zr_construct( &distcode, lengths + nlen, ndist ) != 0 && ( ndist - distcode.count[0] != 1 ) ) return -13;

	return zr_codes( s, &lencode, &distcode );
}

// raw deflate stream -> out. returns 0 on success
static int zr_inflate( uint8_t *out, size_t outlen, const uint8_t *in, size_t inlen )
{
	zr_state s;
	s.in = in; s.inlen = inlen; s.incnt = 0;
	s.bitbuf = 0; s.bitcnt = 0;
	s.out = out; s.outlen = outlen; s.outcnt = 0;
	s.err = 0;

	int last, ret;
	do {
		last = zr_bits( &s, 1 );
		const int type = zr_bits( &s, 2 );
		if ( s.err ) return -1;
		ret = ( type == 0 ) ? zr_stored( &s )
			: ( type == 1 ) ? zr_fixed( &s )
			: ( type == 2 ) ? zr_dynamic( &s )
			: -14;
		if ( ret != 0 ) return ret;
	} while ( !last );

	return ( s.outcnt == outlen ) ? 0 : -15;
}

// ---------------------------------------------------------------- archive

#define ZR_MAXNAME 384

typedef struct {
	char		name[ZR_MAXNAME];	// lowercased, forward slashes
	uint32_t	crc, csize, usize, offset;
	uint16_t	method;
} zrEntry;

typedef struct {
	FILE		*f;
	zrEntry		*entries;
	int			count;
} zrArchive;

static void zr_close( zrArchive *a )
{
	if ( a->f ) fclose( a->f );
	free( a->entries );
	a->f = NULL;
	a->entries = NULL;
	a->count = 0;
}

static uint16_t zr_u16( const uint8_t *p ) { return (uint16_t)( p[0] | ( p[1] << 8 ) ); }
static uint32_t zr_u32( const uint8_t *p ) {
	return (uint32_t)p[0] | ( (uint32_t)p[1] << 8 ) | ( (uint32_t)p[2] << 16 ) | ( (uint32_t)p[3] << 24 );
}

// 0 on success, otherwise a message in *why
static int zr_open( zrArchive *a, const char *path, const char **why )
{
	memset( a, 0, sizeof( *a ) );
	*why = NULL;

	a->f = fopen( path, "rb" );
	if ( !a->f ) { *why = "cannot open"; return -1; }

	if ( fseek( a->f, 0, SEEK_END ) != 0 ) { *why = "seek failed"; zr_close( a ); return -1; }
	const long fileLen = ftell( a->f );
	if ( fileLen < 22 ) { *why = "too small to be a zip"; zr_close( a ); return -1; }

	// end of central directory lives in the last 64K + its own 22 bytes
	long tailLen = 66 * 1024;
	if ( tailLen > fileLen ) tailLen = fileLen;
	uint8_t *tail = (uint8_t *)malloc( (size_t)tailLen );
	if ( !tail ) { *why = "out of memory"; zr_close( a ); return -1; }
	fseek( a->f, fileLen - tailLen, SEEK_SET );
	if ( fread( tail, 1, (size_t)tailLen, a->f ) != (size_t)tailLen ) {
		free( tail ); *why = "short read"; zr_close( a ); return -1;
	}

	long eocd = -1;
	for ( long i = tailLen - 22; i >= 0; i-- ) {
		if ( zr_u32( tail + i ) == 0x06054b50u ) { eocd = i; break; }
	}
	if ( eocd < 0 ) { free( tail ); *why = "no end-of-directory record"; zr_close( a ); return -1; }

	const uint32_t count   = zr_u16( tail + eocd + 10 );
	const uint32_t cdSize  = zr_u32( tail + eocd + 12 );
	const uint32_t cdStart = zr_u32( tail + eocd + 16 );
	free( tail );

	if ( count == 0xffff || cdStart == 0xffffffffu ) { *why = "zip64 is not supported"; zr_close( a ); return -1; }
	if ( cdSize == 0 || (long)cdStart + (long)cdSize > fileLen ) { *why = "bad directory bounds"; zr_close( a ); return -1; }

	uint8_t *cd = (uint8_t *)malloc( cdSize );
	if ( !cd ) { *why = "out of memory"; zr_close( a ); return -1; }
	fseek( a->f, (long)cdStart, SEEK_SET );
	if ( fread( cd, 1, cdSize, a->f ) != cdSize ) { free( cd ); *why = "short directory read"; zr_close( a ); return -1; }

	a->entries = (zrEntry *)calloc( count ? count : 1, sizeof( zrEntry ) );
	if ( !a->entries ) { free( cd ); *why = "out of memory"; zr_close( a ); return -1; }

	const uint8_t *p = cd, *end = cd + cdSize;
	for ( uint32_t i = 0; i < count; i++ ) {
		if ( p + 46 > end || zr_u32( p ) != 0x02014b50u ) break;
		const unsigned nameLen  = zr_u16( p + 28 );
		const unsigned extraLen = zr_u16( p + 30 );
		const unsigned cmtLen   = zr_u16( p + 32 );
		if ( p + 46 + nameLen + extraLen + cmtLen > end ) break;

		zrEntry *e = &a->entries[a->count];
		if ( nameLen < ZR_MAXNAME ) {
			for ( unsigned c = 0; c < nameLen; c++ ) {
				char ch = (char)p[46 + c];
				if ( ch == '\\' ) ch = '/';
				if ( ch >= 'A' && ch <= 'Z' ) ch = (char)( ch - 'A' + 'a' );
				e->name[c] = ch;
			}
			e->name[nameLen] = '\0';
			e->method = zr_u16( p + 10 );
			e->crc    = zr_u32( p + 16 );
			e->csize  = zr_u32( p + 20 );
			e->usize  = zr_u32( p + 24 );
			e->offset = zr_u32( p + 42 );
			a->count++;
		}
		p += 46 + nameLen + extraLen + cmtLen;
	}

	free( cd );
	return 0;
}

// Reads one member. Caller frees. *outLen gets the uncompressed size.
// Returns NULL and sets *why on any failure, including a CRC mismatch.
static uint8_t *zr_read( zrArchive *a, const zrEntry *e, size_t *outLen, const char **why )
{
	*why = NULL;
	*outLen = 0;

	if ( e->method != 0 && e->method != 8 ) { *why = "unsupported compression method"; return NULL; }

	uint8_t local[30];
	if ( fseek( a->f, (long)e->offset, SEEK_SET ) != 0 ) { *why = "seek failed"; return NULL; }
	if ( fread( local, 1, 30, a->f ) != 30 || zr_u32( local ) != 0x04034b50u ) { *why = "bad local header"; return NULL; }
	const long dataOfs = (long)e->offset + 30 + zr_u16( local + 26 ) + zr_u16( local + 28 );

	uint8_t *raw = (uint8_t *)malloc( e->csize ? e->csize : 1 );
	if ( !raw ) { *why = "out of memory"; return NULL; }
	if ( fseek( a->f, dataOfs, SEEK_SET ) != 0 || fread( raw, 1, e->csize, a->f ) != e->csize ) {
		free( raw ); *why = "short read"; return NULL;
	}

	uint8_t *out;
	if ( e->method == 0 ) {
		if ( e->csize != e->usize ) { free( raw ); *why = "stored size mismatch"; return NULL; }
		out = raw;
	}
	else {
		out = (uint8_t *)malloc( e->usize ? e->usize : 1 );
		if ( !out ) { free( raw ); *why = "out of memory"; return NULL; }
		const int r = zr_inflate( out, e->usize, raw, e->csize );
		free( raw );
		if ( r != 0 ) { free( out ); *why = "inflate failed"; return NULL; }
	}

	if ( zr_crc32( out, e->usize ) != e->crc ) { free( out ); *why = "crc mismatch"; return NULL; }

	*outLen = e->usize;
	return out;
}
