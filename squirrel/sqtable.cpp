/*
see copyright notice in squirrel.h
*/
#include "sqpcheader.h"
#include "sqvm.h"
#include "sqtable.h"
#include "sqfuncproto.h"
#include "sqclosure.h"
#include "sqclass.h"
#include "sqcensus.h"

#ifdef _SQ64
static_assert(sizeof(SQObject) == 16, "a 64-bit object is a 4-byte type and an 8-byte value");
#endif

SQTable::SQTable(SQSharedState *ss,SQInteger nInitialSize)
{
    SQInteger pow2size=MINPOWER2;
    while(nInitialSize>pow2size)pow2size=pow2size<<1;
    AllocNodes(pow2size);
    _usednodes = 0;
    _delegate = NULL;
    _lazy = NULL;
    _version = NULL;
    INIT_CHAIN();
    ADD_TO_CHAIN(&_sharedstate->_gc_chain,this);
}

void SQTable::Remove(const SQObjectPtr &key)
{
    if (_lazy) {
        if (!Has(key)) return;
        Materialize();
    }
    if (_version) _DetachVersion();
    _HashNode *n = _Get(key, HashObj(key) & (_numofnodes - 1));
    if (n) {
        if (sq_census_on) sq_census_write(this, key);
        n->val.Null();
        n->key.Null();
        _usednodes--;
        Rehash(false);
    }
}

void SQTable::AllocNodes(SQInteger nSize)
{
#ifdef _SQ64
    static_assert(sizeof(_HashNode) == 32, "a node packs its next index beside its key");
#endif
    // A next index is 32 bits (see _HashKey): 4G nodes, 128 GB of them.
    assert(nSize <= 0xFFFFFFFF);
    _HashNode *nodes=(_HashNode *)SQ_MALLOC(sizeof(_HashNode)*nSize);
    for(SQInteger i=0;i<nSize;i++){
        _HashNode &n = nodes[i];
        new (&n) _HashNode;
    }
    _numofnodes=nSize;
    _nodes=nodes;
    _firstfree=&_nodes[_numofnodes-1];
}

void SQTable::Rehash(bool force)
{
    SQInteger oldsize=_numofnodes;
    //prevent problems with the integer division
    if(oldsize<4)oldsize=4;
    _HashNode *nold=_nodes;
    SQInteger nelems=_usednodes;
    if (nelems >= oldsize-oldsize/4)  /* using more than 3/4? */
        AllocNodes(oldsize*2);
    else if (nelems <= oldsize/4 &&  /* less than 1/4? */
        oldsize > MINPOWER2)
        AllocNodes(oldsize/2);
    else if(force)
        AllocNodes(oldsize);
    else
        return;
    _usednodes = 0;
    int census = sq_census_on;
    sq_census_on = 0;
    for (SQInteger i=0; i<oldsize; i++) {
        _HashNode *old = nold+i;
        if (sq_type(old->key) != OT_NULL)
            _NewSlot(old->key.Obj(),old->val);
    }
    sq_census_on = census;
    for(SQInteger k=0;k<oldsize;k++)
        nold[k].~_HashNode();
    SQ_FREE(nold,oldsize*sizeof(_HashNode));
}

SQTable *SQTable::Clone()
{
    if (_lazy) Materialize();
    int census = sq_census_on;
    if (census) sq_census_clone(this);
    sq_census_on = 0;
    SQTable *nt=Create(_opt_ss(this),_numofnodes);
    SQInteger ridx=0;
    SQObjectPtr key,val;
    while((ridx=Next(true,ridx,key,val))!=-1){
        nt->NewSlot(key,val);
    }
    nt->SetDelegate(_delegate);
    sq_census_on = census;
    return nt;
}

bool SQTable::Get(const SQObjectPtr &key,SQObjectPtr &val)
{
    if(sq_type(key) == OT_NULL)
        return false;
    _HashNode *n = _Get(key, HashObj(key) & (_numofnodes - 1));
    if (n) {
        // `key` and `val` may be the same object (sq_rawget).
        if (sq_census_on) sq_census_get(this, key, n->val);
        val = _realval(n->val);
        return true;
    }
    if (_lazy) return _LazyGet(SQObjectPtr(key), val);
    return false;
}

bool SQTable::NewSlot(const SQObjectPtr &key,const SQObjectPtr &val)
{
    assert(sq_type(key) != OT_NULL);
    if (sq_census_on) sq_census_write(this, key);
    if (_version) _DetachVersion();
    if (_lazy) {
        _HashNode *n = _Get(key, HashObj(key) & (_numofnodes - 1));
        if (n) {
            n->val = val;
            return false;
        }
        if (_lazy->_src->_table->Has(key)) {
            _NewSlot(key, val);
            return false;
        }
        _NewSlot(key, val);
        _lazy->_added.push_back(key);
        return true;
    }
    return _NewSlot(key, val);
}

bool SQTable::_NewSlot(const SQObject &key,const SQObjectPtr &val)
{
    SQHash h = HashObj(key) & (_numofnodes - 1);
    _HashNode *n = _Get(key, h);
    if (n) {
        n->val = val;
        return false;
    }
    _HashNode *mp = &_nodes[h];
    n = mp;


    //key not found I'll insert it
    //main pos is not free

    if(sq_type(mp->key) != OT_NULL) {
        n = _firstfree;  /* get a free place */
        SQHash mph = HashObj(mp->key.Obj()) & (_numofnodes - 1);
        _HashNode *othern;  /* main position of colliding node */

        if (mp > n && (othern = &_nodes[mph]) != mp){
            /* yes; move colliding node into free position */
            while (_NextOf(_nodes, othern) != mp){
                assert(_NextOf(_nodes, othern) != NULL);
                othern = _NextOf(_nodes, othern);  /* find previous */
            }
            _SetNext(_nodes, othern, n);  /* redo the chain with `n' in place of `mp' */
            n->key.Set(mp->key.Obj());
            n->val = mp->val;/* copy colliding node into free pos. (mp->next also goes) */
            n->key._next = mp->key._next;
            mp->key.Null();
            mp->val.Null();
            mp->key._next = 0;  /* now `mp' is free */
        }
        else{
            /* new node will go into free position */
            n->key._next = mp->key._next;  /* chain new position */
            _SetNext(_nodes, mp, n);
            mp = n;
        }
    }
    mp->key.Set(key);

    for (;;) {  /* correct `firstfree' */
        if (sq_type(_firstfree->key) == OT_NULL && _firstfree->key._next == 0) {
            mp->val = val;
            _usednodes++;
            return true;  /* OK; table still has a free place */
        }
        else if (_firstfree == _nodes) break;  /* cannot decrement from here */
        else (_firstfree)--;
    }
    Rehash(true);
    return _NewSlot(key, val);
}

SQInteger SQTable::Next(bool getweakrefs,const SQObjectPtr &refpos, SQObjectPtr &outkey, SQObjectPtr &outval)
{
    SQInteger idx = (SQInteger)TranslateIndex(refpos);
    if (sq_census_on && idx == 0) sq_census_iterate(this);
    if (_lazy) Materialize();
    while (idx < _numofnodes) {
        if(sq_type(_nodes[idx].key) != OT_NULL) {
            //first found
            _HashNode &n = _nodes[idx];
            outkey = n.key.Obj();
            outval = getweakrefs?(SQObject)n.val:_realval(n.val);
            //return idx for the next iteration
            return ++idx;
        }
        ++idx;
    }
    //nothing to iterate anymore
    return -1;
}


bool SQTable::Set(const SQObjectPtr &key, const SQObjectPtr &val)
{
    if (_version) {
        if (!Has(key)) return false;
        _DetachVersion();
    }
    _HashNode *n = _Get(key, HashObj(key) & (_numofnodes - 1));
    if (n) {
        if (sq_census_on) sq_census_write(this, key);
        n->val = val;
        return true;
    }
    if (_lazy && _lazy->_src->_table->Has(key)) {
        if (sq_census_on) sq_census_write(this, key);
        _NewSlot(key, val);
        return true;
    }
    return false;
}

void SQTable::_ClearNodes()
{
    for(SQInteger i = 0;i < _numofnodes; i++) { _HashNode &n = _nodes[i]; n.key.Null(); n.val.Null(); }
}

void SQTable::Finalize()
{
    _ClearNodes();
    _ReleaseLazy();
    SetDelegate(NULL);
}

void SQTable::Clear()
{
    if (sq_census_on) sq_census_iterate(this);
    if (_lazy) Materialize();
    if (_version) _DetachVersion();
    _ClearNodes();
    _usednodes = 0;
    Rehash(true);
}

////////////////////////////////////////////////////////////////////////////////
// Lazy tables (see SQTable::_lazy).

// `o` (a closure or a native closure) bound to `env`, as sq_bindenv binds it.
static SQObjectPtr BindClosure(const SQObjectPtr &o, SQWeakRef *env)
{
    if (sq_type(o) == OT_CLOSURE) {
        SQClosure *c = _closure(o)->Clone();
        __ObjRelease(c->_env);
        c->_env = env;
        __ObjAddRef(c->_env);
        if (_closure(o)->_base) {
            c->_base = _closure(o)->_base;
            __ObjAddRef(c->_base);
        }
        return SQObjectPtr(c);
    }
    SQNativeClosure *c = _nativeclosure(o)->Clone();
    __ObjRelease(c->_env);
    c->_env = env;
    __ObjAddRef(c->_env);
    return SQObjectPtr(c);
}

// Whether a source slot's value is bound when the lazy table binds: what binding a
// table's closures binds (closures and native closures under string keys).
static bool Bindable(const SQObject &key, const SQObjectPtr &val)
{
    return sq_type(key) == OT_STRING && (sq_type(val) == OT_CLOSURE || sq_type(val) == OT_NATIVECLOSURE);
}

SQTable *SQTable::CreateLazy(SQSharedState *ss, SQTable *src)
{
    SQTable *t = Create(ss, 0);
    if (!src->_version) {
        SQTableVersion *v = (SQTableVersion *)SQ_MALLOC(sizeof(SQTableVersion));
        v->_refs = 0;
        v->_table = src;
        __ObjAddRef(src);
        src->_version = v;
    }
    SQTableLazy *lz = (SQTableLazy *)SQ_MALLOC(sizeof(SQTableLazy));
    new (lz) SQTableLazy();
    lz->_src = src->_version;
    lz->_src->_refs++;
    lz->_bind = NULL;
    t->_lazy = lz;
    t->SetDelegate(src->_delegate);
    return t;
}

void SQTable::LazyBind(SQWeakRef *env)
{
    if (!_lazy) return;
    __ObjAddRef(env);
    __ObjRelease(_lazy->_bind);
    _lazy->_bind = env;
}

void SQTable::_ReleaseLazy()
{
    SQTableLazy *lz = _lazy;
    if (!lz) return;
    _lazy = NULL;
    SQTableVersion *v = lz->_src;
    if (--v->_refs == 0) {
        SQTable *t = v->_table;
        if (t && t->_version == v) t->_version = NULL;
        SQ_FREE(v, sizeof(SQTableVersion));
        if (t) __ObjRelease(t);
    }
    __ObjRelease(lz->_bind);
    lz->~SQTableLazy();
    SQ_FREE(lz, sizeof(SQTableLazy));
}

bool SQTable::Has(const SQObjectPtr &key)
{
    if (sq_type(key) == OT_NULL) return false;
    if (_Get(key, HashObj(key) & (_numofnodes - 1))) return true;
    return _lazy && _lazy->_src->_table->Has(key);
}

// `key` is a copy: the caller's may be `val` itself.
bool SQTable::_LazyGet(const SQObjectPtr &key, SQObjectPtr &val)
{
    if (!_lazy->_src->_table->Get(key, val)) return false;
    if (_lazy->_bind && Bindable(key, val)) {
        // Bound once and kept: the same closure on every read, as in the clone.
        val = BindClosure(val, _lazy->_bind);
        int census = sq_census_on;
        sq_census_on = 0;
        _NewSlot(key, val);
        sq_census_on = census;
    }
    if (sq_census_on) sq_census_get(this, key, val);
    return true;
}

// A copy laid out node for node as this table is, so that iterating it, or cloning it,
// gives what this table would.
SQTable *SQTable::_CopyVerbatim()
{
    if (_lazy) Materialize();
    SQTable *nt = Create(_opt_ss(this), _numofnodes);
    _HashNode *basesrc = _nodes;
    _HashNode *basedst = nt->_nodes;
    for (SQInteger n = 0; n < _numofnodes; n++) {
        basedst[n].key.Set(basesrc[n].key.Obj());
        basedst[n].val = basesrc[n].val;
        basedst[n].key._next = basesrc[n].key._next;
    }
    nt->_firstfree = basedst + (_firstfree - basesrc);
    nt->_usednodes = _usednodes;
    nt->SetDelegate(_delegate);
    return nt;
}

// Before a write changes what the lazy tables reading through this one see, they keep a
// copy of it as it is.
void SQTable::_DetachVersion()
{
    SQTableVersion *v = _version;
    int census = sq_census_on;
    sq_census_on = 0;
    SQTable *copy = _CopyVerbatim();
    sq_census_on = census;
    __ObjAddRef(copy);
    v->_table = copy;
    copy->_version = v;
    _version = NULL;
    // The version's reference to this table goes; whoever is writing to it holds another.
    SQTable *self = this;
    __ObjRelease(self);
}

void SQTable::Materialize()
{
    SQTableLazy *lz = _lazy;
    if (!lz) return;
    int census = sq_census_on;
    sq_census_on = 0;
    SQTable *src = lz->_src->_table;
    if (src->_lazy) src->Materialize();
    _HashNode *own = _nodes;
    SQInteger ownsize = _numofnodes;
    _lazy = NULL;
    // The clone: the source's slots in its order, in a table of its size.
    AllocNodes(src->_numofnodes);
    _usednodes = 0;
    for (SQInteger i = 0; i < src->_numofnodes; i++) {
        _HashNode &sn = src->_nodes[i];
        if (sq_type(sn.key) == OT_NULL) continue;
        SQObject skey = sn.key.Obj();
        _HashNode *o = &own[HashObj(skey) & (ownsize - 1)];
        do {
            if (_rawval(o->key) == _rawval(sn.key) && sq_type(o->key) == sq_type(sn.key)) break;
        } while ((o = _NextOf(own, o)));
        if (o) {
            _NewSlot(skey, o->val);
        }
        else {
            SQObjectPtr real = _realval(sn.val);
            if (lz->_bind && Bindable(skey, real)) _NewSlot(skey, BindClosure(real, lz->_bind));
            else _NewSlot(skey, sn.val);
        }
    }
    // Then the slots it gained since, in the order they came.
    for (SQUnsignedInteger a = 0; a < lz->_added.size(); a++) {
        SQObjectPtr &key = lz->_added[a];
        _HashNode *o = &own[HashObj(key) & (ownsize - 1)];
        do {
            if (_rawval(o->key) == _rawval(key) && sq_type(o->key) == sq_type(key)) break;
        } while ((o = _NextOf(own, o)));
        assert(o);
        if (o) _NewSlot(key, o->val);
    }
    for (SQInteger k = 0; k < ownsize; k++) own[k].~_HashNode();
    SQ_FREE(own, ownsize * sizeof(_HashNode));
    _lazy = lz;
    _ReleaseLazy();
    sq_census_on = census;
}
