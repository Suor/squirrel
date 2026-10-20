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
        case OT_STRING:     return _string(key)->_hash;
        case OT_FLOAT:      return (SQHash)((SQInteger)_float(key));
        case OT_BOOL: case OT_INTEGER:  return (SQHash)((SQInteger)_integer(key));
        default:            return hashptr(key._unVal.pRefCounted);
    }
}

struct SQTable;

// A table's contents frozen for the lazy tables that read through it (see SQTable::_lazy):
// the table itself until a write to it would change what they see, then a verbatim copy
// made at that write. Reference counted by the lazy tables.
struct SQTableVersion
{
    SQUnsignedInteger _refs;
    SQTable *_table;
};

// What a lazy table reads through: the version of the table it was made from, the object
// its inherited closures are bound to (or none), and the keys it gained that the source
// lacks, in the order they came.
struct SQTableLazy
{
    SQTableVersion *_src;
    SQWeakRef *_bind;
    sqvector<SQObjectPtr> _added;
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
    SQInteger _numofnodes;
    SQInteger _usednodes;
    // A lazy table behaves exactly as a clone of its source whose closures were bound
    // (bindenv) to _lazy->_bind at the clone, but holds only what was read or written:
    // its own nodes keep the slots written and the bound closures handed out (so a
    // closure is the same object on every read). Anything that needs the whole table
    // (iteration, clone, delete, clear) first makes it a plain table, laid out as that
    // clone would be.
    SQTableLazy *_lazy;
    // Set while lazy tables read through this one's contents as they are now.
    SQTableVersion *_version;

///////////////////////////
    void AllocNodes(SQInteger nSize);
    void Rehash(bool force);
    SQTable(SQSharedState *ss, SQInteger nInitialSize);
    void _ClearNodes();
    bool _NewSlot(const SQObject &key,const SQObjectPtr &val);
    bool _LazyGet(const SQObjectPtr &key,SQObjectPtr &val);
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
    // A lazy table reading through `src` (see _lazy), its closures not bound.
    static SQTable* CreateLazy(SQSharedState *ss, SQTable *src);
    // Binds the closures a lazy table reads from its source to `env` from now on.
    void LazyBind(SQWeakRef *env);
    // Makes a lazy table a plain one, laid out as the clone it stands for would be.
    void Materialize();
    bool IsLazy() const { return _lazy != NULL; }
    // Whether the table has `key`, without reading it (no closure is bound).
    bool Has(const SQObjectPtr &key);
    void Finalize();
    SQTable *Clone();
    ~SQTable()
    {
        sq_census_forget(this);
        _ReleaseLazy();
        if (_version) _version->_table = NULL;
        SetDelegate(NULL);
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
    bool Get(const SQObjectPtr &key,SQObjectPtr &val);
    void Remove(const SQObjectPtr &key);
    bool Set(const SQObjectPtr &key, const SQObjectPtr &val);
    //returns true if a new slot has been created false if it was already present
    bool NewSlot(const SQObjectPtr &key,const SQObjectPtr &val);
    SQInteger Next(bool getweakrefs,const SQObjectPtr &refpos, SQObjectPtr &outkey, SQObjectPtr &outval);

    SQInteger CountUsed(){ return _lazy ? _lazy->_src->_table->CountUsed() + (SQInteger)_lazy->_added.size() : _usednodes;}
    void Clear();
    void Release()
    {
        sq_delete(this, SQTable);
    }

};

#endif //_SQTABLE_H_
