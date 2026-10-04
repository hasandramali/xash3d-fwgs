/* Sven brush texture frame selection, shared by GL and software renderers. */
#ifndef REF_TEXTURE_ANIM_H
#define REF_TEXTURE_ANIM_H

static inline texture_t *R_TextureFrame( texture_t *base, float frame )
{
	texture_t *start = base;
	// Native Svengine advances while the integer step is less than frame.
	// Guard malformed network values and broken/non-circular texture chains.
	if( !(frame > 0.0f) || frame > 65535.0f )
		return base;
	int steps = (int)frame;
	if( frame > steps ) steps++;
	for( int count = 1; steps > 0 && count <= 2 * MOD_FRAMES; count++ )
	{
		if( !base->anim_next ) return base;
		base = base->anim_next;
		steps--;
		if( base == start ) steps %= count;
	}
	return base;
}

#endif
