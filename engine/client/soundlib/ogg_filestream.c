/*
ogg_filestream.c - helper struct for working with Ogg files
Copyright (C) 2024 SNMetamorph

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "ogg_filestream.h"
#include "soundlib.h"
#include <string.h>

size_t OggFilestream_Read( void *ptr, size_t blockSize, size_t nmemb, void *datasource )
{
	ogg_filestream_t *filestream = (ogg_filestream_t *)datasource;
	if( !filestream || !ptr || !filestream->buffer || !blockSize || !nmemb
		|| filestream->position > filestream->filesize )
		return 0;

	size_t remain = filestream->filesize - filestream->position;
	size_t blocks = Q_min( nmemb, remain / blockSize );
	size_t dataSize = blockSize * blocks;

	memcpy( ptr, filestream->buffer + filestream->position, dataSize );
	filestream->position += dataSize;
	return blocks;
}

int OggFilestream_Seek( void *datasource, int64_t offset, int whence )
{
	size_t base;
	ogg_filestream_t *filestream = (ogg_filestream_t *)datasource;

	if( !filestream || filestream->position > filestream->filesize )
		return -1;

	if( whence == SEEK_SET )
		base = 0;
	else if( whence == SEEK_CUR )
		base = filestream->position;
	else if( whence == SEEK_END )
		base = filestream->filesize;
	else
		return -1;

	if( offset < 0 )
	{
		uint64_t distance = (uint64_t)( -( offset + 1 )) + 1;
		if( distance > base ) return -1;
		filestream->position = base - distance;
	}
	else
	{
		if((uint64_t)offset > filestream->filesize - base ) return -1;
		filestream->position = base + offset;
	}
	return 0;
}

long OggFilestream_Tell( void *datasource )
{
	ogg_filestream_t *filestream = (ogg_filestream_t *)datasource;
	if( !filestream || filestream->position > filestream->filesize || filestream->position > LONG_MAX )
		return -1;
	return filestream->position;
}

#if XASH_ENGINE_TESTS
#include "tests.h"

void Test_RunOggFilestream( void )
{
	const byte data[] = { 1, 2, 3, 4, 5 };
	byte output[8] = { 0 };
	ogg_filestream_t file;
	OggFilestream_Init( &file, "test", data, sizeof( data ));
	TASSERT_EQi( OggFilestream_Read( output, 0, 1, &file ), 0 );
	TASSERT_EQi( OggFilestream_Read( output, SIZE_MAX, 2, &file ), 0 );
	TASSERT_EQi( OggFilestream_Read( output, 2, SIZE_MAX, &file ), 2 );
	TASSERT_EQi( file.position, 4 );
	TASSERT_EQi( output[3], 4 );
	TASSERT_EQi( OggFilestream_Seek( &file, -2, SEEK_END ), 0 );
	TASSERT_EQi( OggFilestream_Tell( &file ), 3 );
	TASSERT_EQi( OggFilestream_Seek( &file, INT64_MIN, SEEK_CUR ), -1 );
	TASSERT_EQi( OggFilestream_Seek( &file, INT64_MAX, SEEK_CUR ), -1 );
	TASSERT_EQi( OggFilestream_Tell( &file ), 3 );
	TASSERT_EQi( OggFilestream_Seek( &file, 0, SEEK_END ), 0 );
	TASSERT_EQi( OggFilestream_Read( output, 1, 1, &file ), 0 );
	TASSERT_EQi( OggFilestream_Read( output, 1, 1, NULL ), 0 );
	file.position++;
	TASSERT_EQi( OggFilestream_Seek( &file, 0, SEEK_SET ), -1 );
	TASSERT_EQi( OggFilestream_Tell( &file ), -1 );
}
#endif
