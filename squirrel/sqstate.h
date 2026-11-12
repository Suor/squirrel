/*  see copyright notice in squirrel.h */
#ifndef _SQSTATE_H_
#define _SQSTATE_H_

#include "squtils.h"
#include "sqobject.h"
struct SQString;
struct SQTable;
//max number of character for a printed number
#define NUMBER_MAX_CHAR 50

struct SQStringTable
{
    friend struct SQCensus;
    SQStringTable(SQSharedState*ss);
    ~SQStringTable();
    SQString *Add(const SQChar *,SQInteger len);
    SQString* Concat(const SQChar* a, SQInteger alen, const SQChar* b, SQInteger blen);
    void Remove(SQString *);
private:
    void Resize(SQInteger size);
    void AllocNodes(SQInteger size);
    SQString **_strings;
    SQUnsignedInteger _numofslots;
    SQUnsignedInteger _slotused;
    SQSharedState *_sharedstate;
};

struct RefTable {
    struct RefNode {
        SQObjectPtr obj;
        SQUnsignedInteger refs;
        struct RefNode *next;
    };
    RefTable();
    ~RefTable();
    void AddRef(SQObject &obj);
    SQBool Release(SQObject &obj);
    SQUnsignedInteger GetRefCount(SQObject &obj);
#ifndef NO_GARBAGE_COLLECTOR
    void Mark(SQCollectable **chain);
#endif
    void Finalize();
private:
    RefNode *Get(SQObject &obj,SQHash &mainpos,RefNode **prev,bool add);
    RefNode *Add(SQHash mainpos,SQObject &obj);
    void Resize(SQUnsignedInteger size);
    void AllocNodes(SQUnsignedInteger size);
    SQUnsignedInteger _numofslots;
    SQUnsignedInteger _slotused;
    RefNode *_nodes;
    RefNode *_freelist;
    RefNode **_buckets;
};

#define ADD_STRING(ss,str,len) ss->_stringtable->Add(str,len)
#define REMOVE_STRING(ss,bstr) ss->_stringtable->Remove(bstr)

struct SQObjectPtr;
struct SQClosure;

#ifndef NO_GARBAGE_COLLECTOR
// The closures sq_bindenv made last, by the closure they were bound from: one is handed
// out again for the same binding once nothing but this cache holds it (see sq_bindenv).
// Emptied before every collection and at close, so it never keeps an object alive that
// would otherwise have been freed by then.
#define SQ_BINDCACHE_SIZE 64
struct SQBindCacheEntry
{
    SQClosure *_src; // not held: its destructor clears the entry
    SQObjectPtr _bound;
};
#endif

// Where a table lookup that missed the table itself ended, through its delegates or the
// root table (see SQVM::ChainGet). `_h` is the table that has the key (one of `_t`'s
// delegates, the default delegate, or with bit 0 set the root table), or NULL: a lookup
// the cache leaves to the full path (a delegate's `_get` may answer it). Raw pointers, not
// held: every table an entry depends on is watched (SQTable::Watched), and a watched
// table's change that could change an entry's outcome (a key given or removed, a delegate
// set or cleared) makes the entry stale, as does freeing the table it started at. Stale is
// told by `_stamp`, the sum of the generations it depends on: all entries', its key's and
// its table's (each only ever counts up, so the sum is the same only while none moved).
#define SQ_LOOKUPCACHE_SIZE 8192
#define SQ_LOOKUPKEYGENS 1024
#define SQ_LOOKUPTABLEGENS 1024
struct SQLookupCacheEntry
{
    SQTable *_t;
    SQString *_k;
    SQUnsignedInteger _h;
    unsigned long long _stamp;
};

struct SQSharedState
{
    SQSharedState();
    ~SQSharedState();
    void Init();
public:
    SQChar* GetScratchPad(SQInteger size);
    SQInteger GetMetaMethodIdxByName(const SQObjectPtr &name);
#ifndef NO_GARBAGE_COLLECTOR
    SQInteger CollectGarbage(SQVM *vm);
    void RunMark(SQVM *vm,SQCollectable **tchain);
    SQInteger ResurrectUnreachable(SQVM *vm);
    static void MarkObject(const SQObject &o,SQCollectable **chain);
    static SQUnsignedInteger BindCacheSlot(const SQClosure *src);
    void FlushBindCache();
    void ForgetBound(const SQClosure *bound);
    void ForgetBindSource(const SQClosure *src) {
        SQBindCacheEntry &e = _bindcache[BindCacheSlot(src)];
        if(e._src == src) { e._src = NULL; e._bound.Null(); }
    }
#endif
    // The lookup cache (SQLookupCacheEntry). Every entry stale:
    void LookupFlush() { _lookupgen++; }
    // A watched table gained or lost `key`.
    void LookupKeyChanged(const SQObject &key);
    // `t`, a table lookups started at, is being freed: its entries go (with those of the
    // tables that share its generation).
    void LookupForget(const SQTable *t) { _lookuptablegen[LookupTableGen(t)]++; }
    static SQUnsignedInteger LookupSlot(const SQTable *t, const SQString *k) {
        return (SQUnsignedInteger)(((((unsigned long long)(size_t)t) ^ (((unsigned long long)(size_t)k) << 7)) * 0x9E3779B97F4A7C15ULL) >> 40) & (SQ_LOOKUPCACHE_SIZE - 1);
    }
    static SQUnsignedInteger LookupTableGen(const SQTable *t) {
        return (SQUnsignedInteger)((((unsigned long long)(size_t)t) * 0x9E3779B97F4A7C15ULL) >> 40) & (SQ_LOOKUPTABLEGENS - 1);
    }
    unsigned long long LookupStamp(const SQTable *t, SQHash khash) const {
        return _lookupgen + _lookupkeygen[khash & (SQ_LOOKUPKEYGENS - 1)] + _lookuptablegen[LookupTableGen(t)];
    }
    // `name` (one of GetTypeName's) as a string, made once: typeof's result.
    const SQObjectPtr &TypeName(const SQChar *name);
    SQObjectPtrVec *_metamethods;
    SQObjectPtr _metamethodsmap;
    SQObjectPtrVec *_systemstrings;
    SQObjectPtrVec *_types;
    SQStringTable *_stringtable;
    RefTable _refs_table;
    SQObjectPtr _registry;
    SQObjectPtr _consts;
    SQObjectPtr _constructoridx;
#ifndef NO_GARBAGE_COLLECTOR
    SQCollectable *_gc_chain;
#endif
    SQObjectPtr _root_vm;
    SQObjectPtr _table_default_delegate;
    static const SQRegFunction _table_default_delegate_funcz[];
    SQObjectPtr _array_default_delegate;
    static const SQRegFunction _array_default_delegate_funcz[];
    SQObjectPtr _string_default_delegate;
    static const SQRegFunction _string_default_delegate_funcz[];
    SQObjectPtr _number_default_delegate;
    static const SQRegFunction _number_default_delegate_funcz[];
    SQObjectPtr _generator_default_delegate;
    static const SQRegFunction _generator_default_delegate_funcz[];
    SQObjectPtr _closure_default_delegate;
    static const SQRegFunction _closure_default_delegate_funcz[];
    SQObjectPtr _thread_default_delegate;
    static const SQRegFunction _thread_default_delegate_funcz[];
    SQObjectPtr _class_default_delegate;
    static const SQRegFunction _class_default_delegate_funcz[];
    SQObjectPtr _instance_default_delegate;
    static const SQRegFunction _instance_default_delegate_funcz[];
    SQObjectPtr _weakref_default_delegate;
    static const SQRegFunction _weakref_default_delegate_funcz[];

    SQCOMPILERERROR _compilererrorhandler;
    SQPRINTFUNCTION _printfunc;
    SQPRINTFUNCTION _errorfunc;
    bool _debuginfo;
    bool _notifyallexceptions;
    SQUserPointer _foreignptr;
    SQRELEASEHOOK _releasehook;
    const SQChar *_typenames[16];
    SQObjectPtr _typenamestrings[16];
    bool _lookupcacheon;
    unsigned long long _lookupgen;
    unsigned long long _lookupkeygen[SQ_LOOKUPKEYGENS];
    unsigned long long _lookuptablegen[SQ_LOOKUPTABLEGENS];
    SQLookupCacheEntry _lookupcache[SQ_LOOKUPCACHE_SIZE];
#ifndef NO_GARBAGE_COLLECTOR
    // Last, after the members the VM reads all the time.
    SQBindCacheEntry _bindcache[SQ_BINDCACHE_SIZE];
#endif
private:
    SQChar *_scratchpad;
    SQInteger _scratchpadsize;
};

#define _sp(s) (_sharedstate->GetScratchPad(s))
#define _spval (_sharedstate->GetScratchPad(-1))

#define _table_ddel     _table(_sharedstate->_table_default_delegate)
#define _array_ddel     _table(_sharedstate->_array_default_delegate)
#define _string_ddel    _table(_sharedstate->_string_default_delegate)
#define _number_ddel    _table(_sharedstate->_number_default_delegate)
#define _generator_ddel _table(_sharedstate->_generator_default_delegate)
#define _closure_ddel   _table(_sharedstate->_closure_default_delegate)
#define _thread_ddel    _table(_sharedstate->_thread_default_delegate)
#define _class_ddel     _table(_sharedstate->_class_default_delegate)
#define _instance_ddel  _table(_sharedstate->_instance_default_delegate)
#define _weakref_ddel   _table(_sharedstate->_weakref_default_delegate)

bool CompileTypemask(SQIntVec &res,const SQChar *typemask);


#endif //_SQSTATE_H_
