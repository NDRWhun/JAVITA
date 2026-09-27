// texbake -- pre-compress Jedi Academy textures into the Vita DXT cache.
//
// Reads the game's pk3 archives on a PC, produces the same texcache_dxt entries the
// Vita would bake on first sight of each texture, so the device never pays for it.
// The DXT encoder is the game's own (tr_dxt.cpp), so the output is what the device
// would have written, at the higher quality setting a PC can afford.

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <wincodec.h>

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <ctype.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>

#include "zipread.h"
#include "../../src/code/rd-common/tr_dxt.h"

// ---------------------------------------------------------------- cache format
// Must match src/code*/rd-vanilla/tr_image.cpp.

#define TEXCACHE_MAGIC_DXT	0x41435456u		// "VTCA"
#define TEXCACHE_MAX_MIPS	16
#define TEXCACHE_FMT_DXT1	1
#define TEXCACHE_FMT_DXT5	5
#define MAX_TEXTURE_SIZE	4096			// what the GXM backend reports
#define TEXCACHE_FLAG_VALID		0x80000000u
#define TEXCACHE_FLAG_MIPMAP	0x00000001u
#define MAX_QPATH				64

struct texCacheHdrDxt_t {
	uint32_t magic, format, width, height, mipCount, picmip, flags, totalSize;
};

// FNV-1a 64 of a key: how v1 packs, 'JKTD' delta records and loose files are found
static uint64_t FnvName( const char *s )
{
	uint64_t h = 14695981039346656037ULL;
	for ( const unsigned char *p = (const unsigned char *)s; *p; ++p ) {
		h ^= *p;
		h *= 1099511628211ULL;
	}
	return h;
}

// the cache key, as GenerateImageMappingName in tr_image.cpp files a name: lowercase, '/' separators, cut at the first '.'
static std::string CacheKey( const std::string &name )
{
	std::string key;
	for ( size_t i = 0; i < name.size() && i < MAX_QPATH - 1; i++ ) {
		char c = name[i];
		if ( c >= 'A' && c <= 'Z' ) c = (char)( c - 'A' + 'a' );
		if ( c == '.' ) break;
		if ( c == '\\' ) c = '/';
		key += c;
	}
	return key;
}

// ---------------------------------------------------------------- options

struct Options {
	std::string		assets;					// folder holding the pk3 files
	std::string		out;					// folder that receives texcache_dxt
	std::string		copyTo;					// card path, empty = ask
	std::string		card;					// card root to use instead of scanning the drives
	std::string		pack;					// texcache_dxt folder to pack without baking
	std::string		list;					// pack.bin or pack.delta to print
	int				picmip      = 1;		// r_picmip the device runs
	int				highQuality = 1;		// DXT refine passes: 1 = slow and better
	int				force       = 0;		// rebake entries that already exist
	int				threads     = 0;		// 0 = one per core
	int				noCopy      = 0;
};

static Options opt;

// ---------------------------------------------------------------- small helpers

static std::string Lower( std::string s )
{
	for ( char &c : s ) if ( c >= 'A' && c <= 'Z' ) c = (char)( c - 'A' + 'a' );
	return s;
}

static std::string SlashFix( std::string s )
{
	for ( char &c : s ) if ( c == '\\' ) c = '/';
	return s;
}

static std::string StripExt( const std::string &s )
{
	const size_t dot   = s.find_last_of( '.' );
	const size_t slash = s.find_last_of( '/' );
	if ( dot == std::string::npos ) return s;
	if ( slash != std::string::npos && dot < slash ) return s;
	return s.substr( 0, dot );
}

static std::string ExtOf( const std::string &s )
{
	const size_t dot   = s.find_last_of( '.' );
	const size_t slash = s.find_last_of( '/' );
	if ( dot == std::string::npos ) return "";
	if ( slash != std::string::npos && dot < slash ) return "";
	return Lower( s.substr( dot + 1 ) );
}

static bool MakeDirs( const std::string &path )
{
	std::string p;
	for ( size_t i = 0; i <= path.size(); i++ ) {
		if ( i == path.size() || path[i] == '\\' || path[i] == '/' ) {
			if ( i > 0 ) {
				p = path.substr( 0, i );
				if ( !( p.size() == 2 && p[1] == ':' ) )
					CreateDirectoryA( p.c_str(), NULL );
			}
		}
	}
	const DWORD a = GetFileAttributesA( path.c_str() );
	return a != INVALID_FILE_ATTRIBUTES && ( a & FILE_ATTRIBUTE_DIRECTORY );
}

static bool FileExists( const std::string &p )
{
	return GetFileAttributesA( p.c_str() ) != INVALID_FILE_ATTRIBUTES;
}

static bool DirExists( const std::string &p )
{
	const DWORD a = GetFileAttributesA( p.c_str() );
	return a != INVALID_FILE_ATTRIBUTES && ( a & FILE_ATTRIBUTE_DIRECTORY );
}

// ---------------------------------------------------------------- TGA
// Matches the game's LoadTGA: types 2/3/10, 8/24/32 bits, all four scanline orders,
// output is RGBA with row 0 at the top.

static bool DecodeTGA( const uint8_t *buf, size_t len, std::vector<uint8_t> &rgba, int &ow, int &oh )
{
	if ( len < 18 ) return false;

	const int idLen     = buf[0];
	const int cmapType  = buf[1];
	const int imageType = buf[2];
	const int cmapLen   = buf[5] | ( buf[6] << 8 );
	const int w         = buf[12] | ( buf[13] << 8 );
	const int h         = buf[14] | ( buf[15] << 8 );
	const int bpp       = buf[16];
	const int order     = buf[17] & 0x30;

	if ( cmapType != 0 || cmapLen != 0 ) return false;		// colourmaps unsupported, as in game
	if ( imageType != 2 && imageType != 3 && imageType != 10 ) return false;
	if ( bpp != 8 && bpp != 24 && bpp != 32 ) return false;
	if ( w <= 0 || h <= 0 || w > 16384 || h > 16384 ) return false;

	const uint8_t *in  = buf + 18 + idLen;
	const uint8_t *end = buf + len;
	if ( in > end ) return false;

	rgba.assign( (size_t)w * h * 4, 0 );

	const int xStart = ( order == 0x10 || order == 0x30 ) ? w - 1 : 0;
	const int xStep  = ( order == 0x10 || order == 0x30 ) ? -1 : 1;
	const int yStart = ( order == 0x20 || order == 0x30 ) ? 0 : h - 1;
	const int yStep  = ( order == 0x20 || order == 0x30 ) ? 1 : -1;

	int x = xStart, y = yStart, written = 0;
	const int total = w * h;

	// one texel from the stream into the destination grid
	auto put = [&]( int r, int g, int b, int a ) {
		uint8_t *o = &rgba[( (size_t)y * w + x ) * 4];
		o[0] = (uint8_t)r; o[1] = (uint8_t)g; o[2] = (uint8_t)b; o[3] = (uint8_t)a;
		x += xStep;
		if ( x < 0 || x >= w ) { x = xStart; y += yStep; }
		written++;
	};

	if ( imageType == 2 || imageType == 3 ) {
		const size_t need = (size_t)total * ( bpp / 8 );
		if ( in + need > end ) return false;
		for ( int i = 0; i < total; i++ ) {
			if ( bpp == 8 ) { const int v = *in++; put( v, v, v, 255 ); }
			else if ( bpp == 24 ) { const int b = in[0], g = in[1], r = in[2]; in += 3; put( r, g, b, 255 ); }
			else { const int b = in[0], g = in[1], r = in[2], a = in[3]; in += 4; put( r, g, b, a ); }
		}
	}
	else {	// 10: run-length encoded
		if ( bpp != 24 && bpp != 32 ) return false;
		const int px = bpp / 8;
		while ( written < total ) {
			if ( in >= end ) return false;
			const int packet = *in++;
			const int count  = ( packet & 0x7f ) + 1;
			if ( packet & 0x80 ) {
				if ( in + px > end ) return false;
				const int b = in[0], g = in[1], r = in[2];
				const int a = ( px == 4 ) ? in[3] : 255;
				in += px;
				for ( int i = 0; i < count && written < total; i++ ) put( r, g, b, a );
			}
			else {
				if ( in + (size_t)px * count > end ) return false;
				for ( int i = 0; i < count && written < total; i++ ) {
					const int b = in[0], g = in[1], r = in[2];
					const int a = ( px == 4 ) ? in[3] : 255;
					in += px;
					put( r, g, b, a );
				}
			}
		}
	}

	ow = w;
	oh = h;
	return true;
}

// ---------------------------------------------------------------- JPEG / PNG via WIC

static thread_local IWICImagingFactory *tl_wic = NULL;

static bool WicInitThread( void )
{
	if ( tl_wic ) return true;
	CoInitializeEx( NULL, COINIT_MULTITHREADED );
	const HRESULT hr = CoCreateInstance( CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
										 IID_PPV_ARGS( &tl_wic ) );
	return SUCCEEDED( hr ) && tl_wic != NULL;
}

static bool DecodeWIC( const uint8_t *buf, size_t len, std::vector<uint8_t> &rgba, int &ow, int &oh )
{
	if ( !WicInitThread() ) return false;

	IWICStream *stream = NULL;
	IWICBitmapDecoder *dec = NULL;
	IWICBitmapFrameDecode *frame = NULL;
	IWICFormatConverter *conv = NULL;
	bool ok = false;

	if ( SUCCEEDED( tl_wic->CreateStream( &stream ) )
		&& SUCCEEDED( stream->InitializeFromMemory( (BYTE *)buf, (DWORD)len ) )
		&& SUCCEEDED( tl_wic->CreateDecoderFromStream( stream, NULL, WICDecodeMetadataCacheOnDemand, &dec ) )
		&& SUCCEEDED( dec->GetFrame( 0, &frame ) )
		&& SUCCEEDED( tl_wic->CreateFormatConverter( &conv ) )
		&& SUCCEEDED( conv->Initialize( frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
										NULL, 0.0, WICBitmapPaletteTypeCustom ) ) )
	{
		UINT w = 0, h = 0;
		if ( SUCCEEDED( conv->GetSize( &w, &h ) ) && w && h && w <= 16384 && h <= 16384 ) {
			rgba.assign( (size_t)w * h * 4, 0 );
			if ( SUCCEEDED( conv->CopyPixels( NULL, w * 4, (UINT)rgba.size(), rgba.data() ) ) ) {
				ow = (int)w;
				oh = (int)h;
				ok = true;
			}
		}
	}

	if ( conv ) conv->Release();
	if ( frame ) frame->Release();
	if ( dec ) dec->Release();
	if ( stream ) stream->Release();
	return ok;
}

static bool DecodeImage( const std::string &name, const uint8_t *buf, size_t len,
						 std::vector<uint8_t> &rgba, int &w, int &h )
{
	const std::string e = ExtOf( name );
	if ( e == "tga" ) return DecodeTGA( buf, len, rgba, w, h );
	return DecodeWIC( buf, len, rgba, w, h );
}

// ---------------------------------------------------------------- bake
// R_MipMap with r_simpleMipMaps 1, the Vita default.

static void MipMap( uint8_t *in, int width, int height )
{
	if ( width == 1 && height == 1 ) return;

	const int row = width * 4;
	uint8_t *out = in;
	width >>= 1;
	height >>= 1;

	if ( width == 0 || height == 0 ) {
		width += height;
		for ( int i = 0; i < width; i++, out += 4, in += 8 ) {
			out[0] = (uint8_t)( ( in[0] + in[4] ) >> 1 );
			out[1] = (uint8_t)( ( in[1] + in[5] ) >> 1 );
			out[2] = (uint8_t)( ( in[2] + in[6] ) >> 1 );
			out[3] = (uint8_t)( ( in[3] + in[7] ) >> 1 );
		}
		return;
	}

	for ( int i = 0; i < height; i++, in += row ) {
		for ( int j = 0; j < width; j++, out += 4, in += 8 ) {
			out[0] = (uint8_t)( ( in[0] + in[4] + in[row + 0] + in[row + 4] ) >> 2 );
			out[1] = (uint8_t)( ( in[1] + in[5] + in[row + 1] + in[row + 5] ) >> 2 );
			out[2] = (uint8_t)( ( in[2] + in[6] + in[row + 2] + in[row + 6] ) >> 2 );
			out[3] = (uint8_t)( ( in[3] + in[7] + in[row + 3] + in[row + 7] ) >> 2 );
		}
	}
}

// One mip level into dst as row-major, edge-clamped 4x4 blocks. Returns its byte size.
static int EncodeMip( uint8_t *dst, const uint8_t *rgba, int w, int h, int isDxt5 )
{
	const int blockBytes = isDxt5 ? 16 : 8;
	const int bw = ( w + 3 ) >> 2, bh = ( h + 3 ) >> 2;

	for ( int by = 0; by < bh; ++by ) {
		for ( int bx = 0; bx < bw; ++bx ) {
			uint8_t block[64];
			for ( int r = 0; r < 4; ++r ) {
				int sy = by * 4 + r;
				if ( sy >= h ) sy = h - 1;
				const uint8_t *srow = rgba + (size_t)sy * w * 4;
				uint8_t *brow = block + r * 16;
				for ( int c = 0; c < 4; ++c ) {
					int sx = bx * 4 + c;
					if ( sx >= w ) sx = w - 1;
					const uint8_t *s = srow + (size_t)sx * 4;
					brow[c * 4 + 0] = s[0]; brow[c * 4 + 1] = s[1];
					brow[c * 4 + 2] = s[2]; brow[c * 4 + 3] = s[3];
				}
			}
			R_CompressDxtBlock( dst, block, isDxt5, opt.highQuality );
			dst += blockBytes;
		}
	}
	return bw * bh * blockBytes;
}

struct Baked {
	texCacheHdrDxt_t		hdr;
	uint32_t				mipSizes[TEXCACHE_MAX_MIPS];
	std::vector<uint8_t>	blob;
};

// picmip, the size clamp, the alpha scan and the mip chain, exactly as the device does them
static bool BakeImage( std::vector<uint8_t> &pix, int w, int h, bool mipmap, bool allowPicmip, Baked &out )
{
	if ( allowPicmip ) {
		for ( int i = 0; i < opt.picmip; i++ ) {
			MipMap( pix.data(), w, h );
			w >>= 1; h >>= 1;
			if ( w < 1 ) w = 1;
			if ( h < 1 ) h = 1;
		}
	}
	while ( w > MAX_TEXTURE_SIZE || h > MAX_TEXTURE_SIZE ) {
		MipMap( pix.data(), w, h );
		w >>= 1; h >>= 1;
	}

	int isDxt5 = 0;
	for ( int i = 0, c = w * h; i < c; i++ ) {
		if ( pix[(size_t)i * 4 + 3] != 255 ) { isDxt5 = 1; break; }
	}

	// r_gamma, r_intensity and r_overBrightBits are all identity on the Vita, so
	// R_LightScaleTexture is a no-op here and the pixels go straight to the encoder.

	out.blob.assign( (size_t)w * h * 2 + 4096, 0 );
	int ofs = 0, mipCount = 0;
	int mw = w, mh = h;

	out.mipSizes[mipCount] = (uint32_t)EncodeMip( out.blob.data() + ofs, pix.data(), mw, mh, isDxt5 );
	ofs += (int)out.mipSizes[mipCount];
	mipCount++;

	if ( mipmap ) {
		while ( ( mw > 1 || mh > 1 ) && mipCount < TEXCACHE_MAX_MIPS ) {
			MipMap( pix.data(), mw, mh );
			mw >>= 1; mh >>= 1;
			if ( mw < 1 ) mw = 1;
			if ( mh < 1 ) mh = 1;
			out.mipSizes[mipCount] = (uint32_t)EncodeMip( out.blob.data() + ofs, pix.data(), mw, mh, isDxt5 );
			ofs += (int)out.mipSizes[mipCount];
			mipCount++;
		}
	}

	out.hdr.magic     = TEXCACHE_MAGIC_DXT;
	out.hdr.format    = isDxt5 ? TEXCACHE_FMT_DXT5 : TEXCACHE_FMT_DXT1;
	out.hdr.width     = (uint32_t)w;
	out.hdr.height    = (uint32_t)h;
	out.hdr.mipCount  = (uint32_t)mipCount;
	out.hdr.picmip    = (uint32_t)( allowPicmip ? opt.picmip : 0 );
	out.hdr.flags     = TEXCACHE_FLAG_VALID | ( mipmap ? TEXCACHE_FLAG_MIPMAP : 0 );
	out.hdr.totalSize = (uint32_t)ofs;
	out.blob.resize( ofs );
	return true;
}

// ---------------------------------------------------------------- pack.bin
// header, index sorted by key, the key bytes, then every entry byte-for-byte; must match src/code*/rd-vanilla/tr_image.cpp

#define TEXCACHE_PACK_MAGIC		0x50544B4Au		// "JKTP"
#define TEXCACHE_PACK_VERSION	2u				// 2 = keyed by name, 1 = keyed by name hash; both are read, 2 is written
#define TEXCACHE_PACK_VERSION_V1 1u
#define TEXCACHE_PACK_ALIGN		16
#define TEXCACHE_PACK_MAX_COUNT	( 1u << 20 )
#define TEXCACHE_PACK_MAX_ENTRY	( 32u * 1024 * 1024 + 65536 )

struct texCachePackHdr_t {
	uint32_t magic, version, count, namesSize;	// namesSize is a zero pad in v1
};

struct texCachePackEntry_t {					// v2, sorted by key bytes
	uint32_t nameOffset, nameLen, offset, size;
};

struct texCachePackEntryV1_t {					// v1, sorted by hash
	uint64_t nameHash;
	uint32_t offset, size;
};

// memcmp order with the shorter key first on a shared prefix, the order the device binary-searches
static int KeyCmp( const char *a, size_t alen, const char *b, size_t blen )
{
	const int c = memcmp( a, b, alen < blen ? alen : blen );
	if ( c ) return c;
	return alen < blen ? -1 : ( alen > blen ? 1 : 0 );
}

static bool KeyLess( const std::string &a, const std::string &b )
{
	return KeyCmp( a.data(), a.size(), b.data(), b.size() ) < 0;
}

// header, mip table and payload of one entry, checked as the device checks them
static bool EntryOk( const uint8_t *buf, size_t len )
{
	texCacheHdrDxt_t hdr;
	if ( len < sizeof( hdr ) + 4 || len > TEXCACHE_PACK_MAX_ENTRY ) return false;
	memcpy( &hdr, buf, sizeof( hdr ) );
	if ( hdr.magic != TEXCACHE_MAGIC_DXT || hdr.mipCount < 1 || hdr.mipCount > TEXCACHE_MAX_MIPS ) return false;
	if ( len < sizeof( hdr ) + hdr.mipCount * 4 ) return false;
	uint32_t total = 0;
	for ( uint32_t i = 0; i < hdr.mipCount; i++ ) {
		uint32_t m;
		memcpy( &m, buf + sizeof( hdr ) + i * 4, 4 );
		total += m;
	}
	return total == hdr.totalSize && total != 0 && len == sizeof( hdr ) + hdr.mipCount * 4 + total;
}

static void EntryBytes( const Baked &b, std::vector<uint8_t> &out )
{
	out.resize( sizeof( b.hdr ) + b.hdr.mipCount * 4 + b.blob.size() );
	memcpy( out.data(), &b.hdr, sizeof( b.hdr ) );
	memcpy( out.data() + sizeof( b.hdr ), b.mipSizes, b.hdr.mipCount * 4 );
	memcpy( out.data() + sizeof( b.hdr ) + b.hdr.mipCount * 4, b.blob.data(), b.blob.size() );
}

struct PackItem {
	std::string	key;
	uint32_t	offset, size;
};

// a pack.bin under construction: entries stream into a temp file behind space reserved for the index and its keys
struct PackWriter {
	FILE					*f = NULL;
	std::string				path, tmp;
	std::mutex				lock;
	std::vector<PackItem>	index;
	size_t					reserved = 0;
	uint64_t				pos = 0;
	bool					failed = false;
};

static bool PackBegin( PackWriter &w, const std::string &path, size_t reserve )
{
	w.path = path;
	w.tmp  = path + ".tmp";
	w.f = fopen( w.tmp.c_str(), "wb" );
	if ( !w.f ) return false;
	texCachePackHdr_t ph = { TEXCACHE_PACK_MAGIC, TEXCACHE_PACK_VERSION, 0, 0 };
	w.reserved = reserve;
	w.pos = sizeof( ph ) + (uint64_t)reserve * ( sizeof( texCachePackEntry_t ) + MAX_QPATH );
	w.index.reserve( reserve );
	if ( fwrite( &ph, 1, sizeof( ph ), w.f ) != sizeof( ph ) || _fseeki64( w.f, (long long)w.pos, SEEK_SET ) != 0 ) {
		fclose( w.f );
		w.f = NULL;
		DeleteFileA( w.tmp.c_str() );
		return false;
	}
	return true;
}

static bool PackAdd( PackWriter &w, const std::string &key, const uint8_t *entry, size_t size )
{
	std::lock_guard<std::mutex> hold( w.lock );
	if ( !w.f || w.failed || w.index.size() >= w.reserved || key.empty() || key.size() >= MAX_QPATH ) return false;
	const uint64_t aligned = ( w.pos + TEXCACHE_PACK_ALIGN - 1 ) & ~(uint64_t)( TEXCACHE_PACK_ALIGN - 1 );
	if ( aligned + size > 0xFFFFFFFFull ) { w.failed = true; return false; }
	static const uint8_t zero[TEXCACHE_PACK_ALIGN] = { 0 };
	const size_t pad = (size_t)( aligned - w.pos );
	if ( ( pad && fwrite( zero, 1, pad, w.f ) != pad ) || fwrite( entry, 1, size, w.f ) != size ) {
		w.failed = true;
		return false;
	}
	w.index.push_back( { key, (uint32_t)aligned, (uint32_t)size } );
	w.pos = aligned + size;
	return true;
}

// writes the index, the keys and the header, then puts the file in place of any older pack.bin
static bool PackFinish( PackWriter &w )
{
	if ( !w.f ) return false;
	std::stable_sort( w.index.begin(), w.index.end(), []( const PackItem &a, const PackItem &b ) {
		return KeyLess( a.key, b.key );
	} );
	// the device rejects a repeated key; the first one added came from the higher-priority source
	size_t n = 0;
	for ( size_t i = 0; i < w.index.size(); i++ ) {
		if ( n && w.index[n - 1].key == w.index[i].key ) continue;
		w.index[n++] = w.index[i];
	}
	w.index.resize( n );
	std::vector<texCachePackEntry_t> entries( n );
	std::string names;
	uint64_t firstEntry = ~0ull;
	for ( size_t i = 0; i < n; i++ ) {
		entries[i] = { (uint32_t)names.size(), (uint32_t)w.index[i].key.size(), w.index[i].offset, w.index[i].size };
		names += w.index[i].key;
		if ( w.index[i].offset < firstEntry ) firstEntry = w.index[i].offset;
	}
	texCachePackHdr_t ph = { TEXCACHE_PACK_MAGIC, TEXCACHE_PACK_VERSION, (uint32_t)n, (uint32_t)names.size() };
	const size_t idxBytes = n * sizeof( texCachePackEntry_t );
	bool ok = !w.failed && n && sizeof( ph ) + idxBytes + names.size() <= firstEntry
		&& _fseeki64( w.f, 0, SEEK_SET ) == 0
		&& fwrite( &ph, 1, sizeof( ph ), w.f ) == sizeof( ph )
		&& fwrite( entries.data(), 1, idxBytes, w.f ) == idxBytes
		&& fwrite( names.data(), 1, names.size(), w.f ) == names.size();
	ok = ( fclose( w.f ) == 0 ) && ok;
	w.f = NULL;
	if ( !ok || !MoveFileExA( w.tmp.c_str(), w.path.c_str(), MOVEFILE_REPLACE_EXISTING ) ) {
		DeleteFileA( w.tmp.c_str() );
		return false;
	}
	return true;
}

// an existing pack.bin, opened as the device opens it: any defect and it counts as absent
struct PackReader {
	FILE								*f = NULL;
	uint32_t							version = 0;
	std::vector<texCachePackEntry_t>	index;		// v2
	std::string							names;		// the keys the v2 index points into
	std::vector<texCachePackEntryV1_t>	hashed;		// v1
	size_t Count() const { return version == TEXCACHE_PACK_VERSION_V1 ? hashed.size() : index.size(); }
	std::string Key( const texCachePackEntry_t &e ) const { return names.substr( e.nameOffset, e.nameLen ); }
};

static bool PackOpenRead( PackReader &r, const std::string &path )
{
	r.f = fopen( path.c_str(), "rb" );
	if ( !r.f ) return false;
	_fseeki64( r.f, 0, SEEK_END );
	const long long fileSize = _ftelli64( r.f );
	_fseeki64( r.f, 0, SEEK_SET );
	texCachePackHdr_t ph = { 0, 0, 0, 0 };
	bool ok = fread( &ph, 1, sizeof( ph ), r.f ) == sizeof( ph )
		&& ph.magic == TEXCACHE_PACK_MAGIC && ( ph.version == TEXCACHE_PACK_VERSION || ph.version == TEXCACHE_PACK_VERSION_V1 )
		&& ph.count && ph.count <= TEXCACHE_PACK_MAX_COUNT
		&& ( ph.version == TEXCACHE_PACK_VERSION_V1 || ph.namesSize <= ph.count * ( MAX_QPATH - 1 ) );
	const bool v1 = ph.version == TEXCACHE_PACK_VERSION_V1;
	const uint32_t namesSize = v1 ? 0 : ph.namesSize;
	if ( ok ) {
		r.version = ph.version;
		if ( v1 ) {
			r.hashed.resize( ph.count );
			ok = fread( r.hashed.data(), sizeof( texCachePackEntryV1_t ), ph.count, r.f ) == ph.count;
		}
		else {
			r.index.resize( ph.count );
			r.names.resize( namesSize );
			ok = fread( r.index.data(), sizeof( texCachePackEntry_t ), ph.count, r.f ) == ph.count
				&& ( !namesSize || fread( &r.names[0], 1, namesSize, r.f ) == namesSize );
		}
	}
	const uint64_t dataStart = sizeof( ph ) + (uint64_t)ph.count * ( v1 ? sizeof( texCachePackEntryV1_t ) : sizeof( texCachePackEntry_t ) ) + namesSize;
	for ( size_t i = 0; ok && i < ph.count; i++ ) {
		uint32_t offset, size;
		if ( v1 ) {
			const texCachePackEntryV1_t &e = r.hashed[i];
			if ( i && r.hashed[i - 1].nameHash >= e.nameHash ) ok = false;
			offset = e.offset;
			size = e.size;
		}
		else {
			const texCachePackEntry_t &e = r.index[i];
			if ( e.nameLen == 0 || e.nameLen >= MAX_QPATH || e.nameOffset > namesSize || e.nameOffset + e.nameLen > namesSize
				|| ( i && KeyCmp( r.names.data() + r.index[i - 1].nameOffset, r.index[i - 1].nameLen, r.names.data() + e.nameOffset, e.nameLen ) >= 0 ) )
				ok = false;
			offset = e.offset;
			size = e.size;
		}
		if ( size < sizeof( texCacheHdrDxt_t ) + 4 || size > TEXCACHE_PACK_MAX_ENTRY
			|| offset < dataStart || (long long)offset + size > fileSize )
			ok = false;
	}
	if ( !ok ) {
		fclose( r.f );
		r.f = NULL;
		r.version = 0;
		r.index.clear();
		r.names.clear();
		r.hashed.clear();
	}
	return ok;
}

static void PackCloseRead( PackReader &r )
{
	if ( r.f ) fclose( r.f );
	r.f = NULL;
}

// the v2 entry for key, NULL when absent
static const texCachePackEntry_t *PackFind( const PackReader &r, const std::string &key )
{
	const auto it = std::lower_bound( r.index.begin(), r.index.end(), key,
		[&]( const texCachePackEntry_t &e, const std::string &k ) { return KeyCmp( r.names.data() + e.nameOffset, e.nameLen, k.data(), k.size() ) < 0; } );
	if ( it == r.index.end() || KeyCmp( r.names.data() + it->nameOffset, it->nameLen, key.data(), key.size() ) != 0 ) return NULL;
	return &*it;
}

// the v1 entry for hash, NULL when absent
static const texCachePackEntryV1_t *PackFindHash( const PackReader &r, uint64_t hash )
{
	const auto it = std::lower_bound( r.hashed.begin(), r.hashed.end(), hash,
		[]( const texCachePackEntryV1_t &e, uint64_t h ) { return e.nameHash < h; } );
	if ( it == r.hashed.end() || it->nameHash != hash ) return NULL;
	return &*it;
}

// whether the pack holds key: by name in v2, by the hash of the name in v1
static bool PackHas( const PackReader &r, const std::string &key )
{
	return r.version == TEXCACHE_PACK_VERSION_V1 ? PackFindHash( r, FnvName( key.c_str() ) ) != NULL : PackFind( r, key ) != NULL;
}

static bool PackReadEntry( PackReader &r, uint32_t offset, uint32_t size, std::vector<uint8_t> &buf )
{
	buf.resize( size );
	return r.f && _fseeki64( r.f, (long long)offset, SEEK_SET ) == 0
		&& fread( buf.data(), 1, size, r.f ) == size && EntryOk( buf.data(), buf.size() );
}

static PackWriter g_pack;

// ---------------------------------------------------------------- asset index

struct IndexEntry {
	int archive;
	int entry;
};

static std::vector<std::string>				g_pakPaths;
static std::vector<zrArchive>				g_arch;			// main thread only
static std::map<std::string, IndexEntry>	g_index;		// lowercase path -> newest pak

struct Flags {
	bool mipmap    = true;
	bool allowPicmip = true;
	bool allowTC   = true;
};

struct KeyInfo {
	Flags		f;
	std::string	ref;			// the spelling the game resolves the file by
};

static std::map<std::string, KeyInfo>	g_keys;			// cache key -> how it will be loaded
static std::set<std::string>		g_notc;			// cache keys a shader forbids compressing
static std::set<std::string>		g_shaderNames;	// cache keys of every shader stanza
static std::set<std::string>		g_nomip;		// cache keys reached through RegisterShaderNoMip, so mip and picmip are off

static bool IsImageName( const std::string &n )
{
	const std::string e = ExtOf( n );
	return e == "tga" || e == "jpg" || e == "jpeg" || e == "png";
}

// the ui and cgame modules reach these through RegisterShaderNoMip, which hands
// R_FindShader mipRawImage=false and so loads them unmipmapped and unpicmipped
static bool IsNoMipFamily( const std::string &key )
{
	static const char *pre[] = { "fonts/", "levelshots/", "menu/art/", "menu/video/", "ui/assets/",
								 "gfx/menus/", "gfx/hud/", "gfx/mp/", "gfx/2d/numbers/", "gfx/2d/crosshair", NULL };
	for ( int i = 0; pre[i]; i++ ) {
		const size_t n = strlen( pre[i] );
		if ( key.size() > n && key.compare( 0, n, pre[i] ) == 0 ) return true;
	}
	// models/players/<name>/icon_<skin>
	if ( key.compare( 0, 15, "models/players/" ) == 0 ) {
		const size_t slash = key.find( '/', 15 );
		if ( slash != std::string::npos && key.compare( slash + 1, 5, "icon_" ) == 0 ) return true;
	}
	return false;
}


// R_LoadImage order: the name's own extension first, then jpg, png, tga
static bool ResolveImage( const std::string &key, std::string &file )
{
	const std::string lower = Lower( SlashFix( key ) );
	const std::string e = ExtOf( lower );

	if ( !e.empty() && g_index.count( lower ) ) { file = lower; return true; }

	const std::string base = StripExt( lower );
	static const char *order[] = { "jpg", "png", "tga" };
	for ( int i = 0; i < 3; i++ ) {
		const std::string cand = base + "." + order[i];
		if ( g_index.count( cand ) ) { file = cand; return true; }
	}
	return false;
}

// ---------------------------------------------------------------- shader parsing

struct Token {
	std::string	text;
	int			line;
};

static void Tokenize( const char *p, const char *end, std::vector<Token> &out )
{
	int line = 1;
	while ( p < end ) {
		if ( *p == '\n' ) { line++; p++; continue; }
		if ( (unsigned char)*p <= ' ' ) { p++; continue; }
		if ( p + 1 < end && p[0] == '/' && p[1] == '/' ) {
			while ( p < end && *p != '\n' ) p++;
			continue;
		}
		if ( p + 1 < end && p[0] == '/' && p[1] == '*' ) {
			p += 2;
			while ( p + 1 < end && !( p[0] == '*' && p[1] == '/' ) ) { if ( *p == '\n' ) line++; p++; }
			p += 2;
			continue;
		}
		if ( *p == '{' || *p == '}' ) { out.push_back( { std::string( 1, *p ), line } ); p++; continue; }
		const char *s = p;
		while ( p < end && (unsigned char)*p > ' ' && *p != '{' && *p != '}' ) p++;
		out.push_back( { std::string( s, p - s ), line } );
	}
}

static void NoteKey( const std::string &rawKey, const Flags &f )
{
	if ( rawKey.empty() || rawKey[0] == '$' || rawKey[0] == '*' ) return;
	const std::string key = CacheKey( rawKey );
	if ( key.empty() ) return;
	if ( !f.allowTC ) {
		// an uncompressed image is never read from the cache; noted so the by-name pass skips it
		g_notc.insert( key );
		return;
	}
	g_keys.emplace( key, KeyInfo{ f, SlashFix( rawKey ) } );	// first reference wins, as in the game
}

static void ParseShaderText( const std::string &text )
{
	std::vector<Token> t;
	Tokenize( text.c_str(), text.c_str() + text.size(), t );

	size_t i = 0;
	while ( i < t.size() ) {
		if ( t[i].text == "{" || t[i].text == "}" ) { i++; continue; }

		const std::string name = CacheKey( t[i].text );
		i++;
		if ( i >= t.size() || t[i].text != "{" ) continue;
		i++;

		// take the whole block so flags written after the stages still apply
		const size_t start = i;
		int depth = 1;
		while ( i < t.size() && depth > 0 ) {
			if ( t[i].text == "{" ) depth++;
			else if ( t[i].text == "}" ) depth--;
			i++;
		}
		const size_t stop = ( i > start ) ? i - 1 : start;

		g_shaderNames.insert( name );

		Flags f;
		for ( size_t k = start; k < stop; k++ ) {
			const std::string kw = Lower( t[k].text );
			if ( kw == "nomipmaps" ) { f.mipmap = false; f.allowPicmip = false; }
			else if ( kw == "nopicmip" ) f.allowPicmip = false;
			else if ( kw == "notc" ) f.allowTC = false;
		}

		for ( size_t k = start; k < stop; k++ ) {
			const std::string kw = Lower( t[k].text );

			if ( ( kw == "map" || kw == "clampmap" ) && k + 1 < stop ) {
				NoteKey( t[k + 1].text, f );
			}
			else if ( ( kw == "animmap" || kw == "clampanimmap" || kw == "oneshotanimmap" ) && k + 2 < stop ) {
				const int line = t[k].line;
				for ( size_t m = k + 2; m < stop && t[m].line == line; m++ )
					NoteKey( t[m].text, f );
			}
			else if ( kw == "skyparms" && k + 1 < stop && t[k + 1].text != "-" ) {
				static const char *suf[6] = { "rt", "lf", "bk", "ft", "up", "dn" };
				Flags sky;					// sky faces always load mipmapped and picmipped
				sky.allowTC = f.allowTC;
				for ( int s = 0; s < 6; s++ )
					NoteKey( t[k + 1].text + "_" + suf[s], sky );
			}
		}
	}
}

// .menu files name their art through background / asset_shader, both of which
// ui_shared.c registers with RegisterShaderNoMip
static void ParseMenuText( const std::string &text )
{
	std::vector<Token> t;
	Tokenize( text.c_str(), text.c_str() + text.size(), t );
	for ( size_t i = 0; i + 1 < t.size(); i++ ) {
		const std::string kw = Lower( t[i].text );
		if ( kw != "background" && kw != "asset_shader" ) continue;
		std::string v = t[i + 1].text;
		if ( v.size() >= 2 && v.front() == '"' && v.back() == '"' ) v = v.substr( 1, v.size() - 2 );
		if ( v.empty() || v[0] == '$' || v[0] == '*' ) continue;
		g_nomip.insert( CacheKey( v ) );
	}
}

// RegisterShaderNoMip string literals in both games' modules, read from the tree this tool ships in
static int ScanNoMipLiterals( const std::string &exePath )
{
	size_t cut = exePath.find_last_of( "\\/" );
	if ( cut == std::string::npos ) return 0;
	cut = exePath.find_last_of( "\\/", cut - 1 );		// tools\texbake -> tools
	if ( cut == std::string::npos ) return 0;
	cut = exePath.find_last_of( "\\/", cut - 1 );		// tools -> repo root
	if ( cut == std::string::npos ) return 0;
	const std::string root = exePath.substr( 0, cut );

	static const char *dirs[] = { "\\src\\codemp\\ui", "\\src\\codemp\\cgame", "\\src\\codemp\\client", "\\src\\codemp\\game",
								  "\\src\\code\\ui", "\\src\\code\\cgame", "\\src\\code\\client", "\\src\\code\\game", NULL };
	const size_t before = g_nomip.size();

	for ( int d = 0; dirs[d]; d++ ) {
		WIN32_FIND_DATAA fd;
		const std::string pat = root + dirs[d] + "\\*.c*";
		const HANDLE h = FindFirstFileA( pat.c_str(), &fd );
		if ( h == INVALID_HANDLE_VALUE ) continue;
		do {
			FILE *f = fopen( ( root + dirs[d] + "\\" + fd.cFileName ).c_str(), "rb" );
			if ( !f ) continue;
			std::string txt;
			char buf[8192];
			size_t n;
			while ( ( n = fread( buf, 1, sizeof( buf ), f ) ) > 0 ) txt.append( buf, n );
			fclose( f );

			for ( size_t p = txt.find( "RegisterShaderNoMip" ); p != std::string::npos;
				  p = txt.find( "RegisterShaderNoMip", p + 1 ) ) {
				const size_t q = txt.find( '"', p );
				if ( q == std::string::npos || q > p + 64 ) continue;
				const size_t e = txt.find( '"', q + 1 );
				if ( e == std::string::npos ) continue;
				const std::string v = txt.substr( q + 1, e - q - 1 );
				if ( v.empty() || v[0] == '$' || v[0] == '*' || v.find( '%' ) != std::string::npos ) continue;
				g_nomip.insert( CacheKey( v ) );
			}
		} while ( FindNextFileA( h, &fd ) );
		FindClose( h );
	}
	return (int)( g_nomip.size() - before );
}

// ---------------------------------------------------------------- job list

struct Job {
	std::string					file;		// resolved archive member
	int							archive;
	int							entry;
	bool						mipmap;
	bool						allowPicmip;
	std::vector<std::string>	keys;		// spellings that map to this bake
};

static std::vector<Job>		g_jobs;
static std::atomic<int>		g_next( 0 );
static std::atomic<int>		g_done( 0 );
static std::atomic<int>		g_written( 0 );
static std::atomic<int>		g_skipped( 0 );
static std::atomic<int>		g_failed( 0 );
static std::atomic<long long> g_bytes( 0 );
static std::mutex			g_logMutex;
static std::vector<std::string> g_problems;

static void Problem( const std::string &s )
{
	std::lock_guard<std::mutex> lock( g_logMutex );
	if ( g_problems.size() < 40 ) g_problems.push_back( s );
}

static void Worker( void )
{
	// every thread gets its own handles; the archive reader holds one file position
	std::vector<zrArchive> arch( g_pakPaths.size() );
	for ( size_t i = 0; i < g_pakPaths.size(); i++ ) {
		const char *why = NULL;
		if ( zr_open( &arch[i], g_pakPaths[i].c_str(), &why ) != 0 )
			memset( &arch[i], 0, sizeof( arch[i] ) );
	}

	std::vector<uint8_t> pix, entry;
	Baked baked;

	auto process = [&]( const Job &job ) {
		zrArchive *a = &arch[job.archive];
		if ( !a->f || job.entry >= a->count || job.file != a->entries[job.entry].name ) {
			g_failed++;
			Problem( job.file + ": archive read back differently" );
			return;
		}

		const char *why = NULL;
		size_t rawLen = 0;
		uint8_t *raw = zr_read( a, &a->entries[job.entry], &rawLen, &why );
		if ( !raw ) {
			g_failed++;
			Problem( job.file + ": " + ( why ? why : "unreadable" ) );
			return;
		}

		int w = 0, h = 0;
		const bool okDecode = DecodeImage( job.file, raw, rawLen, pix, w, h );
		free( raw );
		if ( !okDecode ) {
			g_failed++;
			Problem( job.file + ": cannot decode" );
			return;
		}

		// the game refuses these outright, so a cache entry would only be dead weight
		if ( ( w & ( w - 1 ) ) || ( h & ( h - 1 ) ) ) {
			g_skipped++;
			return;
		}

		BakeImage( pix, w, h, job.mipmap, job.allowPicmip, baked );
		EntryBytes( baked, entry );

		for ( const std::string &key : job.keys ) {
			if ( PackAdd( g_pack, key, entry.data(), entry.size() ) ) {
				g_written++;
				g_bytes += (long long)entry.size();
			}
			else {
				g_failed++;
				Problem( key + ": write failed" );
			}
		}
	};

	for ( ;; ) {
		const int idx = g_next++;
		if ( idx >= (int)g_jobs.size() ) break;
		process( g_jobs[idx] );
		g_done++;
	}

	for ( size_t i = 0; i < arch.size(); i++ ) zr_close( &arch[i] );
	if ( tl_wic ) { tl_wic->Release(); tl_wic = NULL; CoUninitialize(); }
}

// ---------------------------------------------------------------- setup

static void FindPaks( const std::string &dir, std::vector<std::string> &out )
{
	WIN32_FIND_DATAA fd;
	const HANDLE hFind = FindFirstFileA( ( dir + "\\*.pk3" ).c_str(), &fd );
	if ( hFind == INVALID_HANDLE_VALUE ) return;
	std::vector<std::string> names;
	do {
		if ( !( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) )
			names.push_back( fd.cFileName );
	} while ( FindNextFileA( hFind, &fd ) );
	FindClose( hFind );

	// ascending, so the last one loaded is the one the game searches first
	std::sort( names.begin(), names.end(), []( const std::string &a, const std::string &b ) {
		return Lower( a ) < Lower( b );
	} );
	for ( const std::string &n : names ) out.push_back( dir + "\\" + n );
}

static std::string TrimSlashes( std::string s )
{
	while ( !s.empty() && ( s.back() == '\\' || s.back() == '/' ) ) s.pop_back();
	return s;
}

static std::string BackslashFix( std::string s )
{
	for ( char &c : s ) if ( c == '/' ) c = '\\';
	return s;
}

// a REG_SZ value, empty when the key or value is absent
static std::string RegString( HKEY root, const char *sub, const char *name )
{
	HKEY k;
	if ( RegOpenKeyExA( root, sub, 0, KEY_READ, &k ) != ERROR_SUCCESS ) return "";
	char buf[1024];
	DWORD type = 0, len = sizeof( buf ) - 1;
	std::string s;
	if ( RegQueryValueExA( k, name, NULL, &type, (BYTE *)buf, &len ) == ERROR_SUCCESS && type == REG_SZ ) {
		buf[len] = '\0';
		s = buf;
	}
	RegCloseKey( k );
	return s;
}

// every "path" value in a Steam libraryfolders.vdf
static void SteamLibraries( const std::string &vdf, std::vector<std::string> &out )
{
	FILE *f = fopen( vdf.c_str(), "rb" );
	if ( !f ) return;
	std::string txt;
	char buf[8192];
	size_t n;
	while ( ( n = fread( buf, 1, sizeof( buf ), f ) ) > 0 ) txt.append( buf, n );
	fclose( f );

	for ( size_t p = txt.find( "\"path\"" ); p != std::string::npos; p = txt.find( "\"path\"", p + 6 ) ) {
		const size_t q = txt.find( '"', p + 6 );
		if ( q == std::string::npos ) break;
		const size_t e = txt.find( '"', q + 1 );
		if ( e == std::string::npos ) break;
		std::string v;
		for ( size_t i = q + 1; i < e; i++ ) {
			if ( txt[i] == '\\' && i + 1 < e ) i++;
			v += txt[i];
		}
		if ( !v.empty() ) out.push_back( v );
		p = e;
	}
}

// the Steam client's libraries first, then the usual install folders
static std::string FindGame( void )
{
	static const char *suffix = "\\steamapps\\common\\Jedi Academy\\GameData\\base";

	std::vector<std::string> libs;
	const std::string steam = RegString( HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath" );
	const std::string steam64 = RegString( HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Valve\\Steam", "InstallPath" );
	if ( !steam.empty() ) libs.push_back( steam );
	if ( !steam64.empty() ) libs.push_back( steam64 );
	if ( !steam.empty() ) SteamLibraries( TrimSlashes( BackslashFix( steam ) ) + "\\steamapps\\libraryfolders.vdf", libs );
	if ( !steam64.empty() ) SteamLibraries( TrimSlashes( BackslashFix( steam64 ) ) + "\\steamapps\\libraryfolders.vdf", libs );

	std::set<std::string> tried;
	for ( const std::string &lib : libs ) {
		const std::string base = TrimSlashes( BackslashFix( lib ) ) + suffix;
		if ( !tried.insert( Lower( base ) ).second ) continue;
		if ( FileExists( base + "\\assets0.pk3" ) ) return base;
	}

	static const char *guesses[] = {
		"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Jedi Academy\\GameData\\base",
		"C:\\Program Files\\Steam\\steamapps\\common\\Jedi Academy\\GameData\\base",
		"D:\\SteamLibrary\\steamapps\\common\\Jedi Academy\\GameData\\base",
		"E:\\SteamLibrary\\steamapps\\common\\Jedi Academy\\GameData\\base",
		"F:\\SteamLibrary\\steamapps\\common\\Jedi Academy\\GameData\\base",
		"C:\\Program Files (x86)\\LucasArts\\Star Wars Jedi Knight Jedi Academy\\GameData\\base",
	};
	for ( const char *g : guesses ) {
		if ( FileExists( std::string( g ) + "\\assets0.pk3" ) ) return g;
	}
	return "";
}

static std::string DetectCard( void )
{
	char drives[256];
	const DWORD n = GetLogicalDriveStringsA( sizeof( drives ), drives );
	for ( DWORD i = 0; i < n; ) {
		const std::string d = &drives[i];
		i += (DWORD)d.size() + 1;
		if ( d.empty() ) break;
		std::string root = d;
		while ( !root.empty() && ( root.back() == '\\' || root.back() == '/' ) ) root.pop_back();
		if ( DirExists( root + "\\data\\JAVITA" ) || DirExists( root + "\\app\\JAVITA001" )
			|| DirExists( root + "\\app\\JAMPV0001" ) )
			return root;
	}
	return "";
}

static void PrintSize( long long bytes, char *out )
{
	if ( bytes >= 1024LL * 1024 * 1024 ) sprintf( out, "%.2f GB", bytes / ( 1024.0 * 1024 * 1024 ) );
	else sprintf( out, "%.1f MB", bytes / ( 1024.0 * 1024 ) );
}

// ---------------------------------------------------------------- loose entries
// <xx>\<hash>.bin and <hash>.bin, as earlier builds wrote them; read only, so an old card still folds

struct PackSource {
	uint64_t	hash;
	std::string	path;
};

// <16 hex>.bin, as both the sharded and the flat layouts name their entries
static bool ParseEntryName( const char *name, uint64_t &hash )
{
	if ( strlen( name ) != 20 || _stricmp( name + 16, ".bin" ) != 0 ) return false;
	for ( int i = 0; i < 16; i++ ) if ( !isxdigit( (unsigned char)name[i] ) ) return false;
	hash = strtoull( std::string( name, 16 ).c_str(), NULL, 16 );
	return true;
}

static void ListEntries( const std::string &dir, std::set<uint64_t> &seen, std::vector<PackSource> &out )
{
	WIN32_FIND_DATAA fd;
	const HANDLE h = FindFirstFileA( ( dir + "\\*.bin" ).c_str(), &fd );
	if ( h == INVALID_HANDLE_VALUE ) return;
	do {
		uint64_t hash;
		if ( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) continue;
		if ( !ParseEntryName( fd.cFileName, hash ) ) continue;
		if ( !seen.insert( hash ).second ) continue;
		out.push_back( { hash, dir + "\\" + fd.cFileName } );
	} while ( FindNextFileA( h, &fd ) );
	FindClose( h );
}

// the file as the device validates it: header, mip table, payload, nothing else
static bool ReadEntry( const std::string &path, std::vector<uint8_t> &buf )
{
	FILE *f = fopen( path.c_str(), "rb" );
	if ( !f ) return false;
	fseek( f, 0, SEEK_END );
	const long len = ftell( f );
	fseek( f, 0, SEEK_SET );
	if ( len < (long)( sizeof( texCacheHdrDxt_t ) + 4 ) || len > 64L * 1024 * 1024 ) { fclose( f ); return false; }
	buf.resize( (size_t)len );
	const bool ok = fread( buf.data(), 1, (size_t)len, f ) == (size_t)len;
	fclose( f );
	return ok && EntryOk( buf.data(), buf.size() );
}

// ---------------------------------------------------------------- pack.delta
// 'JKTE' records = u32 magic, u32 nameLen, u32 size, u32 pad, key, entry
// 'JKTD' records = u32 magic, u64 hash, u32 size, u32 pad, entry

#define TEXCACHE_DELTA_MAGIC		0x45544B4Au		// "JKTE"
#define TEXCACHE_DELTA_MAGIC_V1		0x44544B4Au		// "JKTD"
#define TEXCACHE_DELTA_HDR_SIZE		16
#define TEXCACHE_DELTA_HDR_SIZE_V1	20

struct DeltaRec {
	uint64_t offset;
	uint32_t size;
};

struct DeltaIndex {
	std::map<std::string, DeltaRec>	named;			// newest 'JKTE' record per key
	std::map<uint64_t, DeltaRec>	hashed;			// newest 'JKTD' record per hash
	int								records = 0;
	size_t Count() const { return named.size() + hashed.size(); }
};

// the record at pos: its key or hash, entry size and entry position; false when it is not a whole record inside the file
static bool DeltaParse( FILE *f, uint64_t pos, uint64_t fileSize, std::string &key, uint64_t &hash, uint32_t &size, uint64_t &entryPos )
{
	uint8_t b[TEXCACHE_DELTA_HDR_SIZE + MAX_QPATH];
	if ( pos + TEXCACHE_DELTA_HDR_SIZE > fileSize || _fseeki64( f, (long long)pos, SEEK_SET ) != 0 ) return false;
	const size_t have = fread( b, 1, sizeof( b ), f );
	if ( have < TEXCACHE_DELTA_HDR_SIZE ) return false;
	uint32_t magic, hdrSize, nameLen = 0;
	memcpy( &magic, b, 4 );
	key.clear();
	hash = 0;
	if ( magic == TEXCACHE_DELTA_MAGIC ) {
		hdrSize = TEXCACHE_DELTA_HDR_SIZE;
		memcpy( &nameLen, b + 4, 4 );
		memcpy( &size, b + 8, 4 );
		if ( nameLen == 0 || nameLen >= MAX_QPATH || have < hdrSize + nameLen ) return false;
		key.assign( (const char *)b + hdrSize, nameLen );
	}
	else if ( magic == TEXCACHE_DELTA_MAGIC_V1 ) {
		hdrSize = TEXCACHE_DELTA_HDR_SIZE_V1;
		if ( have < hdrSize ) return false;
		memcpy( &hash, b + 4, 8 );
		memcpy( &size, b + 12, 4 );
	}
	else {
		return false;
	}
	entryPos = pos + hdrSize + nameLen;
	return size >= sizeof( texCacheHdrDxt_t ) + 4 && size <= TEXCACHE_PACK_MAX_ENTRY && entryPos + size <= fileSize;
}

// the newest record of every key, walked from the start and stopped at the first record that is not whole, as the device does
static FILE *DeltaOpen( const std::string &path, DeltaIndex &ix )
{
	FILE *f = fopen( path.c_str(), "rb" );
	if ( !f ) return NULL;
	_fseeki64( f, 0, SEEK_END );
	const uint64_t fileSize = (uint64_t)_ftelli64( f );
	uint64_t pos = 0;
	ix = DeltaIndex();
	for ( ;; ) {
		std::string key;
		uint64_t hash, entryPos;
		uint32_t size;
		if ( !DeltaParse( f, pos, fileSize, key, hash, size, entryPos ) ) break;
		if ( key.empty() ) ix.hashed[hash] = { entryPos, size };
		else ix.named[key] = { entryPos, size };
		ix.records++;
		pos = entryPos + size;
	}
	return f;
}

// ---------------------------------------------------------------- fold
// ordered sources into one fresh pack.bin; the first source that holds a key supplies it

static std::map<uint64_t, std::string> g_hashNames;		// hash -> key, names hash-keyed entries

// the game's key set hashed, so v1 packs, 'JKTD' records and loose files can be named
static void BuildHashNames( void )
{
	if ( !g_hashNames.empty() ) return;
	for ( const auto &kv : g_keys ) g_hashNames[FnvName( kv.first.c_str() )] = kv.first;
}

static bool NameOfHash( uint64_t hash, std::string &key )
{
	const auto it = g_hashNames.find( hash );
	if ( it == g_hashNames.end() ) return false;
	key = it->second;
	return true;
}

struct FoldSource {
	enum Kind { DELTA, PACK, LOOSE };
	Kind		kind;
	std::string	path;		// a pack.delta, a pack.bin, or the folder that holds loose entries
	const char	*label;		// how the summary names this source
};

struct FoldResult {
	std::vector<int>	from;				// entries taken from each source
	int					bad = 0;			// entries no source could supply intact
	int					unnamed = 0;		// hashed entries no known key matches
	int					count = 0;
	uint64_t			bytes = 0;
	bool				unchanged = false;	// the destination already held everything, nothing written
	bool				empty = false;		// no source had a single entry
};

static bool FoldPacks( const std::vector<FoldSource> &src, const std::string &dest, FoldResult &res )
{
	const size_t n = src.size();
	std::vector<FILE *>								deltaF( n, NULL );
	std::vector<DeltaIndex>							delta( n );
	std::vector<PackReader>							packs( n );
	std::vector<std::vector<PackSource>>			loose( n );
	size_t candidates = 0;
	res = FoldResult();
	res.from.assign( n, 0 );
	BuildHashNames();

	for ( size_t i = 0; i < n; i++ ) {
		switch ( src[i].kind ) {
		case FoldSource::DELTA:
			deltaF[i] = DeltaOpen( src[i].path, delta[i] );
			if ( deltaF[i] ) printf( "  %-24s %d records, %d textures\n", src[i].label, delta[i].records, (int)delta[i].Count() );
			candidates += delta[i].Count();
			break;
		case FoldSource::PACK:
			if ( PackOpenRead( packs[i], src[i].path ) )
				printf( "  %-24s %d entries%s\n", src[i].label, (int)packs[i].Count(), packs[i].version == TEXCACHE_PACK_VERSION_V1 ? " (keyed by hash)" : "" );
			candidates += packs[i].Count();
			break;
		case FoldSource::LOOSE: {
			std::set<uint64_t> seen;
			for ( int s = 0; s < 256; s++ ) {
				char sub[16];
				sprintf( sub, "\\%02x", s );
				ListEntries( src[i].path + sub, seen, loose[i] );
			}
			ListEntries( src[i].path, seen, loose[i] );		// the flat layout that predates sharding
			if ( !loose[i].empty() ) printf( "  %-24s %d entries\n", src[i].label, (int)loose[i].size() );
			candidates += loose[i].size();
			break;
		}
		}
	}

	auto closeAll = [&]() {
		for ( size_t i = 0; i < n; i++ ) {
			if ( deltaF[i] ) fclose( deltaF[i] );
			deltaF[i] = NULL;
			PackCloseRead( packs[i] );
		}
	};

	if ( !candidates ) {
		res.empty = true;
		closeAll();
		return false;
	}

	// no rewrite when the destination is keyed by name, nothing outranks it and every other key is already in it
	int destIdx = -1;
	for ( size_t i = 0; i < n; i++ ) {
		if ( src[i].kind == FoldSource::PACK && packs[i].f && packs[i].version == TEXCACHE_PACK_VERSION
			&& Lower( src[i].path ) == Lower( dest ) ) destIdx = (int)i;
	}
	if ( destIdx >= 0 ) {
		bool same = true;
		std::string key;
		for ( size_t i = 0; same && i < n; i++ ) {
			if ( (int)i == destIdx ) continue;
			const bool above = (int)i < destIdx;
			for ( const auto &kv : delta[i].named ) if ( above || !PackHas( packs[destIdx], kv.first ) ) { same = false; break; }
			for ( const auto &kv : delta[i].hashed ) if ( NameOfHash( kv.first, key ) && ( above || !PackHas( packs[destIdx], key ) ) ) { same = false; break; }
			for ( const texCachePackEntry_t &e : packs[i].index ) if ( above || !PackHas( packs[destIdx], packs[i].Key( e ) ) ) { same = false; break; }
			for ( const texCachePackEntryV1_t &e : packs[i].hashed ) if ( NameOfHash( e.nameHash, key ) && ( above || !PackHas( packs[destIdx], key ) ) ) { same = false; break; }
			for ( const PackSource &s : loose[i] ) if ( NameOfHash( s.hash, key ) && ( above || !PackHas( packs[destIdx], key ) ) ) { same = false; break; }
		}
		if ( same ) {
			res.unchanged = true;
			res.count = (int)packs[destIdx].Count();
			_fseeki64( packs[destIdx].f, 0, SEEK_END );
			res.bytes = (uint64_t)_ftelli64( packs[destIdx].f );
			closeAll();
			return true;
		}
	}

	PackWriter w;
	if ( !PackBegin( w, dest, candidates ) ) {
		printf( "  cannot write %s\n", w.tmp.c_str() );
		closeAll();
		return false;
	}

	std::set<std::string> seen;
	std::set<uint64_t> unnamed;
	std::vector<uint8_t> buf;
	std::string key;
	int done = 0;
	auto progress = [&]() {
		if ( ( ++done & 255 ) == 0 ) {
			printf( "\r  packing %d / %d ", done, (int)candidates );
			fflush( stdout );
		}
	};
	auto take = [&]( size_t i, const std::string &k, bool ok ) {
		if ( !ok ) { res.bad++; return; }
		if ( PackAdd( w, k, buf.data(), buf.size() ) ) { seen.insert( k ); res.from[i]++; }
	};
	// a hashed entry is taken when a known key hashes to it and nothing above supplied that key
	auto wanted = [&]( uint64_t hash ) {
		if ( !NameOfHash( hash, key ) ) { unnamed.insert( hash ); return false; }
		return !seen.count( key );
	};
	auto readDelta = [&]( FILE *f, const DeltaRec &r ) {
		buf.resize( r.size );
		return _fseeki64( f, (long long)r.offset, SEEK_SET ) == 0
			&& fread( buf.data(), 1, buf.size(), f ) == buf.size() && EntryOk( buf.data(), buf.size() );
	};

	for ( size_t i = 0; i < n; i++ ) {
		switch ( src[i].kind ) {
		case FoldSource::DELTA:
			for ( const auto &kv : delta[i].named ) {
				progress();
				if ( seen.count( kv.first ) ) continue;
				take( i, kv.first, readDelta( deltaF[i], kv.second ) );
			}
			for ( const auto &kv : delta[i].hashed ) {
				progress();
				if ( !wanted( kv.first ) ) continue;
				take( i, key, readDelta( deltaF[i], kv.second ) );
			}
			break;
		case FoldSource::PACK:
			for ( const texCachePackEntry_t &e : packs[i].index ) {
				progress();
				const std::string k = packs[i].Key( e );
				if ( seen.count( k ) ) continue;
				take( i, k, PackReadEntry( packs[i], e.offset, e.size, buf ) );
			}
			for ( const texCachePackEntryV1_t &e : packs[i].hashed ) {
				progress();
				if ( !wanted( e.nameHash ) ) continue;
				take( i, key, PackReadEntry( packs[i], e.offset, e.size, buf ) );
			}
			break;
		case FoldSource::LOOSE:
			for ( const PackSource &s : loose[i] ) {
				progress();
				if ( !wanted( s.hash ) ) continue;
				take( i, key, ReadEntry( s.path, buf ) );
			}
			break;
		}
	}
	closeAll();		// the replace below needs the old file closed
	res.unnamed = (int)unnamed.size();

	if ( !PackFinish( w ) ) return false;
	res.count = (int)w.index.size();
	res.bytes = w.pos;
	return true;
}

static void FoldSummary( const std::vector<FoldSource> &src, const FoldResult &res )
{
	char sz[32];
	PrintSize( (long long)res.bytes, sz );
	printf( "\r  pack.bin: %d entries, %s                    \n   ", res.count, sz );
	bool first = true;
	for ( size_t i = 0; i < src.size(); i++ ) {
		if ( !res.from[i] ) continue;
		printf( "%s %d from %s", first ? "" : ",", res.from[i], src[i].label );
		first = false;
	}
	if ( res.bad ) printf( ", %d unreadable left out", res.bad );
	if ( res.unnamed ) printf( ", %d stored by hash alone matched no known name and were left out", res.unnamed );
	printf( "\n" );
}

// pack.delta, then the old pack.bin, then loose entries, into a fresh pack.bin in dir; the delta goes once that is in place
static bool FoldDir( const std::string &dir )
{
	const std::string packPath = dir + "\\pack.bin", deltaPath = dir + "\\pack.delta";
	const std::vector<FoldSource> src = {
		{ FoldSource::DELTA, deltaPath, "pack.delta" },
		{ FoldSource::PACK,  packPath,  "the old pack.bin" },
		{ FoldSource::LOOSE, dir,       "loose files" },
	};
	FoldResult res;
	if ( !FoldPacks( src, packPath, res ) ) {
		if ( res.empty ) printf( "  no cache entries in %s, no pack written\n", dir.c_str() );
		else printf( "\r  pack.bin not written; pack.delta kept                    \n" );
		return false;
	}
	if ( res.unchanged ) printf( "  pack.bin already holds everything (%d entries), nothing to fold\n", res.count );
	else FoldSummary( src, res );
	if ( FileExists( deltaPath ) ) {
		if ( DeleteFileA( deltaPath.c_str() ) ) printf( "  pack.delta is folded in and removed\n" );
		else printf( "  pack.delta could not be deleted; the next run folds it again\n" );
	}
	return true;
}

// ---------------------------------------------------------------- card
// a status line for the card, a verified copy onto it, and the merge that keeps what the device found

// entries a pack.bin header claims, -1 when there is no usable pack
static int PackCount( const std::string &path )
{
	FILE *f = fopen( path.c_str(), "rb" );
	if ( !f ) return -1;
	texCachePackHdr_t ph = { 0, 0, 0, 0 };
	const bool ok = fread( &ph, 1, sizeof( ph ), f ) == sizeof( ph )
		&& ph.magic == TEXCACHE_PACK_MAGIC && ( ph.version == TEXCACHE_PACK_VERSION || ph.version == TEXCACHE_PACK_VERSION_V1 )
		&& ph.count <= TEXCACHE_PACK_MAX_COUNT;
	fclose( f );
	return ok ? (int)ph.count : -1;
}

// distinct textures in a pack.delta, -1 when there is none
static int DeltaCount( const std::string &path )
{
	DeltaIndex ix;
	FILE *f = DeltaOpen( path, ix );
	if ( !f ) return -1;
	fclose( f );
	return (int)ix.Count();
}

// ---------------------------------------------------------------- list
// every entry of a pack.bin or pack.delta with what its header says

static void PrintEntryLine( const std::string &key, const uint8_t *hdrBytes, size_t have )
{
	texCacheHdrDxt_t h;
	if ( have < sizeof( h ) ) { printf( "  %-56s (unreadable)\n", key.c_str() ); return; }
	memcpy( &h, hdrBytes, sizeof( h ) );
	if ( h.magic != TEXCACHE_MAGIC_DXT ) { printf( "  %-56s (bad header)\n", key.c_str() ); return; }
	printf( "  %-56s %5ux%-5u %2u mip%s  picmip %u  %s%s\n", key.c_str(), h.width, h.height, h.mipCount, h.mipCount == 1 ? " " : "s",
			h.picmip, h.format == TEXCACHE_FMT_DXT5 ? "dxt5" : "dxt1",
			!( h.flags & TEXCACHE_FLAG_VALID ) ? "  (no flags)" : ( h.flags & TEXCACHE_FLAG_MIPMAP ) ? "" : "  nomipmaps" );
}

static int ListFile( const std::string &path )
{
	FILE *f = fopen( path.c_str(), "rb" );
	if ( !f ) { printf( "  cannot open %s\n\n", path.c_str() ); return 1; }
	uint32_t magic = 0;
	fread( &magic, 1, 4, f );
	uint8_t hdr[sizeof( texCacheHdrDxt_t )];
	char hashName[32];
	if ( magic == TEXCACHE_PACK_MAGIC ) {
		fclose( f );
		PackReader r;
		if ( !PackOpenRead( r, path ) ) { printf( "  %s is not a pack.bin this build reads\n\n", path.c_str() ); return 1; }
		printf( "  %s: pack.bin v%u, %d entries%s\n\n", path.c_str(), r.version, (int)r.Count(),
				r.version == TEXCACHE_PACK_VERSION_V1 ? ", keyed by hash" : "" );
		for ( const texCachePackEntry_t &e : r.index ) {
			const size_t have = _fseeki64( r.f, e.offset, SEEK_SET ) == 0 ? fread( hdr, 1, sizeof( hdr ), r.f ) : 0;
			PrintEntryLine( r.Key( e ), hdr, have );
		}
		for ( const texCachePackEntryV1_t &e : r.hashed ) {
			sprintf( hashName, "#%016llx", (unsigned long long)e.nameHash );
			const size_t have = _fseeki64( r.f, e.offset, SEEK_SET ) == 0 ? fread( hdr, 1, sizeof( hdr ), r.f ) : 0;
			PrintEntryLine( hashName, hdr, have );
		}
		PackCloseRead( r );
		printf( "\n" );
		return 0;
	}
	_fseeki64( f, 0, SEEK_END );
	const uint64_t fileSize = (uint64_t)_ftelli64( f );
	printf( "  %s: pack.delta, records in file order\n\n", path.c_str() );
	uint64_t pos = 0;
	int records = 0, hashedRecords = 0;
	for ( ;; ) {
		std::string key;
		uint64_t hash, entryPos;
		uint32_t size;
		if ( !DeltaParse( f, pos, fileSize, key, hash, size, entryPos ) ) break;
		if ( key.empty() ) { sprintf( hashName, "#%016llx", (unsigned long long)hash ); key = hashName; hashedRecords++; }
		const size_t have = _fseeki64( f, (long long)entryPos, SEEK_SET ) == 0 ? fread( hdr, 1, sizeof( hdr ), f ) : 0;
		PrintEntryLine( key, hdr, have );
		records++;
		pos = entryPos + size;
	}
	fclose( f );
	printf( "\n  %d records", records );
	if ( hashedRecords ) printf( ", %d of them keyed by hash", hashedRecords );
	if ( pos < fileSize ) printf( ", %llu bytes of torn tail after the last whole record", (unsigned long long)( fileSize - pos ) );
	printf( "\n\n" );
	return records ? 0 : 1;
}

static std::string CardCacheDir( const std::string &card )
{
	return TrimSlashes( card ) + "\\data\\JAVITA\\texcache_dxt";
}

static std::string WinError( DWORD e )
{
	switch ( e ) {
	case ERROR_ACCESS_DENIED:		return "the card is not writable";
	case ERROR_WRITE_PROTECT:		return "the card is write-protected";
	case ERROR_DISK_FULL:
	case ERROR_HANDLE_DISK_FULL:	return "the card is full";
	case ERROR_NOT_READY:
	case ERROR_DEVICE_NOT_CONNECTED:
	case ERROR_PATH_NOT_FOUND:		return "the card went away";
	}
	char buf[64];
	sprintf( buf, "Windows error %lu", (unsigned long)e );
	return buf;
}

static bool SameBytes( const std::string &a, const std::string &b )
{
	FILE *fa = fopen( a.c_str(), "rb" ), *fb = fopen( b.c_str(), "rb" );
	bool same = fa && fb;
	if ( same ) {
		std::vector<uint8_t> ba( 1 << 20 ), bb( 1 << 20 );
		for ( ;; ) {
			const size_t na = fread( ba.data(), 1, ba.size(), fa ), nb = fread( bb.data(), 1, bb.size(), fb );
			if ( na != nb || memcmp( ba.data(), bb.data(), na ) != 0 ) { same = false; break; }
			if ( !na ) break;
		}
	}
	if ( fa ) fclose( fa );
	if ( fb ) fclose( fb );
	return same;
}

// copies src beside dst, reads the copy back against src in full, then puts it in place of dst
static bool CopyVerified( const std::string &src, const std::string &dst, std::string &why )
{
	const std::string tmp = dst + ".tmp";
	if ( !CopyFileA( src.c_str(), tmp.c_str(), FALSE ) ) {
		why = WinError( GetLastError() );
		DeleteFileA( tmp.c_str() );
		return false;
	}
	if ( !SameBytes( src, tmp ) ) {
		why = "the copy on the card does not match the file here";
		DeleteFileA( tmp.c_str() );
		return false;
	}
	if ( !MoveFileExA( tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING ) ) {
		why = WinError( GetLastError() ) + " (could not replace pack.bin)";
		DeleteFileA( tmp.c_str() );
		return false;
	}
	return true;
}

// what the device found, then this build, then what was already on the card, into localPack; the copy is verified before the card's pack.delta goes
static bool Deliver( const std::string &card, const std::string &localPack )
{
	const std::string dir = CardCacheDir( card );
	const std::string cardPack = dir + "\\pack.bin", cardDelta = dir + "\\pack.delta";

	if ( !MakeDirs( dir ) ) {
		printf( "  The Vita card is not writable: cannot create %s\n  Check it is plugged in with write access and run again.\n\n", dir.c_str() );
		return false;
	}

	const std::vector<FoldSource> src = {
		{ FoldSource::DELTA, cardDelta, "the Vita's pack.delta" },
		{ FoldSource::PACK,  localPack, "this build" },
		{ FoldSource::PACK,  cardPack,  "the Vita's pack.bin" },
		{ FoldSource::LOOSE, dir,       "loose files on the Vita" },
	};
	printf( "  merging with the Vita:\n" );
	FoldResult res;
	if ( !FoldPacks( src, localPack, res ) ) {
		printf( "  Could not merge the Vita's textures into pack.bin; nothing was copied and nothing on the Vita was touched.\n\n" );
		return false;
	}
	if ( res.unchanged ) printf( "  nothing new on the Vita\n" );
	else FoldSummary( src, res );

	char sz[32];
	PrintSize( (long long)res.bytes, sz );
	if ( res.unchanged && PackCount( cardPack ) == res.count && SameBytes( localPack, cardPack ) ) {
		printf( "  the Vita already has this pack.bin, %d entries, %s\n", res.count, sz );
	}
	else {
		printf( "\n  copying to %s ...\n", cardPack.c_str() );
		fflush( stdout );
		std::string why;
		if ( !CopyVerified( localPack, cardPack, why ) ) {
			printf( "  Copy failed: %s.\n  The Vita's pack.delta was left in place, nothing was lost. Run again once the card is writable.\n\n", why.c_str() );
			return false;
		}
		printf( "  copied and verified, %d entries, %s\n", res.count, sz );
	}

	if ( FileExists( cardDelta ) ) {
		if ( DeleteFileA( cardDelta.c_str() ) ) printf( "  the Vita's pack.delta is folded in and removed\n" );
		else printf( "  the Vita's pack.delta could not be removed; the game keeps using it until the next run\n" );
	}
	printf( "\n  Done. The Vita has the whole cache.\n\n" );
	return true;
}

// a whole line from the console with quotes, spaces and the newline trimmed off; false at end of input
static bool ReadLine( std::string &out )
{
	char buf[2048];
	out.clear();
	if ( !fgets( buf, sizeof( buf ), stdin ) ) return false;
	out = buf;
	if ( out.compare( 0, 3, "\xEF\xBB\xBF" ) == 0 ) out.erase( 0, 3 );		// a piped UTF-8 byte order mark
	while ( !out.empty() && (unsigned char)out.back() <= ' ' ) out.pop_back();
	size_t s = 0;
	while ( s < out.size() && (unsigned char)out[s] <= ' ' ) s++;
	out = out.substr( s );
	if ( out.size() >= 2 && out.front() == '"' && out.back() == '"' ) out = out.substr( 1, out.size() - 2 );
	return true;
}

static void Usage( void )
{
	printf(
		"texbake -- pre-compress Jedi Academy textures for the Vita\n"
		"\n"
		"  texbake [<folder with the pk3 files>] [options]\n"
		"  texbake --pack <texcache_dxt folder>\n"
		"  texbake --list <pack.bin or pack.delta>\n"
		"\n"
		"  -o <folder>    where to put texcache_dxt (default: .\\texcache_out)\n"
		"  --picmip <n>   the r_picmip the Vita runs (default 1; must match the game)\n"
		"  --fast         quicker, slightly worse blocks (default is the better encoder)\n"
		"  --force        rebake entries that already exist\n"
		"  --threads <n>  worker threads (default: one per core)\n"
		"  --copy <path>  put pack.bin on the card at this root when finished, no questions;\n"
		"                 what the card's pack.delta holds is merged in first\n"
		"  --no-copy      never offer to copy\n"
		"  --pack <dir>   fold pack.delta, the old pack.bin and any loose entries in <dir>\n"
		"                 into a fresh pack.bin, no baking\n"
		"  --list <file>  print every entry of a pack.bin or pack.delta: its key, size, mips,\n"
		"                 picmip and flags\n"
		"\n"
		"  With no arguments it finds the game and the card itself and shows a menu.\n" );
}

// keeps the output honest when a setting that changes the pixels is altered
static bool ManifestMatches( const std::string &dir, bool &hadOne )
{
	const std::string path = dir + "\\texbake.txt";
	hadOne = false;

	char want[128];
	sprintf( want, "picmip=%d quality=%d\n", opt.picmip, opt.highQuality );

	FILE *f = fopen( path.c_str(), "rb" );
	if ( f ) {
		char have[128] = { 0 };
		const size_t n = fread( have, 1, sizeof( have ) - 1, f );
		fclose( f );
		have[n] = '\0';
		hadOne = true;
		if ( strcmp( have, want ) == 0 ) return true;
	}

	f = fopen( path.c_str(), "wb" );
	if ( f ) { fwrite( want, 1, strlen( want ), f ); fclose( f ); }
	return !hadOne;
}

static std::string g_exePath;

// indexes the pk3 files, reads the shaders and menus and settles how every reachable image loads; false with no pk3s
static bool ScanAssets( void )
{
	if ( !g_keys.empty() ) return true;
	FindPaks( opt.assets, g_pakPaths );
	if ( g_pakPaths.empty() ) {
		printf( "  There are no .pk3 files in %s\n  Point me at the folder that holds assets0.pk3.\n\n", opt.assets.c_str() );
		return false;
	}

	// ---- index every archive, highest sorted name wins a duplicate
	g_arch.resize( g_pakPaths.size() );
	for ( size_t i = 0; i < g_pakPaths.size(); i++ ) {
		const char *why = NULL;
		if ( zr_open( &g_arch[i], g_pakPaths[i].c_str(), &why ) != 0 ) {
			printf( "  cannot read %s (%s)\n", g_pakPaths[i].c_str(), why ? why : "unknown" );
			memset( &g_arch[i], 0, sizeof( g_arch[i] ) );
			continue;
		}
		for ( int e = 0; e < g_arch[i].count; e++ ) {
			const std::string n = g_arch[i].entries[e].name;
			if ( n.empty() || n.back() == '/' ) continue;
			g_index[n] = { (int)i, e };
		}
		printf( "  read %-16s %5d files\n", g_pakPaths[i].substr( g_pakPaths[i].find_last_of( '\\' ) + 1 ).c_str(),
				g_arch[i].count );
	}
	printf( "\n" );

	// ---- shaders tell us how each texture will be loaded
	int shaderFiles = 0;
	for ( const auto &kv : g_index ) {
		if ( kv.first.size() < 8 || kv.first.compare( 0, 8, "shaders/" ) != 0 ) continue;
		if ( ExtOf( kv.first ) != "shader" ) continue;

		const char *why = NULL;
		size_t len = 0;
		uint8_t *buf = zr_read( &g_arch[kv.second.archive],
								&g_arch[kv.second.archive].entries[kv.second.entry], &len, &why );
		if ( !buf ) continue;
		ParseShaderText( std::string( (const char *)buf, len ) );
		free( buf );
		shaderFiles++;
	}
	printf( "  %d shader files, %d shaders, %d texture references\n",
			shaderFiles, (int)g_shaderNames.size(), (int)g_keys.size() );

	// ---- menu art is registered unmipmapped, and the engine bakes it that way
	int menuFiles = 0;
	for ( const auto &kv : g_index ) {
		const std::string e = ExtOf( kv.first );
		if ( e != "menu" && e != "txt" ) continue;

		const char *why = NULL;
		size_t len = 0;
		uint8_t *buf = zr_read( &g_arch[kv.second.archive],
								&g_arch[kv.second.archive].entries[kv.second.entry], &len, &why );
		if ( !buf ) continue;
		ParseMenuText( std::string( (const char *)buf, len ) );
		free( buf );
		menuFiles++;
	}
	const int fromSrc = ScanNoMipLiterals( g_exePath );
	printf( "  %d menu files name %d unmipmapped images", menuFiles, (int)g_nomip.size() );
	if ( fromSrc ) printf( ", %d more from the module sources", fromSrc );
	printf( "\n" );

	// ---- every image with no shader of its own is reached by its plain name
	int plain = 0, uncompressed = 0, nomip = 0;
	for ( const auto &kv : g_index ) {
		if ( !IsImageName( kv.first ) ) continue;
		const std::string key = CacheKey( kv.first );
		if ( key.empty() || g_shaderNames.count( key ) ) continue;		// a shader owns this name
		if ( g_keys.count( key ) ) continue;
		if ( g_notc.count( key ) ) { uncompressed++; continue; }
		Flags f;
		// no stanza means R_FindShader loads it raw, passing mipRawImage for both flags
		if ( g_nomip.count( key ) || IsNoMipFamily( key ) ) {
			f.mipmap = false;
			f.allowPicmip = false;
			nomip++;
		}
		g_keys.emplace( key, KeyInfo{ f, kv.first } );
		plain++;
	}
	printf( "  %d more images reached by name alone", plain );
	if ( nomip ) printf( ", %d of them unmipmapped", nomip );
	if ( uncompressed ) printf( ", %d left uncompressed by their shader", uncompressed );
	printf( "\n\n" );
	return true;
}

// bakes opt.assets into outDir\pack.bin, carrying over the last run's entries; 0 when the pack is current
static int RunBake( const std::string &outDir, const std::string &packPath )
{
	printf( "  picmip : %d      encoder: %s\n\n", opt.picmip, opt.highQuality ? "best" : "fast" );
	if ( !ScanAssets() ) return 1;

	// ---- group the keys by the work they need, so each image is encoded once
	if ( !MakeDirs( outDir ) ) {
		printf( "  cannot create the output folder: %s\n\n", outDir.c_str() );
		return 1;
	}

	bool hadManifest = false;
	if ( !ManifestMatches( outDir, hadManifest ) && hadManifest ) {
		printf( "  settings changed since the last run, rebaking everything\n\n" );
		opt.force = 1;
	}

	// the last run's pack: its entries are carried over rather than encoded again
	PackReader old;
	PackOpenRead( old, packPath );

	std::map<std::string, int> group;		// file|mip|pic -> job index
	int unresolved = 0, already = 0;
	size_t keyCount = 0;

	for ( const auto &kv : g_keys ) {
		const std::string &key = kv.first;
		const Flags &f = kv.second.f;
		if ( !f.allowTC ) continue;

		if ( !opt.force ) {
			if ( PackHas( old, key ) ) { already++; continue; }
		}

		std::string file;
		if ( !ResolveImage( kv.second.ref, file ) ) { unresolved++; continue; }

		const std::string tag = file + "|" + ( f.mipmap ? "m" : "-" ) + ( f.allowPicmip ? "p" : "-" );
		auto it = group.find( tag );
		if ( it == group.end() ) {
			const IndexEntry &ie = g_index[file];
			Job j;
			j.file = file;
			j.archive = ie.archive;
			j.entry = ie.entry;
			j.mipmap = f.mipmap;
			j.allowPicmip = f.allowPicmip;
			j.keys.push_back( key );
			group[tag] = (int)g_jobs.size();
			g_jobs.push_back( j );
		}
		else {
			g_jobs[it->second].keys.push_back( key );
		}
		keyCount++;
	}

	printf( "  %d textures to encode", (int)g_jobs.size() );
	if ( already ) printf( ", %d already in pack.bin", already );
	if ( unresolved ) printf( ", %d named but missing", unresolved );
	printf( "\n\n" );

	if ( g_jobs.empty() && old.version != TEXCACHE_PACK_VERSION_V1 ) {
		const bool havePack = old.f != NULL;
		if ( havePack ) printf( "  nothing to encode; pack.bin is current\n\n" );
		else printf( "  Nothing to encode and no pack.bin from before. Are these the game's pk3 files?\n\n" );
		for ( size_t i = 0; i < g_arch.size(); i++ ) zr_close( &g_arch[i] );
		PackCloseRead( old );
		return havePack ? 0 : 1;
	}
	if ( g_jobs.empty() ) printf( "  nothing to encode; the hash-keyed pack.bin is rewritten with names\n\n" );

	if ( !PackBegin( g_pack, packPath, keyCount + old.Count() ) ) {
		printf( "  cannot write %s\n\n", g_pack.tmp.c_str() );
		return 1;
	}

	char sz[32];
	if ( !g_jobs.empty() ) {
		// ---- encode
		int nThreads = opt.threads > 0 ? opt.threads : (int)std::thread::hardware_concurrency();
		if ( nThreads < 1 ) nThreads = 1;
		if ( nThreads > 32 ) nThreads = 32;
		printf( "  encoding on %d threads, this takes a few minutes\n\n", nThreads );

		const DWORD t0 = GetTickCount();
		std::vector<std::thread> pool;
		for ( int i = 0; i < nThreads; i++ ) pool.emplace_back( Worker );

		const int total = (int)g_jobs.size();
		for ( ;; ) {
			const int done = g_done.load();
			PrintSize( g_bytes.load(), sz );
			printf( "\r  %6d / %d   %3d%%   %s written   ", done, total,
					total ? ( done * 100 / total ) : 100, sz );
			fflush( stdout );
			if ( done >= total ) break;
			Sleep( 200 );
		}
		for ( std::thread &t : pool ) t.join();

		PrintSize( g_bytes.load(), sz );
		printf( "\r  %6d / %d   100%%   %s written   \n\n", total, total, sz );

		const DWORD secs = ( GetTickCount() - t0 ) / 1000;
		printf( "  done in %u:%02u   %d entries written", secs / 60, secs % 60, g_written.load() );
		if ( g_skipped.load() ) printf( ", %d skipped (not power of two)", g_skipped.load() );
		if ( g_failed.load() ) printf( ", %d failed", g_failed.load() );
		printf( "\n" );

		if ( !g_problems.empty() ) {
			printf( "\n  problems:\n" );
			for ( const std::string &p : g_problems ) printf( "    %s\n", p.c_str() );
			if ( g_failed.load() > (int)g_problems.size() )
				printf( "    ...and %d more\n", g_failed.load() - (int)g_problems.size() );
		}
	}

	for ( size_t i = 0; i < g_arch.size(); i++ ) zr_close( &g_arch[i] );

	// ---- what the last run baked and this one did not touch stays in the pack
	int carried = 0;
	{
		std::set<std::string> fresh;
		for ( const PackItem &e : g_pack.index ) fresh.insert( e.key );
		std::vector<uint8_t> buf;
		if ( old.version == TEXCACHE_PACK_VERSION ) {
			for ( const texCachePackEntry_t &e : old.index ) {
				const std::string key = old.Key( e );
				if ( fresh.count( key ) ) continue;
				if ( PackReadEntry( old, e.offset, e.size, buf ) && PackAdd( g_pack, key, buf.data(), buf.size() ) ) carried++;
			}
		}
		else {
			// a hash-keyed pack supplies an entry under the key that hashes to it
			for ( const auto &kv : g_keys ) {
				if ( !kv.second.f.allowTC || fresh.count( kv.first ) ) continue;
				const texCachePackEntryV1_t *e = PackFindHash( old, FnvName( kv.first.c_str() ) );
				if ( e && PackReadEntry( old, e->offset, e->size, buf ) && PackAdd( g_pack, kv.first, buf.data(), buf.size() ) ) carried++;
			}
		}
	}
	PackCloseRead( old );		// the replace below needs the old file closed

	if ( !PackFinish( g_pack ) ) {
		printf( "\n  pack.bin not written\n\n" );
		return 1;
	}
	PrintSize( (long long)g_pack.pos, sz );
	printf( "\n  pack.bin: %d entries, %s", (int)g_pack.index.size(), sz );
	if ( carried ) printf( " (%d carried over from the last run)", carried );
	printf( "\n\n" );
	return 0;
}

// ---------------------------------------------------------------- front end

static void PrintStatus( const std::string &game, const std::string &card, const std::string &localPack )
{
	printf( "  Game : %s\n", game.empty() ? "not found" : game.c_str() );
	if ( card.empty() ) {
		printf( "  Vita : not plugged in\n" );
	}
	else {
		const std::string dir = CardCacheDir( card );
		const int packN = PackCount( dir + "\\pack.bin" ), deltaN = DeltaCount( dir + "\\pack.delta" );
		printf( "  Vita : %s  -", card.c_str() );
		if ( packN < 0 && deltaN < 0 ) printf( " no texture cache yet" );
		if ( packN >= 0 ) printf( " pack.bin %d entries", packN );
		if ( packN >= 0 && deltaN >= 0 ) printf( "," );
		if ( deltaN >= 0 ) printf( " pack.delta %d new textures", deltaN );
		printf( "\n" );
	}
	const int localN = PackCount( localPack );
	if ( localN < 0 ) printf( "  Local: none yet\n" );
	else printf( "  Local: %s - %d entries\n", localPack.c_str(), localN );
	printf( "\n" );
}

// '1' build and copy, '2' build only, '3' fold on the card, 'q' quit; Enter takes the default
static char Menu( bool haveCard )
{
	printf( "    1  Build the cache and put it on the Vita%s\n", haveCard ? "" : "   (needs the Vita plugged in)" );
	printf( "    2  Just build - I'll copy pack.bin myself\n" );
	if ( haveCard ) printf( "    3  Fold the Vita's new textures into its pack.bin (no baking)\n" );
	printf( "    q  Quit\n\n" );
	const char def = haveCard ? '1' : '2';
	for ( ;; ) {
		printf( "  choice [%c]: ", def );
		fflush( stdout );
		std::string line;
		if ( !ReadLine( line ) || line.empty() ) { printf( "%c\n", def ); return def; }
		const char c = (char)tolower( (unsigned char)line[0] );
		if ( c == 'q' || c == '2' || ( ( c == '1' || c == '3' ) && haveCard ) ) return c;
		if ( c == '1' ) printf( "  No Vita is plugged in. Plug it in and run again, or pick 2.\n" );
		else printf( "  Type 1, 2%s or q.\n", haveCard ? ", 3" : "" );
	}
}

int main( int argc, char **argv )
{
	printf( "\n  texbake -- Vita texture cache builder\n\n" );
	g_exePath = argv[0];

	bool interactive = true;
	for ( int i = 1; i < argc; i++ ) {
		const std::string a = argv[i];
		if ( a == "--card" && i + 1 < argc ) { opt.card = argv[++i]; continue; }
		interactive = false;
		if ( a == "-h" || a == "--help" || a == "/?" ) { Usage(); return 0; }
		else if ( a == "-o" && i + 1 < argc ) opt.out = argv[++i];
		else if ( a == "--picmip" && i + 1 < argc ) opt.picmip = atoi( argv[++i] );
		else if ( a == "--threads" && i + 1 < argc ) opt.threads = atoi( argv[++i] );
		else if ( a == "--copy" && i + 1 < argc ) opt.copyTo = argv[++i];
		else if ( a == "--pack" && i + 1 < argc ) opt.pack = argv[++i];
		else if ( a == "--list" && i + 1 < argc ) opt.list = argv[++i];
		else if ( a == "--fast" ) opt.highQuality = 0;
		else if ( a == "--force" ) opt.force = 1;
		else if ( a == "--no-copy" ) opt.noCopy = 1;
		else if ( !a.empty() && a[0] != '-' && opt.assets.empty() ) opt.assets = a;
		else { printf( "  unknown option: %s\n\n", a.c_str() ); Usage(); return 1; }
	}

	if ( opt.picmip < 0 || opt.picmip > 16 ) { printf( "  picmip must be 0..16\n" ); return 1; }

	if ( !opt.list.empty() ) return ListFile( opt.list );

	if ( !opt.pack.empty() ) {
		std::string dir = TrimSlashes( opt.pack );
		if ( DirExists( dir + "\\texcache_dxt" ) ) dir += "\\texcache_dxt";
		printf( "  packing : %s\n", dir.c_str() );
		// hash-keyed entries fold in under the game's names
		if ( opt.assets.empty() ) opt.assets = FindGame();
		opt.assets = TrimSlashes( opt.assets );
		if ( opt.assets.empty() ) {
			printf( "  no game found, so entries stored by hash alone cannot be named and are left out\n\n" );
		}
		else {
			printf( "  naming  : %s\n\n", opt.assets.c_str() );
			ScanAssets();
		}
		const bool ok = FoldDir( dir );
		printf( "\n" );
		return ok ? 0 : 1;
	}

	// ---- what we have to work with
	std::string card;
	if ( !opt.copyTo.empty() ) {
		card = TrimSlashes( opt.copyTo );
		if ( !DirExists( card + "\\" ) ) { printf( "  There is no drive or folder at %s. Is the Vita plugged in?\n\n", card.c_str() ); return 1; }
	}
	else if ( !opt.card.empty() ) {
		card = TrimSlashes( opt.card );
		if ( !DirExists( card + "\\" ) ) card.clear();
	}
	else {
		card = DetectCard();
	}

	if ( opt.assets.empty() ) opt.assets = FindGame();
	opt.assets = TrimSlashes( opt.assets );

	if ( opt.out.empty() ) opt.out = "texcache_out";
	const std::string outDir = opt.out + "\\texcache_dxt";
	const std::string packPath = outDir + "\\pack.bin";

	PrintStatus( opt.assets, card, packPath );

	if ( opt.assets.empty() ) {
		printf( "  I could not find Jedi Academy on this PC.\n" );
		if ( !interactive ) {
			printf( "  Drag the folder that holds assets0.pk3 onto TEXBAKE.bat and it will be used.\n\n" );
			return 1;
		}
		printf( "  Drag the folder that holds assets0.pk3 onto this window, then press Enter.\n\n  folder: " );
		fflush( stdout );
		std::string line;
		if ( !ReadLine( line ) || line.empty() ) { printf( "\n  Nothing entered, so nothing to do.\n\n" ); return 1; }
		opt.assets = TrimSlashes( line );
		printf( "\n  Game : %s\n\n", opt.assets.c_str() );
	}

	// ---- what to do: '1' build and deliver, '2' build only, '3' fold on the card, 'a' ask after the build
	char choice;
	if ( interactive ) choice = Menu( !card.empty() );
	else if ( !opt.copyTo.empty() ) choice = '1';
	else if ( opt.noCopy || card.empty() ) choice = '2';
	else choice = 'a';
	if ( interactive ) printf( "\n" );

	if ( choice == 'q' ) return 0;
	if ( choice == '3' ) {
		const std::string dir = CardCacheDir( card );
		printf( "  folding : %s\n\n", dir.c_str() );
		ScanAssets();
		const bool ok = FoldDir( dir );
		printf( "\n" );
		return ok ? 0 : 1;
	}

	const int rc = RunBake( outDir, packPath );
	if ( rc ) return rc;

	if ( choice == 'a' ) {
		printf( "  A Vita card looks like it is on %s\n  Put pack.bin on it now? [y/N] ", card.c_str() );
		fflush( stdout );
		std::string line;
		choice = ( ReadLine( line ) && !line.empty() && tolower( (unsigned char)line[0] ) == 'y' ) ? '1' : '2';
		printf( "\n" );
	}

	if ( choice == '1' ) return Deliver( card, packPath ) ? 0 : 1;

	printf( "  Copy this file to your Vita:\n"
			"      %s\n"
			"  goes to\n"
			"      ux0:data/JAVITA/texcache_dxt/pack.bin\n\n", packPath.c_str() );
	return 0;
}
