#if defined(ANDROID)

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <algorithm>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <tier0/platform.h>
#include <vgui/ISurface.h>
#include <vgui_controls/Controls.h>

#include "vgui2_surfacelib/Win32Font.h"

static stbtt_fontinfo s_FontInfo;
static const unsigned char* s_FontData = nullptr;
static size_t s_FontDataSize = 0;
static float s_fontScale = 1.0f;

static unsigned char* LoadFontFile( const char* filename, size_t& size )
{
	FILE* f = fopen( filename, "rb" );
	if( !f ) return nullptr;

	fseek( f, 0, SEEK_END );
	size = ftell( f );
	fseek( f, 0, SEEK_SET );

	auto* data = (unsigned char*)malloc( size );
	if( !data )
	{
		fclose( f );
		return nullptr;
	}

	fread( data, 1, size, f );
	fclose( f );
	return data;
}

static void FreeFontData()
{
	if( s_FontData )
	{
		free( (void*)s_FontData );
		s_FontData = nullptr;
	}
	s_FontDataSize = 0;
}

CWin32Font::CWin32Font()
{
	m_szName[0] = 0;
	m_iTall = 0;
	m_iWeight = 0;
	m_iHeight = 0;
	m_iAscent = 0;
	m_iFlags = 0;
	m_iMaxCharWidth = 0;
	m_bUnderlined = false;
	m_iBlur = 0;
	m_iScanLines = 0;

	for( int i = 0; i < ABCWIDTHS_CACHE_SIZE; i++ )
	{
		m_ABCWidthsCache[i].a = 0;
		m_ABCWidthsCache[i].b = 0;
		m_ABCWidthsCache[i].c = 0;
	}
}

CWin32Font::~CWin32Font()
{
}

bool CWin32Font::Create( const char* windowsFontName, int tall, int weight, int blur, int scanlines, int flags )
{
	FreeFontData();

	strncpy( m_szName, windowsFontName, sizeof( m_szName ) - 1 );
	m_szName[sizeof(m_szName) - 1] = 0;
	m_iTall = tall;
	m_iWeight = weight;
	m_iBlur = blur;
	m_iScanLines = scanlines;
	m_iFlags = flags;

	// Try to find the font file
	char fontPath[4096];
	const char* searchPaths[] = {
		"resource/",
		"../resource/",
		"",
	};

	for( auto path : searchPaths )
	{
		snprintf( fontPath, sizeof( fontPath ), "%s%s.ttf", path, windowsFontName );
		s_FontData = LoadFontFile( fontPath, s_FontDataSize );
		if( s_FontData ) break;

		snprintf( fontPath, sizeof( fontPath ), "%s%s.ttf", path, windowsFontName );
		s_FontData = LoadFontFile( fontPath, s_FontDataSize );
		if( s_FontData ) break;
	}

	if( !s_FontData )
	{
		// Try loading marlett.ttf as fallback
		snprintf( fontPath, sizeof( fontPath ), "resource/marlett.ttf" );
		s_FontData = LoadFontFile( fontPath, s_FontDataSize );
	}

	if( !s_FontData )
	{
		m_szName[0] = 0;
		return false;
	}

	int offset = stbtt_GetFontOffsetForIndex( s_FontData, 0 );
	if( offset < 0 )
	{
		FreeFontData();
		m_szName[0] = 0;
		return false;
	}

	if( !stbtt_InitFont( &s_FontInfo, s_FontData, offset ) )
	{
		FreeFontData();
		m_szName[0] = 0;
		return false;
	}

	s_fontScale = stbtt_ScaleForPixelHeight( &s_FontInfo, (float)tall );

	int ascent, descent, lineGap;
	stbtt_GetFontVMetrics( &s_FontInfo, &ascent, &descent, &lineGap );
	m_iAscent = (int)( ascent * s_fontScale );
	m_iHeight = (int)( ( ascent - descent + lineGap ) * s_fontScale );

	// Estimate max char width
	int minx, maxx, miny, maxy, advance;
	stbtt_GetCodepointBox( &s_FontInfo, 'W', &minx, &miny, &maxx, &maxy );
	stbtt_GetCodepointHMetrics( &s_FontInfo, 'W', &advance, nullptr );
	m_iMaxCharWidth = (int)( advance * s_fontScale );

	return true;
}

void CWin32Font::GetCharABCWidths( int ch, int& a, int& b, int& c )
{
	if( ch >= 0 && ch < ABCWIDTHS_CACHE_SIZE )
	{
		a = m_ABCWidthsCache[ch].a;
		b = m_ABCWidthsCache[ch].b;
		c = m_ABCWidthsCache[ch].c;
		if( b != 0 || ch == 0 )
			return;
	}

	if( !s_FontData )
	{
		a = c = 0;
		b = m_iMaxCharWidth;
		return;
	}

	int advance, leftSideBearing;
	stbtt_GetCodepointHMetrics( &s_FontInfo, ch, &advance, &leftSideBearing );

	a = (int)( leftSideBearing * s_fontScale );
	b = (int)( advance * s_fontScale );
	c = 0; // stb_truetype doesn't provide right side bearing directly

	if( ch >= 0 && ch < ABCWIDTHS_CACHE_SIZE )
	{
		m_ABCWidthsCache[ch].a = a;
		m_ABCWidthsCache[ch].b = b;
		m_ABCWidthsCache[ch].c = c;
	}
}

void CWin32Font::GetCharRGBA( int ch, int rgbaX, int rgbaY, int rgbaWide, int rgbaTall, unsigned char* rgba )
{
	if( !s_FontData || rgbaWide <= 0 || rgbaTall <= 0 )
		return;

	int width, height, xOffset, yOffset;
	unsigned char* monoBitmap = stbtt_GetCodepointBitmap(
		&s_FontInfo, s_fontScale, s_fontScale,
		ch, &width, &height, &xOffset, &yOffset
	);

	if( !monoBitmap )
		return;

	// Copy the glyph bitmap into the RGBA buffer
	for( int y = 0; y < height && y < rgbaTall; y++ )
	{
		for( int x = 0; x < width && x < rgbaWide; x++ )
		{
			int srcIdx = y * width + x;
			int dstIdx = ( ( y + yOffset - rgbaY ) * rgbaWide + ( x + xOffset - rgbaX ) ) * 4;

			if( dstIdx >= 0 && dstIdx + 3 < rgbaTall * rgbaWide * 4 )
			{
				unsigned char alpha = monoBitmap[srcIdx];
				rgba[dstIdx + 0] = 255;
				rgba[dstIdx + 1] = 255;
				rgba[dstIdx + 2] = 255;
				rgba[dstIdx + 3] = alpha;
			}
		}
	}

	stbtt_FreeBitmap( monoBitmap, nullptr );
}

bool CWin32Font::IsEqualTo( const char* windowsFontName, int tall, int weight, int blur, int scanlines, int flags )
{
	return strcmp( m_szName, windowsFontName ) == 0 &&
		m_iTall == tall &&
		m_iWeight == weight &&
		m_iBlur == blur &&
		m_iScanLines == scanlines &&
		m_iFlags == flags;
}

#endif // ANDROID
