/*  see copyright notice in squirrel.h */
#ifndef _SQSTRING_H_
#define _SQSTRING_H_

// The original engine's string hash (Squirrel 3.0 in a 32-bit build): it walks forward
// one character per step, so a string longer than 31 characters hashes only its first
// l / step characters, in 32 bits. Where a key lands in a table, and so the order a
// `foreach` visits a table's slots in, follows from it, and scripts depend on that order
// (Modern Hooks registers the DLCs by iterating ::Const.DLC).
inline SQHash _hashstr (const SQChar *s, size_t l)
{
	unsigned int h = (unsigned int)l;  /* seed */
	size_t step = (l >> 5) | 1;  /* if string is too long, don't hash all its chars */
	for (; l >= step; l -= step)
		h = h ^ ((h << 5) + (h >> 2) + (unsigned short)*(s++));
	return (SQHash)h;
}

// `_hashstr` of the concatenation of `as` and `bs`.
inline SQHash _hashstr2(const SQChar* as, size_t al, const SQChar* bs, size_t bl)
{
	size_t l = al + bl;
	unsigned int h = (unsigned int)l;  /* seed */
	size_t step = (l >> 5) | 1;
	for (size_t i = 0; l >= step; l -= step, i++)
		h = h ^ ((h << 5) + (h >> 2) + (unsigned short)(i < al ? as[i] : bs[i - al]));
	return (SQHash)h;
}

struct SQString : public SQRefCounted
{
    SQString(){}
    ~SQString(){}
public:
    static SQString *Create(SQSharedState *ss, const SQChar *, SQInteger len = -1 );
    static SQString* Concat(SQSharedState* ss, const SQChar* a, SQInteger alen, const SQChar* b, SQInteger blen);
    SQInteger Next(const SQObjectPtr &refpos, SQObjectPtr &outkey, SQObjectPtr &outval);
    void Release();
    SQSharedState *_sharedstate;
    SQString *_next; //chain for the string table
    SQInteger _len;
    SQHash _hash;
    SQChar _val[1];
};



#endif //_SQSTRING_H_
