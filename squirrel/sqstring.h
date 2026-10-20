/*  see copyright notice in squirrel.h */
#ifndef _SQSTRING_H_
#define _SQSTRING_H_

// The original engine's string hash (Squirrel 3.0 in a 32-bit build): it walks forward
// one character per step, so a string of 64 characters or more hashes only its first
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

// The string table's own hash, for strings the original's hash doesn't read whole (64
// characters or more, of which it reads only the first 21 to 42 or so): FNV-1a over the
// first and the last SQ_HASHSTR_ENDS characters, so strings with a long common prefix
// (paths, generated names, serialized data) spread out, at a bounded cost for long ones.
// Shorter strings use `_hashstr`, which reads them whole.
#define SQ_HASHSTR_WHOLE 64
#define SQ_HASHSTR_ENDS 128
inline unsigned int _hashintern_step(unsigned int h, SQChar c) { return (h ^ (unsigned char)c) * 16777619u; }
inline unsigned int _hashintern(const SQChar *s, size_t l)
{
	unsigned int h = 2166136261u ^ (unsigned int)l;
	size_t head = l < SQ_HASHSTR_ENDS ? l : SQ_HASHSTR_ENDS;
	size_t tail = l > 2 * SQ_HASHSTR_ENDS ? l - SQ_HASHSTR_ENDS : head;
	for (size_t i = 0; i < head; i++) h = _hashintern_step(h, s[i]);
	for (size_t i = tail; i < l; i++) h = _hashintern_step(h, s[i]);
	return h;
}

// `_hashintern` of the concatenation of `as` and `bs`.
inline unsigned int _hashintern2(const SQChar *as, size_t al, const SQChar *bs, size_t bl)
{
	size_t l = al + bl;
	unsigned int h = 2166136261u ^ (unsigned int)l;
	size_t head = l < SQ_HASHSTR_ENDS ? l : SQ_HASHSTR_ENDS;
	size_t tail = l > 2 * SQ_HASHSTR_ENDS ? l - SQ_HASHSTR_ENDS : head;
	for (size_t i = 0; i < head; i++) h = _hashintern_step(h, i < al ? as[i] : bs[i - al]);
	for (size_t i = tail; i < l; i++) h = _hashintern_step(h, i < al ? as[i] : bs[i - al]);
	return h;
}

// A string's `_hash` from its `_hashstr` and its string-table hash (see SQString::_hash),
// and back. A 32-bit build has room only for `_hashstr`, used for both.
#ifdef _SQ64
inline SQHash _hashpack(SQHash tables, unsigned int interned) { return (SQHash)(unsigned int)tables | ((SQHash)interned << 32); }
inline SQHash _hashtables(SQHash h) { return (SQHash)(unsigned int)h; }
inline SQHash _hashinterned(SQHash h) { return h >> 32; }
#else
inline SQHash _hashpack(SQHash tables, unsigned int interned) { (void)interned; return tables; }
inline SQHash _hashtables(SQHash h) { return h; }
inline SQHash _hashinterned(SQHash h) { return h; }
#endif

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
    // The low 32 bits: `_hashstr`, which places the string in tables (HashObj). The high
    // 32 bits: where the string table keeps it (`_hashintern`, or `_hashstr` when that
    // reads it whole).
    SQHash _hash;
    SQChar _val[1];
};



#endif //_SQSTRING_H_
