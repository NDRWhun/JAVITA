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

struct texCacheHdrDxt_t {
	uint32_t magic, format, width, height, mipCount, picmip, flags, totalSize;
};

static uint64_t FnvName( const char *s )
{
	uint64_t h = 14695981039346656037ULL;
	for ( const unsigned char *p = (const unsigned char *)s; *p; ++p ) {
		h ^= *p;
		h *= 1099511628211ULL;
	}
	return h;
}

// ---------------------------------------------------------------- options

struct Options {
	std::string		assets;					// folder holding the pk3 files
	std::string		out;					// folder that receives texcache_dxt
	std::string		copyTo;					// card path, empty = ask
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

// sharded on the top hash byte, matching the engine: an exFAT lookup scans the directory
static std::string ShardName( const std::string &key )
{
	const unsigned long long h = (unsigned long long)FnvName( key.c_str() );
	char name[64];
	sprintf( name, "%02x\\%016llx.bin", (unsigned)( h >> 56 ), h );
	return std::string( name );
}

static bool WriteEntry( const std::string &dir, const std::string &key, const Baked &b )
{
	const std::string path = dir + "\\" + ShardName( key );
	const std::string tmp  = path + ".tmp";

	FILE *f = fopen( tmp.c_str(), "wb" );
	if ( !f ) return false;
	const bool ok = fwrite( &b.hdr, 1, sizeof( b.hdr ), f ) == sizeof( b.hdr )
		&& fwrite( b.mipSizes, 1, b.hdr.mipCount * 4, f ) == b.hdr.mipCount * 4
		&& fwrite( b.blob.data(), 1, b.blob.size(), f ) == b.blob.size();
	fclose( f );

	if ( !ok ) { DeleteFileA( tmp.c_str() ); return false; }
	DeleteFileA( path.c_str() );
	return MoveFileA( tmp.c_str(), path.c_str() ) != 0;
}

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

static std::map<std::string, Flags>	g_keys;			// cache key -> how it will be loaded
static std::set<std::string>		g_notc;			// referenced by a shader that forbids compression
static std::set<std::string>		g_shaderNames;	// lowercase, extension stripped
static std::set<std::string>		g_nomip;		// reached through RegisterShaderNoMip, so mip and picmip are off

static bool IsImageName( const std::string &n )
{
	const std::string e = ExtOf( n );
	return e == "tga" || e == "jpg" || e == "jpeg" || e == "png";
}

// the ui and cgame modules reach these through RegisterShaderNoMip, which hands
// R_FindShader mipRawImage=false and so loads them unmipmapped and unpicmipped
static bool IsNoMipFamily( const std::string &key )
{
	static const char *pre[] = { "fonts/", "levelshots/", "menu/art/", "ui/assets/",
								 "gfx/menus/", "gfx/hud/", "gfx/mp/", NULL };
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
	const std::string key = SlashFix( rawKey );
	if ( !f.allowTC ) {
		// an uncompressed image is never read from the cache; noted so the by-name pass skips it
		g_notc.insert( Lower( key ) );
		return;
	}
	g_keys.emplace( key, f );	// first reference wins, as in the game
}

static void ParseShaderText( const std::string &text )
{
	std::vector<Token> t;
	Tokenize( text.c_str(), text.c_str() + text.size(), t );

	size_t i = 0;
	while ( i < t.size() ) {
		if ( t[i].text == "{" || t[i].text == "}" ) { i++; continue; }

		const std::string name = Lower( SlashFix( StripExt( t[i].text ) ) );
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
		g_nomip.insert( Lower( StripExt( SlashFix( v ) ) ) );
	}
}

// the rest of the unmipmapped set is named by string literals in the game modules,
// so read them straight from the tree this tool ships in
static int ScanNoMipLiterals( const std::string &exePath )
{
	size_t cut = exePath.find_last_of( "\\/" );
	if ( cut == std::string::npos ) return 0;
	cut = exePath.find_last_of( "\\/", cut - 1 );		// tools\texbake -> tools
	if ( cut == std::string::npos ) return 0;
	cut = exePath.find_last_of( "\\/", cut - 1 );		// tools -> repo root
	if ( cut == std::string::npos ) return 0;
	const std::string root = exePath.substr( 0, cut );

	static const char *dirs[] = { "\\src\\codemp\\ui", "\\src\\codemp\\cgame",
								  "\\src\\codemp\\client", NULL };
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
				g_nomip.insert( Lower( StripExt( SlashFix( v ) ) ) );
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

static void Worker( std::string outDir )
{
	// every thread gets its own handles; the archive reader holds one file position
	std::vector<zrArchive> arch( g_pakPaths.size() );
	for ( size_t i = 0; i < g_pakPaths.size(); i++ ) {
		const char *why = NULL;
		if ( zr_open( &arch[i], g_pakPaths[i].c_str(), &why ) != 0 )
			memset( &arch[i], 0, sizeof( arch[i] ) );
	}

	std::vector<uint8_t> pix;
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

		for ( const std::string &key : job.keys ) {
			if ( WriteEntry( outDir, key, baked ) ) {
				g_written++;
				g_bytes += (long long)( sizeof( baked.hdr ) + baked.hdr.mipCount * 4 + baked.blob.size() );
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

static std::string AutoDetectAssets( void )
{
	static const char *guesses[] = {
		"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Jedi Academy\\GameData\\base",
		"C:\\Program Files\\Steam\\steamapps\\common\\Jedi Academy\\GameData\\base",
		"D:\\SteamLibrary\\steamapps\\common\\Jedi Academy\\GameData\\base",
		"E:\\SteamLibrary\\steamapps\\common\\Jedi Academy\\GameData\\base",
		"E:\\steamlibrary\\steamapps\\common\\Jedi Academy\\GameData\\base",
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

static void Usage( void )
{
	printf(
		"texbake -- pre-compress Jedi Academy textures for the Vita\n"
		"\n"
		"  texbake [<folder with the pk3 files>] [options]\n"
		"\n"
		"  -o <folder>    where to put texcache_dxt (default: .\\texcache_out)\n"
		"  --picmip <n>   the r_picmip the Vita runs (default 1; must match the game)\n"
		"  --fast         quicker, slightly worse blocks (default is the better encoder)\n"
		"  --force        rebake entries that already exist\n"
		"  --threads <n>  worker threads (default: one per core)\n"
		"  --copy <path>  copy to this card root when finished, no questions\n"
		"  --no-copy      never offer to copy\n" );
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

int main( int argc, char **argv )
{
	printf( "\n  texbake -- Vita texture cache builder\n\n" );

	for ( int i = 1; i < argc; i++ ) {
		const std::string a = argv[i];
		if ( a == "-h" || a == "--help" || a == "/?" ) { Usage(); return 0; }
		else if ( a == "-o" && i + 1 < argc ) opt.out = argv[++i];
		else if ( a == "--picmip" && i + 1 < argc ) opt.picmip = atoi( argv[++i] );
		else if ( a == "--threads" && i + 1 < argc ) opt.threads = atoi( argv[++i] );
		else if ( a == "--copy" && i + 1 < argc ) opt.copyTo = argv[++i];
		else if ( a == "--fast" ) opt.highQuality = 0;
		else if ( a == "--force" ) opt.force = 1;
		else if ( a == "--no-copy" ) opt.noCopy = 1;
		else if ( !a.empty() && a[0] != '-' && opt.assets.empty() ) opt.assets = a;
		else { printf( "  unknown option: %s\n\n", a.c_str() ); Usage(); return 1; }
	}

	if ( opt.picmip < 0 || opt.picmip > 16 ) { printf( "  picmip must be 0..16\n" ); return 1; }

	if ( opt.assets.empty() ) opt.assets = AutoDetectAssets();
	if ( opt.assets.empty() ) {
		printf( "  I could not find the game's base folder.\n"
				"  Drag it onto this window, or pass it on the command line:\n"
				"      texbake \"C:\\...\\Jedi Academy\\GameData\\base\"\n\n" );
		return 1;
	}
	while ( !opt.assets.empty() && ( opt.assets.back() == '\\' || opt.assets.back() == '/' ) )
		opt.assets.pop_back();

	if ( opt.out.empty() ) opt.out = "texcache_out";
	const std::string outDir = opt.out + "\\texcache_dxt";

	FindPaks( opt.assets, g_pakPaths );
	if ( g_pakPaths.empty() ) {
		printf( "  no .pk3 files in: %s\n\n", opt.assets.c_str() );
		return 1;
	}

	printf( "  assets : %s\n", opt.assets.c_str() );
	printf( "  output : %s\n", outDir.c_str() );
	printf( "  picmip : %d      encoder: %s\n\n", opt.picmip, opt.highQuality ? "best" : "fast" );

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
	const int fromSrc = ScanNoMipLiterals( argv[0] );
	printf( "  %d menu files name %d unmipmapped images", menuFiles, (int)g_nomip.size() );
	if ( fromSrc ) printf( ", %d more from the module sources", fromSrc );
	printf( "\n" );

	// ---- every image with no shader of its own is reached by its plain name
	int plain = 0, uncompressed = 0, nomip = 0;
	for ( const auto &kv : g_index ) {
		if ( !IsImageName( kv.first ) ) continue;
		const std::string key = StripExt( kv.first );
		if ( g_shaderNames.count( key ) ) continue;		// a shader owns this name
		if ( g_keys.count( key ) ) continue;
		if ( g_notc.count( key ) ) { uncompressed++; continue; }
		Flags f;
		// no stanza means R_FindShader loads it raw, passing mipRawImage for both flags
		if ( g_nomip.count( key ) || IsNoMipFamily( key ) ) {
			f.mipmap = false;
			f.allowPicmip = false;
			nomip++;
		}
		g_keys.emplace( key, f );
		plain++;
	}
	printf( "  %d more images reached by name alone", plain );
	if ( nomip ) printf( ", %d of them unmipmapped", nomip );
	if ( uncompressed ) printf( ", %d left uncompressed by their shader", uncompressed );
	printf( "\n\n" );

	// ---- group the keys by the work they need, so each image is encoded once
	if ( !MakeDirs( outDir ) ) {
		printf( "  cannot create the output folder: %s\n\n", outDir.c_str() );
		return 1;
	}

	// the engine shards on the top hash byte, so every bucket has to exist up front
	for ( int s = 0; s < 256; s++ ) {
		char sub[16];
		sprintf( sub, "\\%02x", s );
		CreateDirectoryA( ( outDir + sub ).c_str(), NULL );
	}

	bool hadManifest = false;
	if ( !ManifestMatches( outDir, hadManifest ) && hadManifest ) {
		printf( "  settings changed since the last run, rebaking everything\n\n" );
		opt.force = 1;
	}

	std::map<std::string, int> group;		// file|mip|pic -> job index
	int unresolved = 0, already = 0;

	for ( const auto &kv : g_keys ) {
		const std::string &key = kv.first;
		const Flags &f = kv.second;
		if ( !f.allowTC ) continue;

		if ( !opt.force ) {
			if ( FileExists( outDir + "\\" + ShardName( key ) ) ) { already++; continue; }
		}

		std::string file;
		if ( !ResolveImage( key, file ) ) { unresolved++; continue; }

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
	}

	printf( "  %d textures to encode", (int)g_jobs.size() );
	if ( already ) printf( ", %d already done", already );
	if ( unresolved ) printf( ", %d named but missing", unresolved );
	printf( "\n\n" );

	if ( g_jobs.empty() ) {
		printf( "  nothing to do.\n\n" );
		for ( size_t i = 0; i < g_arch.size(); i++ ) zr_close( &g_arch[i] );
		return 0;
	}

	// ---- encode
	int nThreads = opt.threads > 0 ? opt.threads : (int)std::thread::hardware_concurrency();
	if ( nThreads < 1 ) nThreads = 1;
	if ( nThreads > 32 ) nThreads = 32;
	printf( "  encoding on %d threads, this takes a few minutes\n\n", nThreads );

	const DWORD t0 = GetTickCount();
	std::vector<std::thread> pool;
	for ( int i = 0; i < nThreads; i++ ) pool.emplace_back( Worker, outDir );

	const int total = (int)g_jobs.size();
	for ( ;; ) {
		const int done = g_done.load();
		char sz[32];
		PrintSize( g_bytes.load(), sz );
		printf( "\r  %6d / %d   %3d%%   %s written   ", done, total,
				total ? ( done * 100 / total ) : 100, sz );
		fflush( stdout );
		if ( done >= total ) break;
		Sleep( 200 );
	}
	for ( std::thread &t : pool ) t.join();

	char sz[32];
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

	for ( size_t i = 0; i < g_arch.size(); i++ ) zr_close( &g_arch[i] );

	// ---- put it on the card
	printf( "\n  Copy this folder to your Vita:\n"
			"      %s\n"
			"  goes to\n"
			"      ux0:data/JAVITA/texcache_dxt\n\n", outDir.c_str() );

	std::string card = opt.copyTo;
	if ( card.empty() && !opt.noCopy ) {
		card = DetectCard();
		if ( !card.empty() ) {
			printf( "  A Vita card looks like it is on %s\n"
					"  Copy there now? Thousands of small files over USB are slow. [y/N] ", card.c_str() );
			fflush( stdout );
			const int c = getchar();
			if ( c != 'y' && c != 'Y' ) card.clear();
		}
	}

	if ( !card.empty() ) {
		while ( !card.empty() && ( card.back() == '\\' || card.back() == '/' ) ) card.pop_back();
		const std::string dest = card + "\\data\\JAVITA\\texcache_dxt";
		if ( !MakeDirs( dest ) ) {
			printf( "\n  cannot create %s\n", dest.c_str() );
			return 1;
		}
		printf( "\n  copying to %s ...\n", dest.c_str() );

		WIN32_FIND_DATAA fd;
		int copied = 0, failed = 0;
		for ( int s = 0; s < 256; s++ ) {
			char sub[16];
			sprintf( sub, "\\%02x", s );
			const std::string fromDir = outDir + sub;
			const std::string toDir   = dest + sub;
			CreateDirectoryA( toDir.c_str(), NULL );

			const HANDLE hFind = FindFirstFileA( ( fromDir + "\\*.bin" ).c_str(), &fd );
			if ( hFind == INVALID_HANDLE_VALUE ) continue;
			do {
				const std::string from = fromDir + "\\" + fd.cFileName;
				const std::string to   = toDir + "\\" + fd.cFileName;
				if ( CopyFileA( from.c_str(), to.c_str(), FALSE ) ) copied++;
				else failed++;
				if ( ( ( copied + failed ) & 63 ) == 0 ) {
					printf( "\r  %d files ", copied );
					fflush( stdout );
				}
			} while ( FindNextFileA( hFind, &fd ) );
			FindClose( hFind );
		}
		printf( "\r  copied %d files", copied );
		if ( failed ) printf( ", %d failed", failed );
		printf( "\n" );
	}

	printf( "\n" );
	return 0;
}
