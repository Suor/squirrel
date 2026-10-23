/*  see copyright notice in squirrel.h */
#ifndef _SQTABLE_H_
#define _SQTABLE_H_
/*
* The following code is based on Lua 4.0 (Copyright 1994-2002 Tecgraf, PUC-Rio.)
* http://www.lua.org/copyright.html#4
* http://www.lua.org/source/4.0.1/src_ltable.c.html
*/

#include "sqstring.h"
#include "sqcensus.h"


#define hashptr(p)  ((SQHash)(((SQInteger)p) >> 3))

inline SQHash HashObj(const SQObject &key)
{
    switch(sq_type(key)) {
        case OT_STRING:     return _hashtables(_string(key)->_hash);
        case OT_FLOAT:      return (SQHash)((SQInteger)_float(key));
        case OT_BOOL: case OT_INTEGER:  return (SQHash)((SQInteger)_integer(key));
        default:            return hashptr(key._unVal.pRefCounted);
    }
}

struct SQTable;
struct SQArray;

// A table's contents frozen for the lazy tables that read through it (see SQTable::_ext):
// the table itself until a write to it would change what they see, then a verbatim copy
// made at that write. Reference counted by the lazy tables.
struct SQTableVersion
{
    SQUnsignedInteger _refs;
    SQTable *_table;
};

// Slots a lazy table was given without holding them (sq_lazyinject): as if, after the
// first `_pos` keys of its `_added` and the first `_nmarks` of its clone marks, every key
// of `_keys` had been added in that order with its value in `_slots` bound to `_bind`.
// (`_pos` alone can't say whether slots given at a mark came before or after that clone.) `_slots` and `_keys` are shared by every table
// given the same slots; each value is bound when first read and kept, as the source's
// closures are.
struct SQTableInjected
{
    SQTable *_slots;
    SQArray *_keys;
    SQWeakRef *_bind;
    SQInteger _pos;
    SQUnsignedInteger _nmarks;
};

// What a lazy table reads through: the version of the table it was made from, the object
// its inherited closures are bound to (or none), the keys it gained that the source lacks,
// in the order they came, and the slots it was given (in the order given). A lazy clone
// stands for `clone` of its source; a lazy copy (`_copy`) for a new table its source's
// slots were added to one by one, in the source's order, weak references read through
// (as a deep copy's top level is made). A clone of an unbound lazy table is another one
// reading through the same source, holding what that one held: `_marks` are where in
// `_added` each such clone was made (the table it stands for was cloned there).
struct SQTableLazy
{
    SQTableVersion *_src;
    bool _copy;
    SQWeakRef *_bind;
    sqvector<SQObjectPtr> _added;
    sqvector<SQUnsignedInteger> _marks;
    sqvector<SQTableInjected> _injected;
    // The version lazy tables reading through this one read (see SQTable::_ext).
    SQTableVersion *_version;
};

struct SQTable : public SQDelegable
{
    friend struct SQCensus;
private:
    // A node's key, with the chain's next node packed in beside it: a 64-bit SQObject is a
    // 4-byte type and an 8-byte value, so a separate next pointer would pad the node from
    // 32 bytes to 40. `_next` is the next node's index plus one (0: the chain ends here).
    // Assigning or clearing the key leaves `_next` alone.
    struct _HashKey
    {
        SQObjectValue _unVal;
        SQObjectType _type;
        unsigned int _next;
        _HashKey() { _unVal.raw = (SQRawObjectVal)NULL; _type = OT_NULL; _next = 0; }
        ~_HashKey() { __Release(_type, _unVal); }
        SQObject Obj() const { SQObject o; o._type = _type; o._unVal = _unVal; return o; }
        void Set(const SQObject &o)
        {
            SQObjectType tOldType = _type;
            SQObjectValue unOldVal = _unVal;
            _unVal = o._unVal;
            _type = o._type;
            __AddRef(_type, _unVal);
            __Release(tOldType, unOldVal);
        }
        void Null()
        {
            SQObjectType tOldType = _type;
            SQObjectValue unOldVal = _unVal;
            _type = OT_NULL;
            _unVal.raw = (SQRawObjectVal)NULL;
            __Release(tOldType, unOldVal);
        }
    private:
        _HashKey(const _HashKey &);
        _HashKey &operator=(const _HashKey &);
    };
    struct _HashNode
    {
        SQObjectPtr val;
        _HashKey key;
    };
    // The node after `n` in its chain, in the node array `base` (NULL at the chain's end).
    static inline _HashNode *_NextOf(_HashNode *base, const _HashNode *n) { return n->key._next ? base + (n->key._next - 1) : NULL; }
    static inline void _SetNext(_HashNode *base, _HashNode *n, const _HashNode *next) { n->key._next = next ? (unsigned int)(next - base) + 1 : 0; }
    _HashNode *_firstfree;
    _HashNode *_nodes;
    // 32 bits each (a table is 88 bytes, not 104): at most 2^30 nodes, 32 GB of them.
    SQInt32 _numofnodes;
    SQInt32 _usednodes;
    // What a lazy table reads through (Lazy()), and the version lazy tables reading
    // through this one read (Version()), in one word, since a table seldom has both:
    // 0, a SQTableLazy (holding the version, if any), or a SQTableVersion with bit 0 set.
    //
    // A lazy table behaves exactly as a clone of its source whose closures were bound
    // (bindenv) to Lazy()->_bind at the clone, but holds only what was read or written:
    // its own nodes keep the slots written and the bound closures handed out (so a
    // closure is the same object on every read). Anything that needs the whole table
    // (iteration, delete, clear) first makes it a plain table, laid out as that clone
    // would be; a clone of one whose values aren't bound when read is lazy too.
    //
    // A version is set while lazy tables read through this one's contents as they are now.
    //
    // Bits 1 and 2 are the lookup cache's flags (see Watched, Started), kept through both.
    SQUnsignedInteger _ext;
    static const SQUnsignedInteger _WATCHED = 2;
    static const SQUnsignedInteger _STARTED = 4;
    static const SQUnsignedInteger _FLAGS = _WATCHED | _STARTED;
    SQTableLazy *Lazy() const { return (_ext & 1) ? NULL : (SQTableLazy *)(_ext & ~_FLAGS); }
    SQTableVersion *Version() const
    {
        if (_ext & 1) return (SQTableVersion *)(_ext & ~(_FLAGS | 1));
        SQTableLazy *lz = Lazy();
        return lz ? lz->_version : NULL;
    }
    void SetLazy(SQTableLazy *lz)
    {
        SQTableVersion *v = Version();
        SQUnsignedInteger w = _ext & _FLAGS;
        if (lz) { lz->_version = v; _ext = (SQUnsignedInteger)lz | w; }
        else _ext = (v ? (SQUnsignedInteger)v | 1 : 0) | w;
    }
    void SetVersion(SQTableVersion *v)
    {
        if (SQTableLazy *lz = Lazy()) lz->_version = v;
        else _ext = (v ? (SQUnsignedInteger)v | 1 : 0) | (_ext & _FLAGS);
    }

///////////////////////////
    void AllocNodes(SQInteger nSize);
    void Rehash(bool force);
    SQTable(SQSharedState *ss, SQInteger nInitialSize);
    void _ClearNodes();
    bool _NewSlot(const SQObject &key,const SQObjectPtr &val);
    bool _LazyGet(const SQObjectPtr &key,SQObjectPtr &val);
    bool _LazyHas(const SQObjectPtr &key);
    void _FillFromSource(SQTableLazy *lz, _HashNode *own, SQInteger ownsize);
    void _AddFrom(SQTableLazy *lz, _HashNode *own, SQInteger ownsize, SQUnsignedInteger segment, SQUnsignedInteger from, SQUnsignedInteger to);
    void _AddInjected(SQTableLazy *lz, _HashNode *own, SQInteger ownsize, SQUnsignedInteger segment, SQUnsignedInteger pos);
    void _Reclone();
    static _HashNode *_OwnNode(_HashNode *own, SQInteger ownsize, const SQObject &key);
    SQTable *_CloneLazy();
    void _DetachVersion();
    void _ReleaseLazy();
    SQTable *_CopyVerbatim();
public:
    static SQTable* Create(SQSharedState *ss,SQInteger nInitialSize)
    {
        SQTable *newtable = (SQTable*)SQ_MALLOC(sizeof(SQTable));
        new (newtable) SQTable(ss, nInitialSize);
        newtable->_delegate = NULL;
        return newtable;
    }
    // A lazy table reading through `src` (see _ext), its closures not bound.
    static SQTable* CreateLazy(SQSharedState *ss, SQTable *src, bool copy = false);
    // Binds the closures a lazy table reads from its source to `env` from now on.
    void LazyBind(SQWeakRef *env);
    // Gives a lazy table the slots `keys` (in that order), their values read from `extra`
    // and bound to `env` when first read (see SQTableInjected). `extra` must hold exactly
    // those keys. False, and nothing done, if the table isn't lazy or has one of the keys.
    bool LazyInject(SQTable *extra, SQArray *keys, SQWeakRef *env);
    // Makes a lazy table a plain one, laid out as the clone it stands for would be.
    void Materialize();
    bool IsLazy() const { return Lazy() != NULL; }
    // Whether a cached lookup (SQSharedState::_lookupcache) may depend on this table: its
    // changes that could change a lookup's outcome then make the cache's entries stale.
    // Set when an entry is made, and kept.
    bool Watched() const { return (_ext & _WATCHED) != 0; }
    void Watch() { _ext |= _WATCHED; }
    // Whether a cached lookup started at this table (its entries are forgotten as it is
    // freed, so that a table made at its address can't be taken for it). Set with Watch.
    bool Started() const { return (_ext & _STARTED) != 0; }
    void Start() { _ext |= _STARTED | _WATCHED; }
    // SQDelegable's, telling the lookup cache when the table is watched (and as it is
    // freed, which sets no delegate).
    bool SetDelegate(SQTable *mt);
    // Whether the table has `key`, without reading it (no closure is bound).
    bool Has(const SQObjectPtr &key);
    void Finalize();
    SQTable *Clone();
    ~SQTable()
    {
        SQ_CENSUS_FORGET(this);
        _ReleaseLazy();
        if (SQTableVersion *v = Version()) v->_table = NULL;
        // Freed, nothing holds it: a lookup the cache keeps that passed it started at a
        // table holding it, freed before it (see SQSharedState::LookupForget).
        if (Started()) _sharedstate->LookupForget(this);
        SQDelegable::SetDelegate(NULL);
        REMOVE_FROM_CHAIN(&_sharedstate->_gc_chain, this);
        for (SQInteger i = 0; i < _numofnodes; i++) _nodes[i].~_HashNode();
        SQ_FREE(_nodes, _numofnodes * sizeof(_HashNode));
    }
#ifndef NO_GARBAGE_COLLECTOR
    void Mark(SQCollectable **chain);
    SQObjectType GetType() {return OT_TABLE;}
#endif
    inline _HashNode *_Get(const SQObject &key,SQHash hash)
    {
        _HashNode *n = &_nodes[hash];
        do{
            if(_rawval(n->key) == _rawval(key) && sq_type(n->key) == sq_type(key)){
                return n;
            }
        }while((n = _NextOf(_nodes, n)));
        return NULL;
    }
    //for compiler use
    inline bool GetStr(const SQChar* key,SQInteger keylen,SQObjectPtr &val)
    {
        SQHash hash = _hashstr(key,keylen);
        _HashNode *n = &_nodes[hash & (_numofnodes - 1)];
        _HashNode *res = NULL;
        do{
            if(sq_type(n->key) == OT_STRING && (scstrcmp(_stringval(n->key),key) == 0)){
                res = n;
                break;
            }
        }while((n = _NextOf(_nodes, n)));
        if (res) {
            val = _realval(res->val);
            return true;
        }
        return false;
    }
    // Inline: the VM's most frequent call (a lazy table's read through is not).
    inline bool Get(const SQObjectPtr &key,SQObjectPtr &val)
    {
        if(sq_type(key) == OT_NULL)
            return false;
        _HashNode *n = _Get(key, HashObj(key) & (_numofnodes - 1));
        if (n) {
            // `key` and `val` may be the same object (sq_rawget).
            SQ_CENSUS_HOOK(sq_census_get(this, key, n->val));
            val = _realval(n->val);
            return true;
        }
        if (Lazy()) return _LazyGet(SQObjectPtr(key), val);
        return false;
    }
    // Get's read of the table's own slots only: for a lazy table, the slots it was given or
    // has read (each as Get reads it), never its source's. False says nothing about them.
    inline bool GetOwn(const SQObjectPtr &key,SQObjectPtr &val)
    {
        if(sq_type(key) == OT_NULL)
            return false;
        _HashNode *n = _Get(key, HashObj(key) & (_numofnodes - 1));
        if (!n) return false;
        SQ_CENSUS_HOOK(sq_census_get(this, key, n->val));
        val = _realval(n->val);
        return true;
    }
    void Remove(const SQObjectPtr &key);
    bool Set(const SQObjectPtr &key, const SQObjectPtr &val);
    //returns true if a new slot has been created false if it was already present
    bool NewSlot(const SQObjectPtr &key,const SQObjectPtr &val);
    SQInteger Next(bool getweakrefs,const SQObjectPtr &refpos, SQObjectPtr &outkey, SQObjectPtr &outval);

    SQInteger CountUsed();
    void Clear();
    void Release()
    {
        sq_delete(this, SQTable);
    }

};

#endif //_SQTABLE_H_
