/*
===========================================================================
Copyright (C) 1999 - 2005, Id Software, Inc.
Copyright (C) 2000 - 2013, Raven Software, Inc.
Copyright (C) 2001 - 2013, Activision, Inc.
Copyright (C) 2013 - 2015, OpenJK contributors

This file is part of the OpenJK source code.

OpenJK is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, see <http://www.gnu.org/licenses/>.
===========================================================================
*/

// tr_image.c
#include "tr_local.h"
#include "../rd-common/tr_common.h"
#ifndef VITA
#include "glext.h"
#endif

#include <map>
#include <string>

static byte			 s_intensitytable[256];
static unsigned char s_gammatable[256];

int		gl_filter_min = GL_LINEAR_MIPMAP_NEAREST;
int		gl_filter_max = GL_LINEAR;

//#define FILE_HASH_SIZE		1024	// actually the shader code still needs this (from another module, great),
//static	image_t*		hashTable[FILE_HASH_SIZE];

/*
** R_GammaCorrect
*/
void R_GammaCorrect( byte *buffer, int bufSize ) {
	int i;

	for ( i = 0; i < bufSize; i++ ) {
		buffer[i] = s_gammatable[buffer[i]];
	}
}

typedef struct textureMode_s {
	const char *name;
	int	minimize, maximize;
} textureMode_t;

textureMode_t modes[] = {
	{"GL_NEAREST", GL_NEAREST, GL_NEAREST},
	{"GL_LINEAR", GL_LINEAR, GL_LINEAR},
	{"GL_NEAREST_MIPMAP_NEAREST", GL_NEAREST_MIPMAP_NEAREST, GL_NEAREST},
	{"GL_LINEAR_MIPMAP_NEAREST", GL_LINEAR_MIPMAP_NEAREST, GL_LINEAR},
	{"GL_NEAREST_MIPMAP_LINEAR", GL_NEAREST_MIPMAP_LINEAR, GL_NEAREST},
	{"GL_LINEAR_MIPMAP_LINEAR", GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR}
};

static const size_t numTextureModes = ARRAY_LEN(modes);

/*
===============
GL_TextureMode
===============
*/
void GL_TextureMode( const char *string ) {
	size_t	i;
	image_t	*glt;

	for ( i = 0; i < numTextureModes ; i++ ) {
		if ( !Q_stricmp( modes[i].name, string ) ) {
			break;
		}
	}

	if ( i == numTextureModes ) {
		ri.Printf( PRINT_ALL, "bad filter name\n" );
		for ( i = 0; i < numTextureModes ; i++ ) {
			ri.Printf( PRINT_ALL, "%s\n", modes[i].name );
		}
		return;
	}

	gl_filter_min = modes[i].minimize;
	gl_filter_max = modes[i].maximize;

	// If the level they requested is less than possible, set the max possible...
	if ( r_ext_texture_filter_anisotropic->value > glConfig.maxTextureFilterAnisotropy )
		ri.Cvar_SetValue( "r_ext_texture_filter_anisotropic", glConfig.maxTextureFilterAnisotropy );

	// change all the existing mipmap texture objects
					 R_Images_StartIteration();
	while ( (glt   = R_Images_GetNextIteration()) != NULL)
	{
		if ( glt->mipmap ) {
			GL_Bind (glt);
			qglTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, gl_filter_min);
			qglTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, gl_filter_max);

			if(glConfig.maxTextureFilterAnisotropy>0) {
				if(r_ext_texture_filter_anisotropic->integer>1) {
					qglTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, r_ext_texture_filter_anisotropic->value);
				} else {
					qglTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, 1.0f);
				}
			}
		}
	}
}

// Lowercases name into out with '/' separators, cut at the first '.', bounded to outSize; re-entrant.
static void R_ImageMappingName( const char *name, char *out, int outSize )
{
	int		i=0;
	char	letter;

	while (name[i] != '\0' && i<outSize-1)
	{
		letter = tolower((unsigned char)name[i]);
		if (letter =='.') break;				// don't include extension
		if (letter =='\\') letter = '/';		// damn path names
		out[i++] = letter;
	}
	out[i]=0;
}

// makeup a nice clean, consistant name to query for and file under, for map<> usage...
//
static char *GenerateImageMappingName( const char *name )
{
	static char sName[MAX_QPATH];
	R_ImageMappingName( name, sName, sizeof( sName ) );
	return &sName[0];
}

static float R_BytesPerTex (int format)
{
	switch ( format ) {
	case 1:
		//"I    "
		return 1;
		break;
	case 2:
		//"IA   "
		return 2;
		break;
	case 3:
		//"RGB  "
		return glConfig.colorBits/8.0f;
		break;
	case 4:
		//"RGBA "
		return glConfig.colorBits/8.0f;
		break;

	case GL_RGBA4:
		//"RGBA4"
		return 2;
		break;
	case GL_RGB5:
		//"RGB5 "
		return 2;
		break;

	case GL_RGBA8:
		//"RGBA8"
		return 4;
		break;
	case GL_RGB8:
		//"RGB8"
		return 4;
		break;

	case GL_RGB4_S3TC:
		//"S3TC "
		return 0.33333f;
		break;
	case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
		//"DXT1 "
		return 0.33333f;
		break;
	case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
		//"DXT5 "
		return 1;
		break;
	default:
		//"???? "
		return 4;
	}
}

/*
===============
R_SumOfUsedImages
===============
*/
float R_SumOfUsedImages( qboolean bUseFormat )
{
	int	total = 0;
	image_t *pImage;

					  R_Images_StartIteration();
	while ( (pImage = R_Images_GetNextIteration()) != NULL)
	{
		if ( pImage->frameUsed == tr.frameCount- 1 ) {//it has already been advanced for the next frame, so...
			if (bUseFormat)
			{
				float  bytePerTex = R_BytesPerTex (pImage->internalFormat);
				total += bytePerTex * (pImage->width * pImage->height);
			}
			else
			{
				total += pImage->width * pImage->height;
			}
		}
	}

	return total;
}

/*
===============
R_ImageList_f
===============
*/
void R_ImageList_f( void ) {
	int		i=0;
	image_t	*image;
	int		texels=0;
	float	texBytes = 0.0f;
	const char *yesno[] = {"no ", "yes"};

	ri.Printf( PRINT_ALL,  "\n      -w-- -h-- -mm- -if-- wrap --name-------\n");

	int iNumImages = R_Images_StartIteration();
	while ( (image = R_Images_GetNextIteration()) != NULL)
	{
		texels   += image->width*image->height;
		texBytes += image->width*image->height * R_BytesPerTex (image->internalFormat);
		ri.Printf( PRINT_ALL,   "%4i: %4i %4i  %s ",
			i, image->width, image->height, yesno[image->mipmap] );
		switch ( image->internalFormat ) {
		case 1:
			ri.Printf( PRINT_ALL, "I    " );
			break;
		case 2:
			ri.Printf( PRINT_ALL, "IA   " );
			break;
		case 3:
			ri.Printf( PRINT_ALL, "RGB  " );
			break;
		case 4:
			ri.Printf( PRINT_ALL, "RGBA " );
			break;
		case GL_RGBA8:
			ri.Printf( PRINT_ALL, "RGBA8" );
			break;
		case GL_RGB8:
			ri.Printf( PRINT_ALL, "RGB8" );
			break;
		case GL_RGB4_S3TC:
			ri.Printf( PRINT_ALL, "S3TC " );
			break;
		case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
			ri.Printf( PRINT_ALL, "DXT1 " );
			break;
		case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
			ri.Printf( PRINT_ALL, "DXT5 " );
			break;
		case GL_RGBA4:
			ri.Printf( PRINT_ALL, "RGBA4" );
			break;
		case GL_RGB5:
			ri.Printf( PRINT_ALL, "RGB5 " );
			break;
		default:
			ri.Printf( PRINT_ALL, "???? " );
		}

		switch ( image->wrapClampMode ) {
		case GL_REPEAT:
			ri.Printf( PRINT_ALL, "rept " );
			break;
		case GL_CLAMP:
			ri.Printf( PRINT_ALL, "clmp " );
			break;
		case GL_CLAMP_TO_EDGE:
			ri.Printf( PRINT_ALL, "clpE " );
			break;
		default:
			ri.Printf( PRINT_ALL, "%4i ", image->wrapClampMode );
			break;
		}

		ri.Printf( PRINT_ALL, "%s\n", image->imgName );
		i++;
	}
	ri.Printf( PRINT_ALL,  " ---------\n");
	ri.Printf( PRINT_ALL,  "      -w-- -h-- -mm- -if- wrap --name-------\n");
	ri.Printf( PRINT_ALL,  " %i total texels (not including mipmaps)\n", texels );
	ri.Printf( PRINT_ALL,  " %.2fMB total texture mem (not including mipmaps)\n", texBytes/1048576.0f );
	ri.Printf( PRINT_ALL,  " %i total images\n\n", iNumImages );
}

//=======================================================================


/*
================
R_LightScaleTexture

Scale up the pixel values in a texture to increase the
lighting range
================
*/
void R_LightScaleTexture (unsigned *in, int inwidth, int inheight, qboolean only_gamma )
{
	if ( only_gamma )
	{
		if ( !glConfig.deviceSupportsGamma && !glConfigExt.doGammaCorrectionWithShaders )
		{
			int		i, c;
			byte	*p;

			p = (byte *)in;

			c = inwidth*inheight;
			for (i=0 ; i<c ; i++, p+=4)
			{
				p[0] = s_gammatable[p[0]];
				p[1] = s_gammatable[p[1]];
				p[2] = s_gammatable[p[2]];
			}
		}
	}
	else
	{
		int		i, c;
		byte	*p;

		p = (byte *)in;

		c = inwidth*inheight;

		if ( glConfig.deviceSupportsGamma || glConfigExt.doGammaCorrectionWithShaders )
		{
			for (i=0 ; i<c ; i++, p+=4)
			{
				p[0] = s_intensitytable[p[0]];
				p[1] = s_intensitytable[p[1]];
				p[2] = s_intensitytable[p[2]];
			}
		}
		else
		{
			for (i=0 ; i<c ; i++, p+=4)
			{
				p[0] = s_gammatable[s_intensitytable[p[0]]];
				p[1] = s_gammatable[s_intensitytable[p[1]]];
				p[2] = s_gammatable[s_intensitytable[p[2]]];
			}
		}
	}
}


/*
================
R_MipMap2

Operates in place, quartering the size of the texture
Proper linear filter
================
*/
static void R_MipMap2( unsigned *in, int inWidth, int inHeight ) {
	int			i, j, k;
	byte		*outpix;
	int			inWidthMask, inHeightMask;
	int			total;
	int			outWidth, outHeight;
	unsigned	*temp;

	outWidth = inWidth >> 1;
	outHeight = inHeight >> 1;
#ifdef VITA
	// workspace tag so the multi-MB scratch is served from the contiguous arena
	temp = (unsigned int *)Z_Malloc( outWidth * outHeight * 4, TAG_TEMP_WORKSPACE, qfalse );
#else
	temp = (unsigned int *)Hunk_AllocateTempMemory( outWidth * outHeight * 4 );
#endif

	inWidthMask = inWidth - 1;
	inHeightMask = inHeight - 1;

	for ( i = 0 ; i < outHeight ; i++ ) {
		for ( j = 0 ; j < outWidth ; j++ ) {
			outpix = (byte *) ( temp + i * outWidth + j );
			for ( k = 0 ; k < 4 ; k++ ) {
				total =
					1 * ((byte *)&in[ ((i*2-1)&inHeightMask)*inWidth + ((j*2-1)&inWidthMask) ])[k] +
					2 * ((byte *)&in[ ((i*2-1)&inHeightMask)*inWidth + ((j*2)&inWidthMask) ])[k] +
					2 * ((byte *)&in[ ((i*2-1)&inHeightMask)*inWidth + ((j*2+1)&inWidthMask) ])[k] +
					1 * ((byte *)&in[ ((i*2-1)&inHeightMask)*inWidth + ((j*2+2)&inWidthMask) ])[k] +

					2 * ((byte *)&in[ ((i*2)&inHeightMask)*inWidth + ((j*2-1)&inWidthMask) ])[k] +
					4 * ((byte *)&in[ ((i*2)&inHeightMask)*inWidth + ((j*2)&inWidthMask) ])[k] +
					4 * ((byte *)&in[ ((i*2)&inHeightMask)*inWidth + ((j*2+1)&inWidthMask) ])[k] +
					2 * ((byte *)&in[ ((i*2)&inHeightMask)*inWidth + ((j*2+2)&inWidthMask) ])[k] +

					2 * ((byte *)&in[ ((i*2+1)&inHeightMask)*inWidth + ((j*2-1)&inWidthMask) ])[k] +
					4 * ((byte *)&in[ ((i*2+1)&inHeightMask)*inWidth + ((j*2)&inWidthMask) ])[k] +
					4 * ((byte *)&in[ ((i*2+1)&inHeightMask)*inWidth + ((j*2+1)&inWidthMask) ])[k] +
					2 * ((byte *)&in[ ((i*2+1)&inHeightMask)*inWidth + ((j*2+2)&inWidthMask) ])[k] +

					1 * ((byte *)&in[ ((i*2+2)&inHeightMask)*inWidth + ((j*2-1)&inWidthMask) ])[k] +
					2 * ((byte *)&in[ ((i*2+2)&inHeightMask)*inWidth + ((j*2)&inWidthMask) ])[k] +
					2 * ((byte *)&in[ ((i*2+2)&inHeightMask)*inWidth + ((j*2+1)&inWidthMask) ])[k] +
					1 * ((byte *)&in[ ((i*2+2)&inHeightMask)*inWidth + ((j*2+2)&inWidthMask) ])[k];
				outpix[k] = total / 36;
			}
		}
	}

	memcpy( in, temp, outWidth * outHeight * 4 );
#ifdef VITA
	Z_Free( temp );
#else
	Hunk_FreeTempMemory( temp );
#endif
}

/*
================
R_MipMap

Operates in place, quartering the size of the texture
================
*/
static void R_MipMap (byte *in, int width, int height) {
	int		i, j;
	byte	*out;
	int		row;

	if ( !r_simpleMipMaps->integer ) {
		R_MipMap2( (unsigned *)in, width, height );
		return;
	}

	if ( width == 1 && height == 1 ) {
		return;
	}

	row = width * 4;
	out = in;
	width >>= 1;
	height >>= 1;

	if ( width == 0 || height == 0 ) {
		width += height;	// get largest
		for (i=0 ; i<width ; i++, out+=4, in+=8 ) {
			out[0] = ( in[0] + in[4] )>>1;
			out[1] = ( in[1] + in[5] )>>1;
			out[2] = ( in[2] + in[6] )>>1;
			out[3] = ( in[3] + in[7] )>>1;
		}
		return;
	}

	for (i=0 ; i<height ; i++, in+=row) {
		for (j=0 ; j<width ; j++, out+=4, in+=8) {
			out[0] = (in[0] + in[4] + in[row+0] + in[row+4])>>2;
			out[1] = (in[1] + in[5] + in[row+1] + in[row+5])>>2;
			out[2] = (in[2] + in[6] + in[row+2] + in[row+6])>>2;
			out[3] = (in[3] + in[7] + in[row+3] + in[row+7])>>2;
		}
	}
}


/*
==================
R_BlendOverTexture

Apply a color blend over a set of pixels
==================
*/
static void R_BlendOverTexture( byte *data, int pixelCount, byte blend[4] ) {
	int		i;
	int		inverseAlpha;
	int		premult[3];

	inverseAlpha = 255 - blend[3];
	premult[0] = blend[0] * blend[3];
	premult[1] = blend[1] * blend[3];
	premult[2] = blend[2] * blend[3];

	for ( i = 0 ; i < pixelCount ; i++, data+=4 ) {
		data[0] = ( data[0] * inverseAlpha + premult[0] ) >> 9;
		data[1] = ( data[1] * inverseAlpha + premult[1] ) >> 9;
		data[2] = ( data[2] * inverseAlpha + premult[2] ) >> 9;
	}
}

byte	mipBlendColors[16][4] = {
	{0,0,0,0},
	{255,0,0,128},
	{0,255,0,128},
	{0,0,255,128},
	{255,0,0,128},
	{0,255,0,128},
	{0,0,255,128},
	{255,0,0,128},
	{0,255,0,128},
	{0,0,255,128},
	{255,0,0,128},
	{0,255,0,128},
	{0,0,255,128},
	{255,0,0,128},
	{0,255,0,128},
	{0,0,255,128},
};





class CStringComparator
{
public:
	bool operator()(const char *s1, const char *s2) const { return(strcmp(s1, s2) < 0); }
};

typedef std::map <const char *, image_t *, CStringComparator> AllocatedImages_t;
AllocatedImages_t AllocatedImages;
AllocatedImages_t::iterator itAllocatedImages;

// shaders name images no pk3 holds, and each miss costs one probe per loader extension
static std::map<std::string, char> s_imageMisses;

void R_ImageMissCache_Clear( void )
{
	s_imageMisses.clear();
}
int giTextureBindNum = 1024;	// will be set to this anyway at runtime, but wtf?


// return = number of images in the list, for those interested
//
int R_Images_StartIteration(void)
{
	itAllocatedImages = AllocatedImages.begin();
	return AllocatedImages.size();
}

image_t *R_Images_GetNextIteration(void)
{
	if (itAllocatedImages == AllocatedImages.end())
		return NULL;

	image_t *pImage = (*itAllocatedImages).second;
	++itAllocatedImages;
	return pImage;
}

// clean up anything to do with an image_t struct, but caller will have to clear the internal to an image_t struct ready for either struct free() or overwrite...
//
// (avoid using ri->xxxx stuff here in case running on dedicated)
//
static void R_Images_DeleteImageContents( image_t *pImage )
{
	assert(pImage);	// should never be called with NULL
	if (pImage)
	{
		qglDeleteTextures( 1, &pImage->texnum );
#ifdef USE_GXM_NATIVE
		GXM_TexFree( pImage->texnum );
#endif
		Z_Free(pImage);
	}
}





/*
===============
Upload32

===============
*/
#ifdef VITA
// DXT cache on ux0: encode once, later loads upload the pre-compressed mip chain.
// Gated by r_texCacheCompressed; any failure falls back to the stock RGBA path.
#define TEXCACHE_MAGIC_DXT 0x41435456u	// "VTCA"; bump to invalidate
#define TEXCACHE_MAX_MIPS  16
// flags occupy what used to be texbits, which was written as zero and never read;
// an entry without VALID predates the field, so the checks it guards are skipped
#define TEXCACHE_FLAG_VALID		0x80000000u
#define TEXCACHE_FLAG_MIPMAP	0x00000001u
enum { TEXCACHE_FMT_DXT1 = 1, TEXCACHE_FMT_DXT5 = 5 };
typedef struct {					// 32-byte LE header, native Vita byte order
	unsigned int magic;
	unsigned int format;			// TEXCACHE_FMT_DXT1 | TEXCACHE_FMT_DXT5
	unsigned int width;				// mip0, after picmip + maxTextureSize clamp
	unsigned int height;
	unsigned int mipCount;			// 1..TEXCACHE_MAX_MIPS
	unsigned int picmip;			// r_picmip it was baked with, 0 when the image ignores picmip; mismatch = rebuild
	unsigned int flags;				// TEXCACHE_FLAG_*; 0 in entries baked before the field existed
	unsigned int totalSize;			// sum of per-mip sizes
} texCacheHdrDxt_t;

#include "../rd-common/tr_dxt.h"

static void R_TexCacheStoreDxt( const char *name, const texCacheHdrDxt_t *hdr,
								const unsigned *mipSizes, const byte *blob );
static char s_uploadDxtKey[MAX_QPATH];	// asset name of the in-flight Upload32, set by R_CreateImage
static qboolean s_texCacheKeep;			// the cache file validated but its upload failed, so the rebuild must not overwrite it

// Encode one mip as row-major, edge-clamped 4x4 DXT blocks into blob+blobOfs, upload it
// pre-compressed, return the level's byte size.
static int R_DxtEncodeUploadAppend( int level, GLenum glFmt, int w, int h, const byte *rgba,
									byte *blob, int blobOfs )
{
	const int isDxt5     = ( glFmt == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT );
	const int blockBytes = isDxt5 ? 16 : 8;
	const int bw = (w + 3) >> 2, bh = (h + 3) >> 2;
	const int mipSize = bw * bh * blockBytes;
	const int mode = ( r_dxtFast && r_dxtFast->integer ) ? 0 : 2;	// STB_DXT_NORMAL vs HIGHQUAL
	byte *dst = blob + blobOfs;
	for ( int by = 0; by < bh; ++by )
	{
		for ( int bx = 0; bx < bw; ++bx )
		{
			byte block[64];
			for ( int r = 0; r < 4; ++r )
			{
				int sy = by * 4 + r; if ( sy >= h ) sy = h - 1;
				const byte *srow = rgba + (size_t)sy * w * 4;
				byte *brow = block + r * 16;
				for ( int cc = 0; cc < 4; ++cc )
				{
					int sx = bx * 4 + cc; if ( sx >= w ) sx = w - 1;
					const byte *s = srow + sx * 4;
					brow[cc*4+0] = s[0]; brow[cc*4+1] = s[1];
					brow[cc*4+2] = s[2]; brow[cc*4+3] = s[3];
				}
			}
			R_CompressDxtBlock( dst, block, isDxt5, mode != 0 );
			dst += blockBytes;
		}
	}
	qglCompressedTexImage2D( GL_TEXTURE_2D, level, glFmt, w, h, 0, mipSize, blob + blobOfs );
	return mipSize;
}

// ---- background bake: main reads and decodes, two workers build and encode the mip chain ----
#define BAKE_WORKERS		2
#define BAKE_QUEUE			16
#define BAKE_INFLIGHT_MAX	( 13 * 1024 * 1024 )	// two 1024² jobs plus the decoder's next buffer fit the temp arena
#define BAKE_MIN_PIXELS		( 128 * 128 )			// anything smaller is quicker inline than queued

typedef struct bakeJob_s {
	image_t		*image;
	unsigned	*pic;						// RGBA at the source size, freed by main once retired
	byte		*blob;						// the DXT chain, freed by main once uploaded
	int			blobCap;
	int			srcWidth, srcHeight;
	int			width, height;				// after picmip and the size clamp
	int			halvings;					// mip passes that take srcWidth to width
	int			mipmap;
	int			allowPicmip;
	int			clampMode;
	int			store;						// write the cache file when done
	int			bytes;						// pic + blob, against BAKE_INFLIGHT_MAX
	char		key[MAX_QPATH];
	// written by the worker
	int			isDxt5;
	int			mipCount;
	int			blobSize;
	unsigned	mipSizes[TEXCACHE_MAX_MIPS];
} bakeJob_t;

static bakeJob_t	s_bakeJobs[BAKE_QUEUE];
static int			s_bakeFree[BAKE_QUEUE], s_bakeNumFree;
static int			s_bakeReq[BAKE_QUEUE], s_bakeReqHead, s_bakeReqTail;
static int			s_bakeDone[BAKE_QUEUE], s_bakeDoneHead, s_bakeDoneTail;
static int			s_bakePending;				// enqueued and not yet retired
static int			s_bakeInflight;				// bytes held by pending jobs
static SceUID		s_bakeMutex = -1, s_bakeWake = -1, s_bakeDoneSema = -1;
static SceUID		s_bakeThid[BAKE_WORKERS] = { -1, -1 };
static qboolean		s_bakeTookPic;				// set when a job took the caller's pic, so R_FindImageFile leaves it

/*
===============
R_BakeChain

Worker side: picmip, gamma, the mip chain and the DXT encode, then the cache file. No GL, no allocator.
===============
*/
static void R_BakeChain( bakeJob_t *job )
{
	unsigned	*data = job->pic;
	int			w = job->srcWidth, h = job->srcHeight;

	for ( int i = 0; i < job->halvings; i++ ) {
		R_MipMap( (byte *)data, w, h );
		w >>= 1; h >>= 1;
		if ( w < 1 ) w = 1;
		if ( h < 1 ) h = 1;
	}

	// opaque textures take DXT1; any alpha texel means DXT5
	job->isDxt5 = 0;
	{
		const byte *scan = (const byte *)data;
		const int c = w * h;
		for ( int i = 0; i < c; i++ ) {
			if ( scan[i * 4 + 3] != 255 ) { job->isDxt5 = 1; break; }
		}
	}
	const GLenum fmt = job->isDxt5 ? GL_COMPRESSED_RGBA_S3TC_DXT5_EXT : GL_COMPRESSED_RGB_S3TC_DXT1_EXT;

	if ( job->mipmap )
		R_LightScaleTexture( data, w, h, qfalse );

	int ofs = 0, level = 0;
	job->mipSizes[0] = R_DxtEncodeUploadAppend( 0, fmt, w, h, (byte *)data, job->blob, 0 );
	ofs += job->mipSizes[0];
	job->mipCount = 1;
	if ( job->mipmap ) {
		while ( ( w > 1 || h > 1 ) && job->mipCount < TEXCACHE_MAX_MIPS ) {
			R_MipMap( (byte *)data, w, h );
			w >>= 1; h >>= 1;
			if ( w < 1 ) w = 1;
			if ( h < 1 ) h = 1;
			level++;
			if ( r_colorMipLevels->integer )
				R_BlendOverTexture( (byte *)data, w * h, mipBlendColors[level] );
			job->mipSizes[job->mipCount] = R_DxtEncodeUploadAppend( level, fmt, w, h, (byte *)data, job->blob, ofs );
			ofs += job->mipSizes[job->mipCount];
			job->mipCount++;
		}
	}
	job->blobSize = ofs;

	// the cache file is plain kernel io, so it is written here rather than on main
	texCacheHdrDxt_t hdr;
	hdr.magic     = TEXCACHE_MAGIC_DXT;
	hdr.format    = job->isDxt5 ? TEXCACHE_FMT_DXT5 : TEXCACHE_FMT_DXT1;
	hdr.width     = (unsigned)job->width;
	hdr.height    = (unsigned)job->height;
	hdr.mipCount  = (unsigned)job->mipCount;
	hdr.picmip    = (unsigned)( job->allowPicmip && r_picmip ? r_picmip->integer : 0 );
	hdr.flags     = TEXCACHE_FLAG_VALID | ( job->mipmap ? TEXCACHE_FLAG_MIPMAP : 0 );
	hdr.totalSize = (unsigned)job->blobSize;
	if ( job->store )
		R_TexCacheStoreDxt( job->key, &hdr, job->mipSizes, job->blob );
}

static volatile int s_bakeQuit;

static int R_BakeWorker( SceSize argc, void *argv )
{
	for ( ;; ) {
		sceKernelWaitSema( s_bakeWake, 1, NULL );
		if ( s_bakeQuit ) {
			break;
		}

		sceKernelLockMutex( s_bakeMutex, 1, NULL );
		const int j = s_bakeReq[s_bakeReqTail];
		s_bakeReqTail = ( s_bakeReqTail + 1 ) % BAKE_QUEUE;
		sceKernelUnlockMutex( s_bakeMutex, 1 );

		R_BakeChain( &s_bakeJobs[j] );

		sceKernelLockMutex( s_bakeMutex, 1, NULL );
		s_bakeDone[s_bakeDoneHead] = j;
		s_bakeDoneHead = ( s_bakeDoneHead + 1 ) % BAKE_QUEUE;
		sceKernelUnlockMutex( s_bakeMutex, 1 );
		sceKernelSignalSema( s_bakeDoneSema, 1 );
	}
	// plain exit; R_BakeShutdown joins and deletes, and a self-delete races that
	return sceKernelExitThread( 0 );
}

// these can be inside a cache write when the process exits, which wedges teardown
void R_BakeShutdown( void )
{
	if ( s_bakeThid[0] < 0 ) {
		return;
	}
	R_BakeDrainAll();
	s_bakeQuit = 1;
	for ( int i = 0; i < BAKE_WORKERS; i++ ) {
		sceKernelSignalSema( s_bakeWake, 1 );
	}
	for ( int i = 0; i < BAKE_WORKERS; i++ ) {
		if ( s_bakeThid[i] < 0 ) continue;
		SceUInt tmo = 3 * 1000 * 1000;
		sceKernelWaitThreadEnd( s_bakeThid[i], NULL, &tmo );
		sceKernelDeleteThread( s_bakeThid[i] );
		s_bakeThid[i] = -1;
	}
}

static qboolean R_BakeEnsurePool( void )
{
	if ( s_bakeThid[0] >= 0 )
		return qtrue;
	s_bakeQuit     = 0;		// a pool restarting after a shutdown must not exit immediately
	s_bakeMutex    = sceKernelCreateMutex( "tex_bake_mtx", 0, 0, NULL );
	s_bakeWake     = sceKernelCreateSema( "tex_bake_wake", 0, 0, BAKE_QUEUE, NULL );
	s_bakeDoneSema = sceKernelCreateSema( "tex_bake_done", 0, 0, BAKE_QUEUE, NULL );
	if ( s_bakeMutex < 0 || s_bakeWake < 0 || s_bakeDoneSema < 0 )
		return qfalse;
	for ( int i = 0; i < BAKE_QUEUE; i++ )
		s_bakeFree[i] = i;
	s_bakeNumFree = BAKE_QUEUE;
	// main is on core 1; these take the two idle cores, below the mixer so audio never skips
	const int cores[BAKE_WORKERS] = { SCE_KERNEL_CPU_MASK_USER_0, SCE_KERNEL_CPU_MASK_USER_2 };
	for ( int i = 0; i < BAKE_WORKERS; i++ ) {
		s_bakeThid[i] = sceKernelCreateThread( "tex_bake", R_BakeWorker, 0x10000101, 0x8000, 0, cores[i], NULL );
		if ( s_bakeThid[i] < 0 )
			return qfalse;
		sceKernelStartThread( s_bakeThid[i], 0, NULL );
	}
	return qtrue;
}

/*
===============
R_BakeRetire

Main side: the upload and the frees, once a worker is done with the job.
===============
*/
static void R_BakeRetire( bakeJob_t *job )
{
	image_t *image = job->image;

	image->internalFormat = job->isDxt5 ? GL_COMPRESSED_RGBA_S3TC_DXT5_EXT : GL_COMPRESSED_RGB_S3TC_DXT1_EXT;
#ifdef USE_GXM_NATIVE
	if ( !GXM_TexUploadDxt( image->texnum, job->blob, (unsigned)job->blobSize,
			(unsigned)job->width, (unsigned)job->height, (unsigned)job->mipCount, job->isDxt5 ) ) {
		ri.Printf( PRINT_WARNING, "GXM_TexUploadDxt failed: %s\n", job->key );
	}
	GXM_TexFilter( image->texnum, 1, job->clampMode != GL_REPEAT );
#endif
	Z_Free( job->blob );
	Z_Free( job->pic );
	s_bakeInflight -= job->bytes;
	s_bakePending--;
	s_bakeFree[s_bakeNumFree++] = (int)( job - s_bakeJobs );
}

// retires finished jobs on main; waitOne blocks for the first, all blocks for every pending job
static void R_BakeDrain( qboolean waitOne, qboolean all )
{
	qboolean parked = qfalse;

	while ( s_bakePending ) {
		if ( all || waitOne ) {
			sceKernelWaitSema( s_bakeDoneSema, 1, NULL );
			waitOne = qfalse;
		} else if ( sceKernelPollSema( s_bakeDoneSema, 1 ) < 0 ) {
			break;
		}
		sceKernelLockMutex( s_bakeMutex, 1, NULL );
		const int j = s_bakeDone[s_bakeDoneTail];
		s_bakeDoneTail = ( s_bakeDoneTail + 1 ) % BAKE_QUEUE;
		sceKernelUnlockMutex( s_bakeMutex, 1 );

		if ( !parked ) {
			// the upload touches the texture table the backend binds from
			if ( r_renderThread && r_renderThread->integer )
				R_IssuePendingRenderCommands();
			parked = qtrue;
		}
		R_BakeRetire( &s_bakeJobs[j] );
	}
}

void R_BakeDrainAll( void )   { R_BakeDrain( qfalse, qtrue ); }
void R_BakeDrainReady( void ) { if ( s_bakePending ) R_BakeDrain( qfalse, qfalse ); }

/*
===============
R_BakeEnqueue

Hands a cacheable image to the workers. The image gets its final dimensions now and its texels later.
===============
*/
static qboolean R_BakeEnqueue( image_t *image, unsigned *pic, int width, int height, qboolean mipmap,
							   qboolean allowPicmip, qboolean allowTC, int clampMode, const char *key )
{
	if ( !key[0] || !allowTC || !r_texCacheCompressed || !r_texCacheCompressed->integer
		|| glConfig.textureCompression != TC_S3TC_DXT || width * height < BAKE_MIN_PIXELS ) {
		return qfalse;
	}
	if ( !R_BakeEnsurePool() )
		return qfalse;

	// the dimensions Upload32 would arrive at, so the image reports them before the chain exists
	int w = width, h = height, halvings = 0;
	if ( allowPicmip ) {
		for ( int i = 0; i < r_picmip->integer; i++ ) {
			w >>= 1; h >>= 1;
			if ( w < 1 ) w = 1;
			if ( h < 1 ) h = 1;
			halvings++;
		}
	}
	while ( w > glConfig.maxTextureSize || h > glConfig.maxTextureSize ) {
		w >>= 1; h >>= 1;
		halvings++;
	}

	const int blobCap = w * h * 2 + 4096;	// a full DXT5 chain is under w*h*4/3
	const int bytes   = width * height * 4 + blobCap;

	// the decoder shares the temp arena, so earlier jobs are waited out rather than piled up
	while ( s_bakePending && s_bakeInflight + bytes > BAKE_INFLIGHT_MAX )
		R_BakeDrain( qtrue, qfalse );
	R_BakeDrain( qfalse, qfalse );
	if ( !s_bakeNumFree )
		R_BakeDrain( qtrue, qfalse );

	byte *blob = (byte *)Z_Malloc( blobCap, TAG_TEMP_WORKSPACE, qfalse );
	if ( !blob )
		return qfalse;

	const int j = s_bakeFree[--s_bakeNumFree];
	bakeJob_t *job = &s_bakeJobs[j];
	job->image     = image;
	job->pic       = pic;
	job->blob      = blob;
	job->blobCap   = blobCap;
	job->srcWidth  = width;
	job->srcHeight = height;
	job->width     = w;
	job->height    = h;
	job->halvings  = halvings;
	job->mipmap    = mipmap;
	job->allowPicmip = allowPicmip;
	job->clampMode = clampMode;
	job->store     = !s_texCacheKeep;
	job->bytes     = bytes;
	Q_strncpyz( job->key, key, sizeof( job->key ) );

	image->width  = (word)w;
	image->height = (word)h;

	sceKernelLockMutex( s_bakeMutex, 1, NULL );
	s_bakeReq[s_bakeReqHead] = j;
	s_bakeReqHead = ( s_bakeReqHead + 1 ) % BAKE_QUEUE;
	sceKernelUnlockMutex( s_bakeMutex, 1 );
	s_bakePending++;
	s_bakeInflight += bytes;
	s_bakeTookPic = qtrue;
	sceKernelSignalSema( s_bakeWake, 1 );
	return qtrue;
}
#endif // VITA

static void Upload32( unsigned *data,
						 GLenum format,
						 qboolean mipmap,
						 qboolean picmip,
						 qboolean isLightmap,
						 qboolean allowTC,
						 int *pformat,
						 word *pUploadWidth, word *pUploadHeight, bool bRectangle = false )
{
	GLuint uiTarget = GL_TEXTURE_2D;
	if ( bRectangle )
	{
		uiTarget = GL_TEXTURE_RECTANGLE_ARB;
	}

	if (format == GL_RGBA)
	{
		int			samples;
		int			i, c;
		byte		*scan;
		float		rMax = 0, gMax = 0, bMax = 0;
		int			width = *pUploadWidth;
		int			height = *pUploadHeight;

		//
		// perform optional picmip operation
		//
		if ( picmip ) {
			for(i = 0; i < r_picmip->integer; i++) {
				R_MipMap( (byte *)data, width, height );
				width >>= 1;
				height >>= 1;
				if (width < 1) {
					width = 1;
				}
				if (height < 1) {
					height = 1;
				}
			}
		}

		//
		// clamp to the current upper OpenGL limit
		// scale both axis down equally so we don't have to
		// deal with a half mip resampling
		//
		while ( width > glConfig.maxTextureSize	|| height > glConfig.maxTextureSize ) {
			R_MipMap( (byte *)data, width, height );
			width >>= 1;
			height >>= 1;
		}

		//
		// scan the texture for each channel's max values
		// and verify if the alpha channel is being used or not
		//
		c = width*height;
		scan = ((byte *)data);
		samples = 3;
		for ( i = 0; i < c; i++ )
		{
			if ( scan[i*4+0] > rMax )
			{
				rMax = scan[i*4+0];
			}
			if ( scan[i*4+1] > gMax )
			{
				gMax = scan[i*4+1];
			}
			if ( scan[i*4+2] > bMax )
			{
				bMax = scan[i*4+2];
			}
			if ( scan[i*4 + 3] != 255 )
			{
				samples = 4;
				break;
			}
		}

		// select proper internal format
		if ( samples == 3 )
		{
			if ( glConfig.textureCompression == TC_S3TC && allowTC )
			{
				*pformat = GL_RGB4_S3TC;
			}
			else if ( glConfig.textureCompression == TC_S3TC_DXT && allowTC )
			{	// Compress purely color - no alpha
				if ( r_texturebits->integer == 16 ) {
					*pformat = GL_COMPRESSED_RGB_S3TC_DXT1_EXT;	//this format cuts to 16 bit
				}
#ifdef VITA
				else if ( r_texCacheCompressed && r_texCacheCompressed->integer ) {
					*pformat = GL_COMPRESSED_RGB_S3TC_DXT1_EXT;	// opaque -> DXT1 8:1; DXT5's alpha block would be wasted
				}
#endif
				else {//if we aren't using 16 bit then, use 32 bit compression
					*pformat = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
				}
			}
			else if ( isLightmap && r_texturebitslm->integer > 0 )
			{
				int lmBits = r_texturebitslm->integer & 0x30; // 16 or 32
				// Allow different bit depth when we are a lightmap
				if ( lmBits == 16 )
					*pformat = GL_RGB5;
				else
					*pformat = GL_RGB8;
			}
			else if ( r_texturebits->integer == 16 )
			{
				*pformat = GL_RGB5;
			}
			else if ( r_texturebits->integer == 32 )
			{
				*pformat = GL_RGB8;
			}
			else
			{
				*pformat = 3;
			}
		}
		else if ( samples == 4 )
		{
			if ( glConfig.textureCompression == TC_S3TC_DXT && allowTC)
			{	// Compress both alpha and color
				*pformat = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
			}
			else if ( r_texturebits->integer == 16 )
			{
				*pformat = GL_RGBA4;
			}
			else if ( r_texturebits->integer == 32 )
			{
				*pformat = GL_RGBA8;
			}
			else
			{
				*pformat = 4;
			}
		}

		*pUploadWidth = width;
		*pUploadHeight = height;

#ifdef VITA
		// DXT cache write path: encode the mip chain, upload it pre-compressed, store it.
		// Alloc failure falls through to stock RGBA.
		if ( !bRectangle && r_texCacheCompressed && r_texCacheCompressed->integer && s_uploadDxtKey[0]
			&& ( *pformat == GL_COMPRESSED_RGB_S3TC_DXT1_EXT || *pformat == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT ) )
		{
			const int isDxt5  = ( *pformat == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT );
			const int blobCap = width * height * 2 + 4096;	// full DXT5 chain < w*h*4/3, so *2 is plenty
			byte *blob = (byte *)Z_Malloc( blobCap, TAG_TEMP_WORKSPACE, qfalse );
			if ( blob )
			{
				unsigned mipSizes[TEXCACHE_MAX_MIPS];
				int      mipCount = 0, blobOfs = 0;
				int      mw = width, mh = height, miplevel = 0;

				if ( mipmap )
					R_LightScaleTexture( data, mw, mh, qfalse );

				mipSizes[mipCount] = R_DxtEncodeUploadAppend( 0, *pformat, mw, mh, (byte *)data, blob, blobOfs );
				blobOfs += mipSizes[mipCount]; mipCount++;

				if ( mipmap )
				{
					while ( ( mw > 1 || mh > 1 ) && mipCount < TEXCACHE_MAX_MIPS )
					{
						R_MipMap( (byte *)data, mw, mh );
						mw >>= 1; mh >>= 1;
						if ( mw < 1 ) mw = 1;
						if ( mh < 1 ) mh = 1;
						miplevel++;
						if ( r_colorMipLevels->integer )
							R_BlendOverTexture( (byte *)data, mw * mh, mipBlendColors[miplevel] );
						mipSizes[mipCount] = R_DxtEncodeUploadAppend( miplevel, *pformat, mw, mh, (byte *)data, blob, blobOfs );
						blobOfs += mipSizes[mipCount]; mipCount++;
					}
				}

				texCacheHdrDxt_t hdr;
				hdr.magic     = TEXCACHE_MAGIC_DXT;
				hdr.format    = isDxt5 ? TEXCACHE_FMT_DXT5 : TEXCACHE_FMT_DXT1;
				hdr.width     = (unsigned)width;
				hdr.height    = (unsigned)height;
				hdr.mipCount  = (unsigned)mipCount;
				hdr.picmip    = (unsigned)( picmip && r_picmip ? r_picmip->integer : 0 );
				hdr.flags     = TEXCACHE_FLAG_VALID | ( mipmap ? TEXCACHE_FLAG_MIPMAP : 0 );
				hdr.totalSize = (unsigned)blobOfs;
#ifdef USE_GXM_NATIVE
				GXM_TexUploadDxt( glState.currenttextures[glState.currenttmu], blob, (unsigned)blobOfs,
					(unsigned)width, (unsigned)height, (unsigned)mipCount, isDxt5 != 0 );
#endif
				if ( !s_texCacheKeep )
					R_TexCacheStoreDxt( s_uploadDxtKey, &hdr, mipSizes, blob );
				Z_Free( blob );
				goto done;
			}
		}
#endif

		// copy or resample data as appropriate for first MIP level
		if (!mipmap)
		{
			qglTexImage2D( uiTarget, 0, *pformat, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data );
#ifdef USE_GXM_NATIVE
			// GXM has no rectangle target, and that path never bound through GL_Bind
			if ( !bRectangle )
				GXM_TexUpload( glState.currenttextures[glState.currenttmu], data, width, height );
#endif
			goto done;
		}

		R_LightScaleTexture (data, width, height, (qboolean)!mipmap );

		qglTexImage2D( uiTarget, 0, *pformat, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data );
#ifdef USE_GXM_NATIVE
		if ( !bRectangle )
			GXM_TexUpload( glState.currenttextures[glState.currenttmu], data, width, height );
		// the GXM RGBA upload keeps level 0 only, so the chain below would be thrown away
		goto done;
#endif

		if (mipmap)
		{
			int		miplevel;

			miplevel = 0;
			while (width > 1 || height > 1)
			{
				R_MipMap( (byte *)data, width, height );
				width >>= 1;
				height >>= 1;
				if (width < 1)
					width = 1;
				if (height < 1)
					height = 1;
				miplevel++;

				if ( r_colorMipLevels->integer )
				{
					R_BlendOverTexture( (byte *)data, width * height, mipBlendColors[miplevel] );
				}

				qglTexImage2D( uiTarget, miplevel, *pformat, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data );
			}
		}
	}
	else
	{
	}

done:

	if (mipmap)
	{
		qglTexParameterf(uiTarget, GL_TEXTURE_MIN_FILTER, gl_filter_min);
		qglTexParameterf(uiTarget, GL_TEXTURE_MAG_FILTER, gl_filter_max);
		if(r_ext_texture_filter_anisotropic->integer>1 && glConfig.maxTextureFilterAnisotropy>0)
		{
			qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, r_ext_texture_filter_anisotropic->value );
		}
	}
	else
	{
		qglTexParameterf(uiTarget, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
		qglTexParameterf(uiTarget, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	}

	GL_CheckErrors();
}

static void GL_ResetBinds(void)
{
	memset( glState.currenttextures, 0, sizeof( glState.currenttextures ) );
	if ( qglActiveTextureARB ) {
		GL_SelectTexture( 1 );
		qglBindTexture( GL_TEXTURE_2D, 0 );
		GL_SelectTexture( 0 );
		qglBindTexture( GL_TEXTURE_2D, 0 );
	} else {
		qglBindTexture( GL_TEXTURE_2D, 0 );
	}
}


// special function used in conjunction with "devmapbsp"...
//
// (avoid using ri->xxxx stuff here in case running on dedicated)
//
void R_Images_DeleteLightMaps(void)
{
#ifdef VITA
	R_BakeDrainAll();
#endif
	for (AllocatedImages_t::iterator itImage = AllocatedImages.begin(); itImage != AllocatedImages.end(); /* empty */)
	{
		image_t *pImage = (*itImage).second;

		if (pImage->imgName[0] == '*' && strstr(pImage->imgName,"lightmap"))	// loose check, but should be ok
		{
			R_Images_DeleteImageContents(pImage);

			AllocatedImages.erase(itImage++);
		}
		else
		{
			++itImage;
		}
	}

	GL_ResetBinds();
}

// special function currently only called by Dissolve code...
//
void R_Images_DeleteImage(image_t *pImage)
{
#ifdef VITA
	R_BakeDrainAll();
#endif
	// Even though we supply the image handle, we need to get the corresponding iterator entry...
	//
	AllocatedImages_t::iterator itImage = AllocatedImages.find(pImage->imgName);
	if (itImage != AllocatedImages.end())
	{
		R_Images_DeleteImageContents(pImage);
		AllocatedImages.erase(itImage);
	}
	else
	{
		assert(0);
	}
}

// called only at app startup, vid_restart, app-exit
//
void R_Images_Clear(void)
{
#ifdef VITA
	R_BakeDrainAll();
#endif
	image_t *pImage;
	//	int iNumImages =
					  R_Images_StartIteration();
	while ( (pImage = R_Images_GetNextIteration()) != NULL)
	{
		R_Images_DeleteImageContents(pImage);
	}

	AllocatedImages.clear();
	s_imageMisses.clear();

	giTextureBindNum = 1024;
}


void RE_RegisterImages_Info_f( void )
{
	image_t *pImage	= NULL;
	int iImage		= 0;
	int iTexels		= 0;

	int iNumImages	= R_Images_StartIteration();
	while ( (pImage	= R_Images_GetNextIteration()) != NULL)
	{
		ri.Printf( PRINT_ALL, "%d: (%4dx%4dy) \"%s\"",iImage, pImage->width, pImage->height, pImage->imgName);
		ri.Printf( PRINT_DEVELOPER, S_COLOR_RED ", levused %d",pImage->iLastLevelUsedOn);
		ri.Printf( PRINT_ALL, "\n");

		iTexels += pImage->width * pImage->height;
		iImage++;
	}
	ri.Printf( PRINT_ALL, "%d Images. %d (%.2fMB) texels total, (not including mipmaps)\n",iNumImages, iTexels, (float)iTexels / 1024.0f / 1024.0f);
	ri.Printf( PRINT_DEVELOPER, S_COLOR_RED "RE_RegisterMedia_GetLevel(): %d",RE_RegisterMedia_GetLevel());
}

// currently, this just goes through all the images and dumps any not referenced on this level...
//
qboolean RE_RegisterImages_LevelLoadEnd(void)
{
#ifdef VITA
	R_BakeDrainAll();	// every image of this level is uploaded before anything can be purged
#endif
	ri.Printf( PRINT_DEVELOPER, S_COLOR_RED "RE_RegisterImages_LevelLoadEnd():\n");

//	int iNumImages = AllocatedImages.size();	// more for curiosity, really.

	qboolean imageDeleted = qfalse;
	for (AllocatedImages_t::iterator itImage = AllocatedImages.begin(); itImage != AllocatedImages.end(); /* blank */)
	{
		qboolean bEraseOccured = qfalse;

		image_t *pImage = (*itImage).second;

		// don't un-register system shaders (*fog, *dlight, *white, *default), but DO de-register lightmaps ("*<mapname>/lightmap%d")
		if (pImage->imgName[0] != '*' || strchr(pImage->imgName,'/'))
		{
			// image used on this level?
			//
			if ( pImage->iLastLevelUsedOn != RE_RegisterMedia_GetLevel() )
			{
				// nope, so dump it...
				//
				ri.Printf( PRINT_DEVELOPER, S_COLOR_RED "Dumping image \"%s\"\n",pImage->imgName);

				R_Images_DeleteImageContents(pImage);

				AllocatedImages.erase(itImage++);
				bEraseOccured = qtrue;
				imageDeleted = qtrue;
			}
		}

		if ( !bEraseOccured )
		{
			++itImage;
		}
	}


	// this check can be deleted AFAIC, it seems to be just a quake thing...
	//
//	iNumImages = R_Images_StartIteration();
//	if (iNumImages > MAX_DRAWIMAGES)
//	{
//		ri.Printf( PRINT_ALL, S_COLOR_YELLOW  "Level uses %d images, old limit was MAX_DRAWIMAGES (%d)\n", iNumImages, MAX_DRAWIMAGES);
//	}

	ri.Printf( PRINT_DEVELOPER, S_COLOR_RED "RE_RegisterImages_LevelLoadEnd(): Ok\n");

	GL_ResetBinds();

	return imageDeleted;
}



// returns image_t struct if we already have this, else NULL. No disk-open performed
//	(important for creating default images).
//
// This is called by both R_FindImageFile and anything that creates default images...
//
static image_t *R_FindImageFile_NoLoad(const char *name, qboolean mipmap, qboolean allowPicmip, qboolean allowTC, int glWrapClampMode )
{
	if (!name) {
		return NULL;
	}

	char *pName = GenerateImageMappingName(name);

	//
	// see if the image is already loaded
	//
	AllocatedImages_t::iterator itAllocatedImage = AllocatedImages.find(pName);
	if (itAllocatedImage != AllocatedImages.end())
	{
		image_t *pImage = (*itAllocatedImage).second;

		// the white image can be used with any set of parms, but other mismatches are errors...
		//
		if ( strcmp( pName, "*white" ) ) {
			if ( pImage->mipmap != !!mipmap ) {
				ri.Printf( PRINT_ALL, S_COLOR_YELLOW  "WARNING: reused image %s with mixed mipmap parm\n", pName );
			}
			if ( pImage->allowPicmip != !!allowPicmip ) {
				ri.Printf( PRINT_ALL, S_COLOR_YELLOW  "WARNING: reused image %s with mixed allowPicmip parm\n", pName );
			}
			if ( pImage->wrapClampMode != glWrapClampMode ) {
				ri.Printf( PRINT_ALL, S_COLOR_YELLOW  "WARNING: reused image %s with mixed glWrapClampMode parm\n", pName );
			}
		}

		pImage->iLastLevelUsedOn = RE_RegisterMedia_GetLevel();

		return pImage;
	}

	return NULL;
}



/*
================
R_CreateImage

This is the only way any image_t are created
================
*/
image_t *R_CreateImage( const char *name, const byte *pic, int width, int height,
					   GLenum format, qboolean mipmap, qboolean allowPicmip, qboolean allowTC, int glWrapClampMode, bool bRectangle )
{
	image_t		*image;
	qboolean	isLightmap = qfalse;

	if (strlen(name) >= MAX_QPATH ) {
		Com_Error (ERR_DROP, "R_CreateImage: \"%s\" is too long\n", name);
	}

#ifdef VITA
	s_bakeTookPic = qfalse;
	// waits out the backend before this main-thread GL upload (no-op before tr.registered)
	if ( r_renderThread && r_renderThread->integer ) {
		R_IssuePendingRenderCommands();
	}
#endif

	if(glConfig.clampToEdgeAvailable && glWrapClampMode == GL_CLAMP) {
		glWrapClampMode = GL_CLAMP_TO_EDGE;
	}

	if (name[0] == '*')
	{
		const char *psLightMapNameSearchPos = strrchr(name,'/');
		if (  psLightMapNameSearchPos && !strncmp( psLightMapNameSearchPos+1, "lightmap", 8 ) ) {
			isLightmap = qtrue;
		}
	}

	if ( (width&(width-1)) || (height&(height-1)) )
	{
		Com_Error( ERR_FATAL, "R_CreateImage: %s dimensions (%i x %i) not power of 2!\n",name,width,height);
	}

	image = R_FindImageFile_NoLoad(name, mipmap, allowPicmip, allowTC, glWrapClampMode );
	if (image) {
		return image;
	}

	image = (image_t*) Z_Malloc( sizeof( image_t ), TAG_IMAGE_T, qtrue );
//	memset(image,0,sizeof(*image));	// qtrue above does this

	image->texnum = 1024 + giTextureBindNum++;	// ++ is of course staggeringly important...

	// record which map it was used on...
	//
	image->iLastLevelUsedOn = RE_RegisterMedia_GetLevel();

	image->mipmap = !!mipmap;
	image->allowPicmip = !!allowPicmip;

	Q_strncpyz(image->imgName, name, sizeof(image->imgName));

	image->width = width;
	image->height = height;
	image->wrapClampMode = glWrapClampMode;

	if ( qglActiveTextureARB ) {
		GL_SelectTexture( 0 );
	}

	GLuint uiTarget = GL_TEXTURE_2D;
	if ( bRectangle )
	{
		qglDisable( uiTarget );
		uiTarget = GL_TEXTURE_RECTANGLE_ARB;
		qglEnable( uiTarget );
		glWrapClampMode = GL_CLAMP_TO_EDGE;	// default mode supported by rectangle.
		qglBindTexture( uiTarget, image->texnum );
	}
	else
	{
		GL_Bind(image);
	}

#ifdef VITA
	// key for Upload32's DXT cache; lightmaps (*) and built-ins ($) get no key = no caching
	if ( !bRectangle && name[0] != '$' && name[0] != '*' )
		Q_strncpyz( s_uploadDxtKey, name, sizeof( s_uploadDxtKey ) );
	else
		s_uploadDxtKey[0] = '\0';

	// a cacheable image bakes on the worker cores and gets its texels when they finish
	const qboolean baked = (qboolean)( format == GL_RGBA && !isLightmap && R_BakeEnqueue( image, (unsigned *)pic,
		width, height, (qboolean)image->mipmap, allowPicmip, allowTC, glWrapClampMode, s_uploadDxtKey ) );
	if ( !baked )
#endif
	Upload32( (unsigned *)pic,	format,
								(qboolean)image->mipmap,
								allowPicmip,
								isLightmap,
								allowTC,
								&image->internalFormat,
								&image->width,
								&image->height, bRectangle );

#ifdef VITA
	s_uploadDxtKey[0] = '\0';	// clear it so a later upload can't reuse this name
	s_texCacheKeep = qfalse;
#endif

	qglTexParameterf( uiTarget, GL_TEXTURE_WRAP_S, glWrapClampMode );
	qglTexParameterf( uiTarget, GL_TEXTURE_WRAP_T, glWrapClampMode );
#ifdef USE_GXM_NATIVE
	// GL_REPEAT is 0x2901; anything else here is one of the clamp modes
	GXM_TexFilter( image->texnum, 1, glWrapClampMode != GL_REPEAT );
#endif

	qglBindTexture( uiTarget, 0 );	//jfm: i don't know why this is here, but it breaks lightmaps when there's only 1
	glState.currenttextures[glState.currenttmu] = 0;	//mark it not bound

	const char *psNewName = GenerateImageMappingName(name);
	Q_strncpyz(image->imgName, psNewName, sizeof(image->imgName));
	AllocatedImages[ image->imgName ] = image;

	if ( bRectangle )
	{
		qglDisable( uiTarget );
		qglEnable( GL_TEXTURE_2D );
	}

	return image;
}

#ifdef VITA
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

// DXT mip-chain cache (see the block above Upload32); dir shared with JA SP (same assets)
// FNV-1a 64 of a key: how the v1 pack index, 'JKTD' delta records and loose files are looked up.
static unsigned long long R_TexCacheDxt_Hash( const char *name )
{
	unsigned long long h = 14695981039346656037ULL;
	for ( const char *p = name; *p; ++p ) { h ^= (unsigned char)*p; h *= 1099511628211ULL; }
	return h;
}

// The cache key: the name as GenerateImageMappingName files it, lowercase, '/' separators, no extension.
static void R_TexCacheDxt_Key( const char *name, char out[MAX_QPATH] )
{
	R_ImageMappingName( name, out, MAX_QPATH );
}

// sharded on the top hash byte: an exFAT lookup scans the directory, and one flat
// folder holds every baked entry
static void R_TexCacheDxt_Path( const char *name, char *out, int outSize )
{
	const unsigned long long h = R_TexCacheDxt_Hash( name );
	const unsigned shard = (unsigned)( h >> 56 );
	Com_sprintf( out, outSize, "ux0:data/JAVITA/texcache_dxt/%02x/%016llx.bin", shard, h );
}

// where entries baked before the sharding lived; a card full of them still reads
static void R_TexCacheDxt_PathFlat( const char *name, char *out, int outSize )
{
	Com_sprintf( out, outSize, "ux0:data/JAVITA/texcache_dxt/%016llx.bin", R_TexCacheDxt_Hash( name ) );
}

// pack.bin: the loose entries byte-for-byte behind one index, so a hit is one read on one fd
#define TEXCACHE_PACK_MAGIC		0x50544B4Au	// "JKTP"
#define TEXCACHE_PACK_VERSION	2u			// 2 = index keyed by name, 1 = index keyed by name hash; both are read
#define TEXCACHE_PACK_VERSION_V1 1u
#define TEXCACHE_PACK_MAX_COUNT	( 1u << 20 )
#define TEXCACHE_PACK_MAX_ENTRY	( 32u * 1024 * 1024 + 65536 )	// caps the alloc a corrupt index could ask for
typedef struct {
	unsigned int magic, version, count, namesSize;	// namesSize is a zero pad in v1
} texCachePackHdr_t;
typedef struct {					// v2 index entry, sorted ascending by key bytes
	unsigned int nameOffset;		// key start inside the name table that follows the index
	unsigned int nameLen;			// key bytes, no terminator
	unsigned int offset;			// entry start from the file start
	unsigned int size;				// header + mip sizes + payload
} texCachePackEntry_t;
typedef struct {					// v1 index entry, sorted ascending by nameHash
	unsigned long long nameHash;	// R_TexCacheDxt_Hash of the key
	unsigned int offset;
	unsigned int size;
} texCachePackEntryV1_t;
typedef struct {					// an opened pack or delta: named entries, hashed entries, or both
	SceUID					fd;
	texCachePackEntry_t		*entries;		// sorted by key
	unsigned				count;
	char					*names;			// the key bytes the entries point into
	unsigned				namesSize;
	texCachePackEntryV1_t	*hashed;		// sorted by nameHash
	unsigned				hashedCount;
} texCacheIndex_t;
static texCacheIndex_t		s_texPack = { -1 };
static qboolean				s_texPackTried;		// one open attempt per renderer lifetime

// pack.delta: what the device bakes, appended as self-describing records with no index
#define TEXCACHE_DELTA_MAGIC		0x45544B4Au	// "JKTE": u32 magic, u32 nameLen, u32 size, u32 pad, key bytes, entry
#define TEXCACHE_DELTA_MAGIC_V1		0x44544B4Au	// "JKTD": u32 magic, u64 nameHash, u32 size, u32 pad, entry
#define TEXCACHE_DELTA_HDR_SIZE		16u
#define TEXCACHE_DELTA_HDR_SIZE_V1	20u
#define TEXCACHE_DELTA_PEEK			( TEXCACHE_DELTA_HDR_SIZE + MAX_QPATH )	// one read covers either header and a key
#define TEXCACHE_DELTA_MAX_COUNT	( 1u << 20 )
static texCacheIndex_t		s_texDelta = { -1 };	// newest record per key
static SceUID				s_texDeltaWrFd = -1;
static qboolean				s_texDeltaWrTried;		// one append-side open per renderer lifetime
static SceOff				s_texDeltaWrEnd;		// file length after the last whole record
static volatile unsigned char s_texDeltaLock;		// serializes appends from the bakers and main

static inline void R_TexCacheDxt_DeltaLock( void )   { while ( __atomic_test_and_set( &s_texDeltaLock, __ATOMIC_ACQUIRE ) ) {} }
static inline void R_TexCacheDxt_DeltaUnlock( void ) { __atomic_clear( &s_texDeltaLock, __ATOMIC_RELEASE ); }

static void R_TexCacheDxt_DeltaPath( char *out, int outSize )
{
	Q_strncpyz( out, "ux0:data/JAVITA/texcache_dxt/pack.delta", outSize );
}

// memcmp order on the key bytes, the shorter key first on a shared prefix.
static int R_TexCacheDxt_KeyCmp( const char *a, unsigned alen, const char *b, unsigned blen )
{
	const int c = memcmp( a, b, alen < blen ? alen : blen );
	if ( c ) return c;
	return alen < blen ? -1 : ( alen > blen ? 1 : 0 );
}

typedef struct {					// one delta record header as read from the file
	unsigned			hdrSize;	// bytes before the key (v2) or the entry (v1)
	unsigned			nameLen;	// 0 for a v1 record
	const char			*name;		// the key inside the caller's buffer, v2 only
	unsigned long long	hash;		// v1 only
	unsigned			size;		// entry bytes
} texCacheDeltaRec_t;

// Decodes the record at pos from its first have bytes; false when it is not a whole record inside the file.
static qboolean R_TexCacheDxt_DeltaHdrParse( const byte *b, unsigned have, SceOff pos, SceOff fileSize, texCacheDeltaRec_t *rec )
{
	unsigned magic;
	if ( have < TEXCACHE_DELTA_HDR_SIZE ) return qfalse;
	memcpy( &magic, b, 4 );
	rec->nameLen = 0;
	rec->name = NULL;
	rec->hash = 0;
	if ( magic == TEXCACHE_DELTA_MAGIC )
	{
		rec->hdrSize = TEXCACHE_DELTA_HDR_SIZE;
		memcpy( &rec->nameLen, b + 4, 4 );
		memcpy( &rec->size, b + 8, 4 );
		if ( rec->nameLen == 0 || rec->nameLen >= MAX_QPATH || have < rec->hdrSize + rec->nameLen ) return qfalse;
		rec->name = (const char *)b + rec->hdrSize;
	}
	else if ( magic == TEXCACHE_DELTA_MAGIC_V1 )
	{
		rec->hdrSize = TEXCACHE_DELTA_HDR_SIZE_V1;
		if ( have < rec->hdrSize ) return qfalse;
		memcpy( &rec->hash, b + 4, 8 );
		memcpy( &rec->size, b + 12, 4 );
	}
	else
	{
		return qfalse;
	}
	return (qboolean)( rec->size >= sizeof(texCacheHdrDxt_t) + sizeof(unsigned) && rec->size <= TEXCACHE_PACK_MAX_ENTRY
		&& pos + rec->hdrSize + rec->nameLen + rec->size <= fileSize );
}

// Writes a 'JKTE' record header and its key into b; returns the bytes written.
static unsigned R_TexCacheDxt_DeltaHdrBuild( byte *b, const char *key, unsigned keyLen, unsigned size )
{
	const unsigned magic = TEXCACHE_DELTA_MAGIC, pad = 0;
	memcpy( b, &magic, 4 );
	memcpy( b + 4, &keyLen, 4 );
	memcpy( b + 8, &size, 4 );
	memcpy( b + 12, &pad, 4 );
	memcpy( b + 16, key, keyLen );
	return TEXCACHE_DELTA_HDR_SIZE + keyLen;
}

// Grows list to at least need items of elem bytes, doubling from first up to maxCap; false leaves list untouched.
static qboolean R_TexCacheDxt_Grow( void **list, unsigned *cap, unsigned need, unsigned elem, unsigned first, unsigned maxCap )
{
	unsigned c = *cap;
	while ( c < need ) c = c ? c * 2 : first;
	if ( c == *cap ) return qtrue;
	if ( c > maxCap ) return qfalse;
	void *grown = realloc( *list, (size_t)c * elem );
	if ( !grown ) return qfalse;
	*list = grown;
	*cap = c;
	return qtrue;
}

// Walks the records from the file start and returns the length of the whole-record prefix; fills ix when given.
static SceOff R_TexCacheDxt_DeltaScan( SceUID fd, SceOff fileSize, texCacheIndex_t *ix )
{
	texCachePackEntry_t *named = NULL;
	texCachePackEntryV1_t *hashed = NULL;
	char *names = NULL;
	unsigned n = 0, nCap = 0, h = 0, hCap = 0, namesLen = 0, namesCap = 0;
	qboolean indexing = (qboolean)( ix != NULL );
	SceOff pos = 0;
	for ( ;; )
	{
		byte b[TEXCACHE_DELTA_PEEK];
		texCacheDeltaRec_t rec;
		if ( pos + TEXCACHE_DELTA_HDR_SIZE > fileSize ) break;
		const int have = sceIoPread( fd, b, sizeof(b), pos );
		if ( have < (int)TEXCACHE_DELTA_HDR_SIZE || !R_TexCacheDxt_DeltaHdrParse( b, (unsigned)have, pos, fileSize, &rec ) ) break;
		const SceOff entryPos = pos + rec.hdrSize + rec.nameLen;
		// the index stops at its caps while the walk goes on, so the prefix length stays exact
		if ( indexing && entryPos + rec.size <= 0xFFFFFFFFll )
		{
			if ( rec.name )
			{
				indexing = (qboolean)( R_TexCacheDxt_Grow( (void **)&named, &nCap, n + 1, sizeof(*named), 256, TEXCACHE_DELTA_MAX_COUNT )
					&& R_TexCacheDxt_Grow( (void **)&names, &namesCap, namesLen + rec.nameLen, 1, 16384, TEXCACHE_DELTA_MAX_COUNT * MAX_QPATH ) );
				if ( indexing )
				{
					memcpy( names + namesLen, rec.name, rec.nameLen );
					named[n].nameOffset = namesLen;
					named[n].nameLen = rec.nameLen;
					named[n].offset = (unsigned)entryPos;
					named[n].size = rec.size;
					namesLen += rec.nameLen;
					n++;
				}
			}
			else
			{
				indexing = R_TexCacheDxt_Grow( (void **)&hashed, &hCap, h + 1, sizeof(*hashed), 256, TEXCACHE_DELTA_MAX_COUNT );
				if ( indexing )
				{
					hashed[h].nameHash = rec.hash;
					hashed[h].offset = (unsigned)entryPos;
					hashed[h].size = rec.size;
					h++;
				}
			}
		}
		pos = entryPos + rec.size;
	}
	if ( ix )
	{
		ix->entries = named;
		ix->count = n;
		ix->names = names;
		ix->namesSize = namesLen;
		ix->hashed = hashed;
		ix->hashedCount = h;
	}
	else
	{
		free( named );
		free( hashed );
		free( names );
	}
	return pos;
}

static const char *s_texSortNames;	// name table of the index qsort is ordering

// Orders by key, then file position, so the last of a run is the newest record for that key.
static int R_TexCacheDxt_DeltaCmp( const void *a, const void *b )
{
	const texCachePackEntry_t *x = (const texCachePackEntry_t *)a, *y = (const texCachePackEntry_t *)b;
	const int c = R_TexCacheDxt_KeyCmp( s_texSortNames + x->nameOffset, x->nameLen, s_texSortNames + y->nameOffset, y->nameLen );
	if ( c ) return c;
	return x->offset < y->offset ? -1 : ( x->offset > y->offset ? 1 : 0 );
}

// Orders by nameHash, then file position, so the last of a run is the newest v1 record for that hash.
static int R_TexCacheDxt_DeltaCmpV1( const void *a, const void *b )
{
	const texCachePackEntryV1_t *x = (const texCachePackEntryV1_t *)a, *y = (const texCachePackEntryV1_t *)b;
	if ( x->nameHash != y->nameHash ) return x->nameHash < y->nameHash ? -1 : 1;
	return x->offset < y->offset ? -1 : ( x->offset > y->offset ? 1 : 0 );
}

// Closes the fd and frees the index arrays.
static void R_TexCacheDxt_IndexClose( texCacheIndex_t *ix )
{
	if ( ix->fd >= 0 ) sceIoClose( ix->fd );
	free( ix->entries );
	free( ix->names );
	free( ix->hashed );
	memset( ix, 0, sizeof(*ix) );
	ix->fd = -1;
}

// Indexes the delta once per renderer lifetime; a missing or empty file leaves it closed.
static void R_TexCacheDxt_DeltaOpen( void )
{
	char path[256];
	R_TexCacheDxt_DeltaPath( path, sizeof(path) );
	const SceUID fd = sceIoOpen( path, SCE_O_RDONLY, 0 );
	if ( fd < 0 ) return;
	SceIoStat st;
	texCacheIndex_t ix;
	memset( &ix, 0, sizeof(ix) );
	ix.fd = fd;
	if ( sceIoGetstatByFd( fd, &st ) >= 0 )
		R_TexCacheDxt_DeltaScan( fd, st.st_size, &ix );
	if ( !ix.count && !ix.hashedCount )
	{
		R_TexCacheDxt_IndexClose( &ix );
		return;
	}
	if ( ix.count )
	{
		s_texSortNames = ix.names;
		qsort( ix.entries, ix.count, sizeof(*ix.entries), R_TexCacheDxt_DeltaCmp );
		unsigned n = 0;
		for ( unsigned i = 0; i < ix.count; ++i )
		{
			const texCachePackEntry_t *e = &ix.entries[i];
			if ( i + 1 < ix.count && !R_TexCacheDxt_KeyCmp( ix.names + e->nameOffset, e->nameLen,
					ix.names + e[1].nameOffset, e[1].nameLen ) ) continue;	// an older bake of the same key
			ix.entries[n++] = *e;
		}
		ix.count = n;
	}
	if ( ix.hashedCount )
	{
		qsort( ix.hashed, ix.hashedCount, sizeof(*ix.hashed), R_TexCacheDxt_DeltaCmpV1 );
		unsigned n = 0;
		for ( unsigned i = 0; i < ix.hashedCount; ++i )
		{
			if ( i + 1 < ix.hashedCount && ix.hashed[i + 1].nameHash == ix.hashed[i].nameHash ) continue;
			ix.hashed[n++] = ix.hashed[i];
		}
		ix.hashedCount = n;
	}
	s_texDelta = ix;
}

// Sets the file length; false when the filesystem refused.
static qboolean R_TexCacheDxt_DeltaTruncate( SceUID fd, SceOff len )
{
	SceIoStat st;
	memset( &st, 0, sizeof(st) );
	st.st_size = len;
	return (qboolean)( sceIoChstatByFd( fd, &st, SCE_CST_SIZE ) >= 0 );
}

// Opens the append side once per renderer lifetime, after cutting any torn tail off the file; caller holds the lock.
static void R_TexCacheDxt_DeltaOpenWrite( void )
{
	if ( s_texDeltaWrTried ) return;
	s_texDeltaWrTried = qtrue;
	sceIoMkdir( "ux0:data/JAVITA", 0777 );
	sceIoMkdir( "ux0:data/JAVITA/texcache_dxt", 0777 );
	char path[256];
	R_TexCacheDxt_DeltaPath( path, sizeof(path) );
	const SceUID fd = sceIoOpen( path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666 );
	if ( fd < 0 ) return;
	SceIoStat st;
	SceOff valid = -1;
	memset( &st, 0, sizeof(st) );
	const SceUID rd = sceIoOpen( path, SCE_O_RDONLY, 0 );
	if ( rd >= 0 )
	{
		if ( sceIoGetstatByFd( rd, &st ) >= 0 ) valid = R_TexCacheDxt_DeltaScan( rd, st.st_size, NULL );
		sceIoClose( rd );
	}
	if ( valid < 0 || ( valid < st.st_size && !R_TexCacheDxt_DeltaTruncate( fd, valid ) ) )
	{
		sceIoClose( fd );
		return;
	}
	s_texDeltaWrFd = fd;
	s_texDeltaWrEnd = valid;
}

// Opens the pack once and keeps its index; any defect leaves it closed and the loose files serve.
static void R_TexCacheDxt_PackOpen( void )
{
	if ( s_texPackTried ) return;
	s_texPackTried = qtrue;
	R_TexCacheDxt_DeltaOpen();
	const SceUID fd = sceIoOpen( "ux0:data/JAVITA/texcache_dxt/pack.bin", SCE_O_RDONLY, 0 );
	if ( fd < 0 ) return;
	texCachePackHdr_t ph;
	SceIoStat st;
	if ( sceIoGetstatByFd( fd, &st ) < 0
		|| sceIoRead( fd, &ph, sizeof(ph) ) != (int)sizeof(ph)
		|| ph.magic != TEXCACHE_PACK_MAGIC
		|| ( ph.version != TEXCACHE_PACK_VERSION && ph.version != TEXCACHE_PACK_VERSION_V1 )
		|| ph.count == 0 || ph.count > TEXCACHE_PACK_MAX_COUNT
		|| ( ph.version == TEXCACHE_PACK_VERSION && ph.namesSize > ph.count * ( MAX_QPATH - 1 ) ) )
	{
		sceIoClose( fd );
		return;
	}
	const qboolean v1 = (qboolean)( ph.version == TEXCACHE_PACK_VERSION_V1 );
	const unsigned entryBytes = v1 ? (unsigned)sizeof(texCachePackEntryV1_t) : (unsigned)sizeof(texCachePackEntry_t);
	const unsigned namesSize = v1 ? 0u : ph.namesSize;
	const unsigned idxBytes  = ph.count * entryBytes;
	const unsigned dataStart = (unsigned)sizeof(ph) + idxBytes + namesSize;
	void *idx = malloc( idxBytes );
	char *names = namesSize ? (char *)malloc( namesSize ) : NULL;
	qboolean ok = (qboolean)( idx && ( !namesSize || names )
		&& sceIoRead( fd, idx, idxBytes ) == (int)idxBytes
		&& ( !namesSize || sceIoRead( fd, names, namesSize ) == (int)namesSize ) );
	for ( unsigned i = 0; ok && i < ph.count; ++i )
	{
		unsigned offset, size;
		if ( v1 )
		{
			const texCachePackEntryV1_t *e = (const texCachePackEntryV1_t *)idx + i;
			if ( i && e[-1].nameHash >= e->nameHash ) ok = qfalse;
			offset = e->offset;
			size = e->size;
		}
		else
		{
			const texCachePackEntry_t *e = (const texCachePackEntry_t *)idx + i;
			if ( e->nameLen == 0 || e->nameLen >= MAX_QPATH || e->nameOffset > namesSize || e->nameOffset + e->nameLen > namesSize
				|| ( i && R_TexCacheDxt_KeyCmp( names + e[-1].nameOffset, e[-1].nameLen, names + e->nameOffset, e->nameLen ) >= 0 ) )
				ok = qfalse;
			offset = e->offset;
			size = e->size;
		}
		if ( size < sizeof(texCacheHdrDxt_t) + sizeof(unsigned) || size > TEXCACHE_PACK_MAX_ENTRY
			|| offset < dataStart || (SceOff)offset + size > st.st_size )
			ok = qfalse;
	}
	if ( !ok )
	{
		free( idx );
		free( names );
		sceIoClose( fd );
		return;
	}
	s_texPack.fd = fd;
	if ( v1 )
	{
		s_texPack.hashed = (texCachePackEntryV1_t *)idx;
		s_texPack.hashedCount = ph.count;
	}
	else
	{
		s_texPack.entries = (texCachePackEntry_t *)idx;
		s_texPack.count = ph.count;
		s_texPack.names = names;
		s_texPack.namesSize = namesSize;
	}
}

void R_TexCacheDxt_PackClose( void )
{
	R_TexCacheDxt_IndexClose( &s_texPack );
	s_texPackTried = qfalse;
	R_TexCacheDxt_IndexClose( &s_texDelta );
	R_TexCacheDxt_DeltaLock();
	if ( s_texDeltaWrFd >= 0 ) sceIoClose( s_texDeltaWrFd );
	s_texDeltaWrFd = -1;
	s_texDeltaWrTried = qfalse;
	R_TexCacheDxt_DeltaUnlock();
}

// Reads the whole entry for key in one positioned read, found by name, else by the v1 hash; NULL when absent.
static byte *R_TexCacheDxt_IndexedRead( const texCacheIndex_t *ix, const char *key, unsigned *size )
{
	unsigned offset, len;
	if ( ix->fd < 0 ) return NULL;
	const unsigned keyLen = (unsigned)strlen( key );
	unsigned lo = 0, hi = ix->count;
	while ( lo < hi )
	{
		const unsigned mid = lo + ( hi - lo ) / 2;
		const texCachePackEntry_t *e = &ix->entries[mid];
		if ( R_TexCacheDxt_KeyCmp( ix->names + e->nameOffset, e->nameLen, key, keyLen ) < 0 ) lo = mid + 1; else hi = mid;
	}
	if ( lo < ix->count && !R_TexCacheDxt_KeyCmp( ix->names + ix->entries[lo].nameOffset, ix->entries[lo].nameLen, key, keyLen ) )
	{
		offset = ix->entries[lo].offset;
		len = ix->entries[lo].size;
	}
	else if ( ix->hashedCount )
	{
		const unsigned long long h = R_TexCacheDxt_Hash( key );
		lo = 0;
		hi = ix->hashedCount;
		while ( lo < hi )
		{
			const unsigned mid = lo + ( hi - lo ) / 2;
			if ( ix->hashed[mid].nameHash < h ) lo = mid + 1; else hi = mid;
		}
		if ( lo == ix->hashedCount || ix->hashed[lo].nameHash != h ) return NULL;
		offset = ix->hashed[lo].offset;
		len = ix->hashed[lo].size;
	}
	else
	{
		return NULL;
	}
	byte *entry = (byte *)Z_Malloc( len, TAG_TEMP_WORKSPACE, qfalse );
	if ( !entry ) return NULL;
	if ( sceIoPread( ix->fd, entry, len, (SceOff)offset ) != (int)len )
	{
		Z_Free( entry );
		return NULL;
	}
	*size = len;
	return entry;
}

static byte *R_TexCacheDxt_PackRead( const char *key, unsigned *size )
{
	R_TexCacheDxt_PackOpen();
	return R_TexCacheDxt_IndexedRead( &s_texPack, key, size );
}

static byte *R_TexCacheDxt_DeltaRead( const char *key, unsigned *size )
{
	R_TexCacheDxt_PackOpen();
	return R_TexCacheDxt_IndexedRead( &s_texDelta, key, size );
}

// Header checks shared by the packed and loose readers; a lower baked picmip passes for R_TexCacheDxt_Picmip to trim.
static qboolean R_TexCacheDxt_HdrOk( const texCacheHdrDxt_t *hdr, qboolean mipmap, qboolean allowPicmip )
{
	if ( hdr->magic != TEXCACHE_MAGIC_DXT
		|| ( hdr->format != TEXCACHE_FMT_DXT1 && hdr->format != TEXCACHE_FMT_DXT5 )
		|| hdr->mipCount < 1 || hdr->mipCount > TEXCACHE_MAX_MIPS
		|| hdr->width == 0 || hdr->height == 0
		|| (int)hdr->width > glConfig.maxTextureSize || (int)hdr->height > glConfig.maxTextureSize )
	{
		return qfalse;
	}
	if ( hdr->picmip > (unsigned)( allowPicmip && r_picmip ? r_picmip->integer : 0 ) )
	{
		return qfalse;
	}
	// an unmipmapped entry would otherwise be given mip sampling downstream
	if ( ( hdr->flags & TEXCACHE_FLAG_VALID )
		&& ( ( hdr->flags & TEXCACHE_FLAG_MIPMAP ) != 0 ) != ( mipmap != qfalse ) )
	{
		return qfalse;
	}
	return qtrue;
}

// Sum of the mip sizes when they agree with the header, else 0.
static unsigned R_TexCacheDxt_Total( const texCacheHdrDxt_t *hdr, const unsigned *mipSizes )
{
	unsigned total = 0;
	for ( unsigned i = 0; i < hdr->mipCount; ++i ) total += mipSizes[i];
	if ( total != hdr->totalSize || total == 0 || total > (unsigned)( hdr->width * hdr->height * 2 + 4096 ) )
	{
		return 0;
	}
	return total;
}

// Drops the leading mip levels a higher r_picmip would have halved away; false when the chain runs out first.
static qboolean R_TexCacheDxt_Picmip( texCacheHdrDxt_t *hdr, unsigned *mipSizes, qboolean allowPicmip, unsigned *skipBytes )
{
	const unsigned want = (unsigned)( allowPicmip && r_picmip ? r_picmip->integer : 0 );
	unsigned w = hdr->width, h = hdr->height, lvl = 0, ofs = 0;
	for ( unsigned skip = want - hdr->picmip; skip > 0 && ( w > 1 || h > 1 ); --skip )
	{
		if ( lvl + 1 >= hdr->mipCount ) return qfalse;
		ofs += mipSizes[lvl++];
		w >>= 1; if ( w < 1 ) w = 1;
		h >>= 1; if ( h < 1 ) h = 1;
	}
	if ( lvl )
	{
		hdr->mipCount -= lvl;
		memmove( mipSizes, mipSizes + lvl, hdr->mipCount * sizeof(unsigned) );
		hdr->width = w;
		hdr->height = h;
		hdr->totalSize -= ofs;
	}
	hdr->picmip = want;
	*skipBytes = ofs;
	return qtrue;
}

// Validates one entry held in memory and returns the mip payload for this r_picmip, or NULL.
static byte *R_TexCacheDxt_ParseEntry( byte *entry, unsigned size, qboolean mipmap, qboolean allowPicmip,
									   texCacheHdrDxt_t *hdr, unsigned *mipSizes, unsigned *total )
{
	const unsigned hdrLen = (unsigned)sizeof(*hdr);
	unsigned skip = 0;
	if ( size < hdrLen ) return NULL;
	memcpy( hdr, entry, hdrLen );
	if ( !R_TexCacheDxt_HdrOk( hdr, mipmap, allowPicmip ) ) return NULL;
	const unsigned mipLen = hdr->mipCount * (unsigned)sizeof(unsigned);
	if ( size < hdrLen + mipLen ) return NULL;
	memcpy( mipSizes, entry + hdrLen, mipLen );
	*total = R_TexCacheDxt_Total( hdr, mipSizes );
	if ( !*total || size != hdrLen + mipLen + *total ) return NULL;
	if ( !R_TexCacheDxt_Picmip( hdr, mipSizes, allowPicmip, &skip ) ) return NULL;
	*total = hdr->totalSize;
	return entry + hdrLen + mipLen + skip;
}

// The validated entry for key from the delta, then the pack, with *blob at its mip payload; NULL on a miss.
static byte *R_TexCacheDxt_ReadPacked( const char *key, qboolean mipmap, qboolean allowPicmip,
									   texCacheHdrDxt_t *hdr, unsigned *mipSizes, unsigned *total, byte **blob )
{
	for ( int src = 0; src < 2; ++src )
	{
		unsigned size = 0;
		byte *entry = src == 0 ? R_TexCacheDxt_DeltaRead( key, &size ) : R_TexCacheDxt_PackRead( key, &size );
		if ( !entry ) continue;
		*blob = R_TexCacheDxt_ParseEntry( entry, size, mipmap, allowPicmip, hdr, mipSizes, total );
		if ( *blob ) return entry;
		Z_Free( entry );
	}
	return NULL;
}

// Build an image_t straight from a cached DXT mip chain, no decode or encode. Returns NULL
// on a miss, version/picmip mismatch, or corruption so the caller falls back to the load path.
static image_t *R_CreateImageFromDxtCache( const char *name, qboolean mipmap, qboolean allowPicmip,
										   qboolean allowTC, int glWrapClampMode )
{
	if ( !r_texCacheCompressed || !r_texCacheCompressed->integer ) return NULL;
	// park the render thread before touching GL from the frontend
	if ( r_renderThread && r_renderThread->integer ) {
		R_IssuePendingRenderCommands();
	}
	char key[MAX_QPATH];
	R_TexCacheDxt_Key( name, key );
	if ( !key[0] ) return NULL;
	texCacheHdrDxt_t hdr;
	unsigned mipSizes[TEXCACHE_MAX_MIPS];
	unsigned total = 0;
	byte *blob = NULL;											// the mip payload inside entry
	byte *entry = R_TexCacheDxt_ReadPacked( key, mipmap, allowPicmip, &hdr, mipSizes, &total, &blob );	// owns the bytes the upload reads
	if ( !entry )
	{
		// loose files: the sharded path, then the flat path
		char path[256];
		R_TexCacheDxt_Path( key, path, sizeof(path) );
		SceUID fd = sceIoOpen( path, SCE_O_RDONLY, 0 );
		if ( fd < 0 ) {
			R_TexCacheDxt_PathFlat( key, path, sizeof(path) );
			fd = sceIoOpen( path, SCE_O_RDONLY, 0 );
		}
		if ( fd < 0 ) {
			return NULL;
		}
		SceIoStat st;
		if ( sceIoGetstatByFd( fd, &st ) < 0
			|| st.st_size < (SceOff)( sizeof(hdr) + sizeof(unsigned) ) || st.st_size > (SceOff)TEXCACHE_PACK_MAX_ENTRY )
		{
			sceIoClose( fd );
			return NULL;
		}
		const unsigned size = (unsigned)st.st_size;
		entry = (byte *)Z_Malloc( size, TAG_TEMP_WORKSPACE, qfalse );
		if ( !entry || sceIoRead( fd, entry, size ) != (int)size )
		{
			if ( entry ) Z_Free( entry );
			sceIoClose( fd );
			return NULL;
		}
		sceIoClose( fd );
		blob = R_TexCacheDxt_ParseEntry( entry, size, mipmap, allowPicmip, &hdr, mipSizes, &total );
		if ( !blob )
		{
			Z_Free( entry );
			return NULL;
		}
	}

	const GLenum glFmt = ( hdr.format == TEXCACHE_FMT_DXT5 )
		? GL_COMPRESSED_RGBA_S3TC_DXT5_EXT : GL_COMPRESSED_RGB_S3TC_DXT1_EXT;

	image_t *image = (image_t *)Z_Malloc( sizeof( image_t ), TAG_IMAGE_T, qtrue );
	image->texnum = 1024 + giTextureBindNum++;
	image->iLastLevelUsedOn = RE_RegisterMedia_GetLevel();
	image->mipmap = !!mipmap;
	image->allowPicmip = !!allowPicmip;
	image->width = (int)hdr.width;
	image->height = (int)hdr.height;
	image->wrapClampMode = glWrapClampMode;
	image->internalFormat = (int)glFmt;
	Q_strncpyz( image->imgName, name, sizeof( image->imgName ) );

	if ( qglActiveTextureARB ) GL_SelectTexture( 0 );
	GL_Bind( image );

	qglGetError();	// flush any stale GL error so the check after the uploads is ours
	{
		int w = (int)hdr.width, h = (int)hdr.height, ofs = 0;
		for ( unsigned i = 0; i < hdr.mipCount; ++i )
		{
			qglCompressedTexImage2D( GL_TEXTURE_2D, (int)i, glFmt, w, h, 0, (int)mipSizes[i], blob + ofs );
			ofs += (int)mipSizes[i];
			w >>= 1; if ( w < 1 ) w = 1;
			h >>= 1; if ( h < 1 ) h = 1;
		}
	}
#ifdef USE_GXM_NATIVE
	// the cached blob is already UBC, so it goes over whole rather than per level
	const int uploaded = GXM_TexUploadDxt( image->texnum, blob, hdr.totalSize, hdr.width, hdr.height,
		hdr.mipCount, hdr.format == TEXCACHE_FMT_DXT5 );
#else
	const int uploaded = 1;
#endif
	Z_Free( entry );

	if ( !uploaded || qglGetError() != GL_NO_ERROR )
	{
		// upload rejected the cached blob: drop this image, let the normal load path rebuild it
		GLuint tn = (GLuint)image->texnum;
		qglDeleteTextures( 1, &tn );
#ifdef USE_GXM_NATIVE
		GXM_TexFree( image->texnum );
#endif
		qglBindTexture( GL_TEXTURE_2D, 0 );
		glState.currenttextures[glState.currenttmu] = 0;
		Z_Free( image );
		s_texCacheKeep = qtrue;
		return NULL;
	}

	if ( mipmap )
	{
		qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, gl_filter_min );
		qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, gl_filter_max );
		if ( r_ext_texture_filter_anisotropic->integer > 1 && glConfig.maxTextureFilterAnisotropy > 0 )
			qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, r_ext_texture_filter_anisotropic->value );
	}
	else
	{
		qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
		qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	}
	qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, glWrapClampMode );
	qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, glWrapClampMode );
#ifdef USE_GXM_NATIVE
	// GL_REPEAT is 0x2901; anything else here is one of the clamp modes
	GXM_TexFilter( image->texnum, 1, glWrapClampMode != GL_REPEAT );
#endif

	qglBindTexture( GL_TEXTURE_2D, 0 );
	glState.currenttextures[glState.currenttmu] = 0;

	const char *psNewName = GenerateImageMappingName( name );
	Q_strncpyz( image->imgName, psNewName, sizeof( image->imgName ) );
	AllocatedImages[ image->imgName ] = image;
	return image;
}

// Appends one 'JKTE' record to pack.delta: the key, then the entry bytes a loose file or pack blob holds.
static void R_TexCacheStoreDxt( const char *name, const texCacheHdrDxt_t *hdr,
								const unsigned *mipSizes, const byte *blob )
{
	if ( !r_texCacheCompressed || !r_texCacheCompressed->integer || !hdr || !mipSizes || !blob ) return;
	char key[MAX_QPATH];
	R_TexCacheDxt_Key( name, key );
	const unsigned keyLen = (unsigned)strlen( key );
	if ( !keyLen ) return;
	const unsigned hdrLen = (unsigned)sizeof(*hdr), mipLen = hdr->mipCount * (unsigned)sizeof(unsigned);
	const unsigned entrySize = hdrLen + mipLen + hdr->totalSize;
	const unsigned recSize = TEXCACHE_DELTA_HDR_SIZE + keyLen + entrySize;
	// one buffer, one write: the file gains a whole record or a tail the reader rejects
	byte *rec = (byte *)malloc( recSize );
	if ( !rec ) return;
	byte *p = rec + R_TexCacheDxt_DeltaHdrBuild( rec, key, keyLen, entrySize );
	memcpy( p, hdr, hdrLen );
	memcpy( p + hdrLen, mipSizes, mipLen );
	memcpy( p + hdrLen + mipLen, blob, hdr->totalSize );
	R_TexCacheDxt_DeltaLock();
	R_TexCacheDxt_DeltaOpenWrite();
	if ( s_texDeltaWrFd >= 0 )
	{
		if ( sceIoWrite( s_texDeltaWrFd, rec, recSize ) == (int)recSize )
		{
			s_texDeltaWrEnd += recSize;
		}
		else if ( !R_TexCacheDxt_DeltaTruncate( s_texDeltaWrFd, s_texDeltaWrEnd ) )
		{
			// the short write stays as the tail, so nothing else may land behind it this session
			sceIoClose( s_texDeltaWrFd );
			s_texDeltaWrFd = -1;
		}
	}
	R_TexCacheDxt_DeltaUnlock();
	free( rec );
}
#endif // VITA

/*
===============
R_FindImageFile

Finds or loads the given image.
Returns NULL if it fails, not a default image.
==============
*/
image_t	*R_FindImageFile( const char *name, qboolean mipmap, qboolean allowPicmip, qboolean allowTC, int glWrapClampMode ) {
	image_t	*image;
	int		width, height;
	byte	*pic;

	if (!name || ri.Cvar_VariableIntegerValue( "dedicated" ) )	// stop ghoul2 horribleness as regards image loading from server
	{
		return NULL;
	}

	// need to do this here as well as in R_CreateImage, or R_FindImageFile_NoLoad() may complain about
	//	different clamp parms used...
	//
	if(glConfig.clampToEdgeAvailable && glWrapClampMode == GL_CLAMP) {
		glWrapClampMode = GL_CLAMP_TO_EDGE;
	}

	image = R_FindImageFile_NoLoad(name, mipmap, allowPicmip, allowTC, glWrapClampMode );
	if (image) {
		return image;
	}

	if ( s_imageMisses.find( name ) != s_imageMisses.end() ) {
		return NULL;
	}

#ifdef VITA
	// DXT cache hit: build straight from the cached mip chain, no decode/encode/picmip
	s_texCacheKeep = qfalse;
	if ( r_texCacheCompressed && r_texCacheCompressed->integer && allowTC && name[0] != '$' && name[0] != '*' ) {
		image = R_CreateImageFromDxtCache( name, mipmap, allowPicmip, allowTC, glWrapClampMode );
		if ( image ) {
			return image;
		}
	}
#endif

	//
	// load the pic from disk
	//
	R_LoadImage( name, &pic, &width, &height );
	if ( pic == NULL ) {                                    // if we dont get a successful load
		s_imageMisses[name] = 1;                            // every loader extension was tried
		return NULL;                                        // bail
	}


	// refuse to find any files not power of 2 dims...
	//
	if ( (width&(width-1)) || (height&(height-1)) )
	{
		ri.Printf( PRINT_ALL, "Refusing to load non-power-2-dims(%d,%d) pic \"%s\"...\n", width,height,name );
		return NULL;
	}

	image = R_CreateImage( ( char * ) name, pic, width, height, GL_RGBA, mipmap, allowPicmip, allowTC, glWrapClampMode );
#ifdef VITA
	if ( !s_bakeTookPic )	// otherwise a worker still reads it; main frees it at retire
#endif
	Z_Free( pic );
	return image;
}


/*
================
R_CreateDlightImage
================
*/
#define	DLIGHT_SIZE	16
static void R_CreateDlightImage( void )
{
	int		width, height;
	byte	*pic;

	R_LoadImage("gfx/2d/dlight", &pic, &width, &height);
	if (pic)
	{
		tr.dlightImage = R_CreateImage("*dlight", pic, width, height, GL_RGBA, qfalse, qfalse, qfalse, GL_CLAMP );
		Z_Free(pic);
	}
	else
	{	// if we dont get a successful load
		int		x,y;
		byte	data[DLIGHT_SIZE][DLIGHT_SIZE][4];
		int		b;

		// make a centered inverse-square falloff blob for dynamic lighting
		for (x=0 ; x<DLIGHT_SIZE ; x++) {
			for (y=0 ; y<DLIGHT_SIZE ; y++) {
				float	d;

				d = ( DLIGHT_SIZE/2 - 0.5f - x ) * ( DLIGHT_SIZE/2 - 0.5f - x ) +
					( DLIGHT_SIZE/2 - 0.5f - y ) * ( DLIGHT_SIZE/2 - 0.5f - y );
				b = 4000 / d;
				if (b > 255) {
					b = 255;
				} else if ( b < 75 ) {
					b = 0;
				}
				data[y][x][0] =
					data[y][x][1] =
					data[y][x][2] = b;
				data[y][x][3] = 255;
			}
		}
		tr.dlightImage = R_CreateImage("*dlight", (byte *)data, DLIGHT_SIZE, DLIGHT_SIZE, GL_RGBA, qfalse, qfalse, qfalse, GL_CLAMP );
	}
}


/*
=================
R_InitFogTable
=================
*/
void R_InitFogTable( void ) {
	int		i;
	float	d;
	float	exp;

	exp = 0.5;

	for ( i = 0 ; i < FOG_TABLE_SIZE ; i++ ) {
		d = pow ( (float)i/(FOG_TABLE_SIZE-1), exp );

		tr.fogTable[i] = d;
	}
}

/*
================
R_FogFactor

Returns a 0.0 to 1.0 fog density value
This is called for each texel of the fog texture on startup
and for each vertex of transparent shaders in fog dynamically
================
*/
float	R_FogFactor( float s, float t ) {
	float	d;

	s -= 1.0/512;
	if ( s < 0 ) {
		return 0;
	}
	if ( t < 1.0/32 ) {
		return 0;
	}
	if ( t < 31.0/32 ) {
		s *= (t - 1.0f/32.0f) / (30.0f/32.0f);
	}

	// we need to leave a lot of clamp range
	s *= 8;

	if ( s > 1.0 ) {
		s = 1.0;
	}

	d = tr.fogTable[ (int)(s * (FOG_TABLE_SIZE-1)) ];

	return d;
}

/*
================
R_CreateFogImage
================
*/
#define	FOG_S	256
#define	FOG_T	32
static void R_CreateFogImage( void ) {
	int		x,y;
	byte	*data;
	float	d;
	float	borderColor[4];

	data = (unsigned char *)Hunk_AllocateTempMemory( FOG_S * FOG_T * 4 );

	// S is distance, T is depth
	for (x=0 ; x<FOG_S ; x++) {
		for (y=0 ; y<FOG_T ; y++) {
			d = R_FogFactor( ( x + 0.5f ) / FOG_S, ( y + 0.5f ) / FOG_T );

			data[(y*FOG_S+x)*4+0] =
			data[(y*FOG_S+x)*4+1] =
			data[(y*FOG_S+x)*4+2] = 255;
			data[(y*FOG_S+x)*4+3] = 255*d;
		}
	}
	// standard openGL clamping doesn't really do what we want -- it includes
	// the border color at the edges.  OpenGL 1.2 has clamp-to-edge, which does
	// what we want.
	tr.fogImage = R_CreateImage("*fog", (byte *)data, FOG_S, FOG_T, GL_RGBA, qfalse, qfalse, qfalse, GL_CLAMP );
	Hunk_FreeTempMemory( data );

	borderColor[0] = 1.0;
	borderColor[1] = 1.0;
	borderColor[2] = 1.0;
	borderColor[3] = 1;

	qglTexParameterfv( GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor );
}

/*
==================
R_CreateDefaultImage
==================
*/
#define	DEFAULT_SIZE	16
static void R_CreateDefaultImage( void ) {
	int		x;
	byte	data[DEFAULT_SIZE][DEFAULT_SIZE][4];

	// the default image will be a box, to allow you to see the mapping coordinates
	memset( data, 32, sizeof( data ) );
	for ( x = 0 ; x < DEFAULT_SIZE ; x++ ) {
		data[0][x][0] =
		data[0][x][1] =
		data[0][x][2] =
		data[0][x][3] = 255;

		data[x][0][0] =
		data[x][0][1] =
		data[x][0][2] =
		data[x][0][3] = 255;

		data[DEFAULT_SIZE-1][x][0] =
		data[DEFAULT_SIZE-1][x][1] =
		data[DEFAULT_SIZE-1][x][2] =
		data[DEFAULT_SIZE-1][x][3] = 255;

		data[x][DEFAULT_SIZE-1][0] =
		data[x][DEFAULT_SIZE-1][1] =
		data[x][DEFAULT_SIZE-1][2] =
		data[x][DEFAULT_SIZE-1][3] = 255;
	}
	tr.defaultImage = R_CreateImage("*default", (byte *)data, DEFAULT_SIZE, DEFAULT_SIZE, GL_RGBA, qtrue, qfalse, qfalse, GL_REPEAT );
}

/*
==================
R_CreateBuiltinImages
==================
*/
void R_CreateBuiltinImages( void ) {
	int		x,y;
	byte	data[DEFAULT_SIZE][DEFAULT_SIZE][4];

	R_CreateDefaultImage();

	// we use a solid white image instead of disabling texturing
	memset( data, 255, sizeof( data ) );
	tr.whiteImage = R_CreateImage("*white", (byte *)data, 8, 8, GL_RGBA, qfalse, qfalse, qfalse, GL_REPEAT);

	tr.screenImage = R_CreateImage("*screen", (byte *)data, 8, 8, GL_RGBA, qfalse, qfalse, qfalse, GL_REPEAT );

	// Create the scene glow image. - AReis
	tr.screenGlow = 1024 + giTextureBindNum++;
	qglDisable( GL_TEXTURE_2D );
	qglEnable( GL_TEXTURE_RECTANGLE_ARB );
	qglBindTexture( GL_TEXTURE_RECTANGLE_ARB, tr.screenGlow );
	qglTexImage2D( GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA16, glConfig.vidWidth, glConfig.vidHeight, 0, GL_RGB, GL_FLOAT, 0 );
	qglTexParameteri( GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	qglTexParameteri( GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	qglTexParameteri( GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_WRAP_S, GL_CLAMP );
	qglTexParameteri( GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_WRAP_T, GL_CLAMP );

	// Create the scene image. - AReis
	tr.sceneImage = 1024 + giTextureBindNum++;
	qglBindTexture( GL_TEXTURE_RECTANGLE_ARB, tr.sceneImage );
	qglTexImage2D( GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA16, glConfig.vidWidth, glConfig.vidHeight, 0, GL_RGB, GL_FLOAT, 0 );
	qglTexParameteri( GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	qglTexParameteri( GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	qglTexParameteri( GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_WRAP_S, GL_CLAMP );
	qglTexParameteri( GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_WRAP_T, GL_CLAMP );

	// Create the minimized scene blur image.
	if ( r_DynamicGlowWidth->integer > glConfig.vidWidth  )
	{
		r_DynamicGlowWidth->integer = glConfig.vidWidth;
	}
	if ( r_DynamicGlowHeight->integer > glConfig.vidHeight  )
	{
		r_DynamicGlowHeight->integer = glConfig.vidHeight;
	}
	tr.blurImage = 1024 + giTextureBindNum++;
	qglBindTexture( GL_TEXTURE_RECTANGLE_ARB, tr.blurImage );
	qglTexImage2D( GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA16, r_DynamicGlowWidth->integer, r_DynamicGlowHeight->integer, 0, GL_RGB, GL_FLOAT, 0 );
	qglTexParameteri( GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	qglTexParameteri( GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	qglTexParameteri( GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_WRAP_S, GL_CLAMP );
	qglTexParameteri( GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_WRAP_T, GL_CLAMP );
	qglDisable( GL_TEXTURE_RECTANGLE_ARB );

	if ( glConfigExt.doGammaCorrectionWithShaders )
	{
		qglEnable( GL_TEXTURE_3D );
		tr.gammaCorrectLUTImage = 1024 + giTextureBindNum++;
		qglBindTexture(GL_TEXTURE_3D, tr.gammaCorrectLUTImage);
		qglTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, 64, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		qglTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		qglTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		qglTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		qglTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
		qglTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		qglDisable(GL_TEXTURE_3D);
	}

	qglEnable(GL_TEXTURE_2D);

	// with overbright bits active, we need an image which is some fraction of full color,
	// for default lightmaps, etc
	for (x=0 ; x<DEFAULT_SIZE ; x++) {
		for (y=0 ; y<DEFAULT_SIZE ; y++) {
			data[y][x][0] =
			data[y][x][1] =
			data[y][x][2] = tr.identityLightByte;
			data[y][x][3] = 255;
		}
	}

	tr.identityLightImage = R_CreateImage("*identityLight", (byte *)data, 8, 8, GL_RGBA, qfalse, qfalse, qfalse, GL_REPEAT);

	for(x=0;x<NUM_SCRATCH_IMAGES;x++) {
		// scratchimage is usually used for cinematic drawing
		tr.scratchImage[x] = R_CreateImage(va("*scratch%d",x), (byte *)data, DEFAULT_SIZE, DEFAULT_SIZE, GL_RGBA, qfalse, qtrue, qfalse, GL_CLAMP);
	}

	R_CreateDlightImage();

	R_CreateFogImage();
}


/*
===============
R_SetColorMappings
===============
*/
void R_SetColorMappings( void ) {
	int		i, j;
	float	g;
	int		inf;
	int		shift;

	// setup the overbright lighting
	tr.overbrightBits = r_overBrightBits->integer;
	if ( !glConfig.deviceSupportsGamma && !glConfigExt.doGammaCorrectionWithShaders ) {
		tr.overbrightBits = 0;		// need hardware gamma for overbright
	}

	// never overbright in windowed mode
	if ( !glConfig.isFullscreen )
	{
		tr.overbrightBits = 0;
	}

	if ( tr.overbrightBits > 1 ) {
		tr.overbrightBits = 1;
	}

	if ( tr.overbrightBits < 0 ) {
		tr.overbrightBits = 0;
	}

	tr.identityLight = 1.0f / ( 1 << tr.overbrightBits );
	tr.identityLightByte = 255 * tr.identityLight;


	if ( r_intensity->value < 1.0f ) {
		ri.Cvar_Set( "r_intensity", "1" );
	}

	if ( r_gamma->value < 0.5f ) {
		ri.Cvar_Set( "r_gamma", "0.5" );
	} else if ( r_gamma->value > 3.0f ) {
		ri.Cvar_Set( "r_gamma", "3.0" );
	}

	g = r_gamma->value;

	shift = tr.overbrightBits;

	if ( !glConfigExt.doGammaCorrectionWithShaders )
	{
		for ( i = 0; i < 256; i++ ) {
			if ( g == 1 ) {
				inf = i;
			} else {
				inf = 255 * pow ( i/255.0f, 1.0f / g ) + 0.5f;
			}
			inf <<= shift;
			if (inf < 0) {
				inf = 0;
			}
			if (inf > 255) {
				inf = 255;
			}
			s_gammatable[i] = inf;
		}

		if ( glConfig.deviceSupportsGamma )
		{
			ri.WIN_SetGamma( &glConfig, s_gammatable, s_gammatable, s_gammatable );
		}
	}

	for (i=0 ; i<256 ; i++) {
		j = i * r_intensity->value;
		if (j > 255) {
			j = 255;
		}
		s_intensitytable[i] = j;
	}
}

void R_SetGammaCorrectionLUT()
{
	if ( glConfigExt.doGammaCorrectionWithShaders )
	{
		int inf;
		int shift = tr.overbrightBits;
		float g = r_gamma->value;
		byte gammaCorrected[64];

		for ( int i = 0; i < 64; i++ )
		{
			if ( g == 1.0f )
			{
				inf = (int)(((float)i / 63.0f) * 255.0f + 0.5f);
			}
			else
			{
				inf = (int)(255.0f * pow((float)i / 63.0f, 1.0f / g) + 0.5f);
			}

			gammaCorrected[i] = Com_Clampi(0, 255, inf << shift);
		}

		byte *lutTable = (byte *)ri.Hunk_AllocateTempMemory(64 * 64 * 64 * 3);
		byte *write = lutTable;
		for ( int z = 0; z < 64; z++ )
		{
			for ( int y = 0; y < 64; y++ )
			{
				for ( int x = 0; x < 64; x++ )
				{
					*write++ = gammaCorrected[x];
					*write++ = gammaCorrected[y];
					*write++ = gammaCorrected[z];
				}
			}
		}

		qglBindTexture(GL_TEXTURE_3D, tr.gammaCorrectLUTImage);
		qglPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		qglTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, 64, 64, 64, GL_RGB, GL_UNSIGNED_BYTE, lutTable);

		ri.Hunk_FreeTempMemory(lutTable);
	}
}

/*
===============
R_InitImages
===============
*/
void	R_InitImages( void ) {
	//memset(hashTable, 0, sizeof(hashTable));	// DO NOT DO THIS NOW (because of image cacheing)	-ste.
	// build brightness translation tables
	R_SetColorMappings();

	// create default texture and white texture
	R_CreateBuiltinImages();

	R_SetGammaCorrectionLUT();
}

/*
===============
R_DeleteTextures
===============
*/
// (only gets called during vid_restart now (and app exit), not during map load)
//
void R_DeleteTextures( void ) {

	R_Images_Clear();
	GL_ResetBinds();
}

