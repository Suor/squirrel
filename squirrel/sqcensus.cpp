/*
see copyright notice in squirrel.h
*/
// A heap census for measuring what the VM's objects cost: every collectable object, its
// size in bytes as allocated, and for tables, closures and function prototypes the
// references and shape an offline analysis needs. Measurement only: it reads the VM.
#include "sqpcheader.h"
#include <stdio.h>
#include "sqvm.h"
#include "sqstring.h"
#include "sqtable.h"
#include "sqarray.h"
#include "sqfuncproto.h"
#include "sqclosure.h"
#include "sqclass.h"
#include "squserdata.h"
#include "sqcensus.h"
#include <unordered_map>
#include <unordered_set>
#include <string>

#ifdef SQ_CENSUS
int sq_census_on = 0;
static int census_enabled = 0;
static int census_paused = 0;

struct CensusTableTrack
{
    // 1 iterated, 2 cloned, 4 a slot added, 8 a slot removed (or cleared), 16 iterated
    // after a slot was added or removed, 32 cloned after.
    int flags;
    std::unordered_set<SQRawObjectVal> gets, writes;
    // Where it was first iterated and first cloned (the script functions on the stack).
    std::string iterated_at, cloned_at;
};
static std::unordered_map<void *, CensusTableTrack> *census_tables;
static std::unordered_set<void *> *census_fetched;

static void census_update() { sq_census_on = census_enabled && census_paused == 0; }

void sq_census_get(SQTable *t, const SQObjectPtr &key, const SQObjectPtr &val)
{
    (*census_tables)[t].gets.insert(_rawval(key));
    if (sq_type(val) == OT_CLOSURE || sq_type(val) == OT_NATIVECLOSURE) census_fetched->insert(val._unVal.pRefCounted);
}
void sq_census_write(SQTable *t, const SQObjectPtr &key) { (*census_tables)[t].writes.insert(_rawval(key)); }
// The innermost script functions running (`name@source`, innermost first), or natives.
static std::string census_site(SQTable *t)
{
    std::string site;
    SQObjectPtr &root = t->_sharedstate->_root_vm;
    if (sq_type(root) != OT_THREAD) return site;
    SQVM *v = _thread(root);
    int n = 0;
    for (SQInteger i = v->_callsstacksize - 1; i >= 0 && n < 3; i--) {
        SQObjectPtr &c = v->_callsstack[i]._closure;
        if (!site.empty()) site += "<";
        if (sq_type(c) == OT_CLOSURE) {
            SQFunctionProto *p = _closure(c)->_function;
            site += sq_type(p->_name) == OT_STRING ? _stringval(p->_name) : "?";
            site += "@";
            site += sq_type(p->_sourcename) == OT_STRING ? _stringval(p->_sourcename) : "?";
            n++;
        }
        else if (sq_type(c) == OT_NATIVECLOSURE) {
            SQObjectPtr &nm = _nativeclosure(c)->_name;
            site += "native:";
            site += sq_type(nm) == OT_STRING ? _stringval(nm) : "?";
        }
    }
    if (site.empty()) site = "host";
    return site;
}

void sq_census_iterate(SQTable *t)
{
    CensusTableTrack &tr = (*census_tables)[t];
    if (tr.flags & 12) tr.flags |= 16;
    if (!(tr.flags & 1)) tr.iterated_at = census_site(t);
    tr.flags |= 1;
}
void sq_census_clone(SQTable *t)
{
    CensusTableTrack &tr = (*census_tables)[t];
    if (tr.flags & 12) tr.flags |= 32;
    if (!(tr.flags & 2)) tr.cloned_at = census_site(t);
    tr.flags |= 2;
}
void sq_census_reshape(SQTable *t, int flag) { (*census_tables)[t].flags |= flag; }
void sq_census_forget(void *o)
{
    if (!census_tables) return;
    census_tables->erase(o);
    census_fetched->erase(o);
}

// 1: start tracking (forgetting what was tracked), 0: stop, 2: pause, 3: resume.
void sq_censustrack(HSQUIRRELVM v, SQInteger mode)
{
    (void)v;
    switch (mode) {
    case 1:
        // Made when first started, so pausing and resuming alone costs nothing.
        if (!census_tables) {
            census_tables = new std::unordered_map<void *, CensusTableTrack>();
            census_fetched = new std::unordered_set<void *>();
        }
        census_tables->clear(); census_fetched->clear(); census_enabled = 1; census_paused = 0; break;
    case 0: census_enabled = 0; break;
    case 2: census_paused++; break;
    case 3: if (census_paused > 0) census_paused--; break;
    }
    census_update();
}

#else
// Built without access tracking: nothing to track.
void sq_censustrack(HSQUIRRELVM v, SQInteger mode) { (void)v; (void)mode; }
#endif

// Whether a table read handed out this closure while tracked.
static int census_was_fetched(void *o)
{
#ifdef SQ_CENSUS
    return census_fetched && census_fetched->count(o) ? 1 : 0;
#else
    (void)o;
    return 0;
#endif
}

#ifdef SQ_CENSUS
static void census_keys(FILE *f, const char *tag, const std::unordered_set<SQRawObjectVal> &keys, SQTable *t,
    const std::unordered_set<SQRawObjectVal> &strings);
#endif

static void census_value(FILE *f, const SQObject &o);
static void *census_env(SQWeakRef *env);

struct SQCensus
{
    static SQInteger TableNodes(SQTable *t) { return t->_numofnodes; }
    static void Header(FILE *f, SQTable *t, int weak)
    {
        SQInteger nodes = TableNodes(t);
        SQInteger extra = 0;
        if (t->Lazy()) extra += sizeof(SQTableLazy) + t->Lazy()->_added.capacity() * sizeof(SQObjectPtr);
        if (t->Version()) extra += sizeof(SQTableVersion);
        fprintf(f, "T %p %lld %lld %lld %p %d\n", (void *)t,
            (long long)(sizeof(SQTable) + nodes * TableNodeSize() + extra), (long long)nodes,
            (long long)t->_usednodes, (void *)t->_delegate, weak);
        if (t->Lazy())
            fprintf(f, "L %p %p %lld\n", (void *)t->Lazy()->_src->_table, census_env(t->Lazy()->_bind),
                (long long)t->Lazy()->_added.size());
    }
    // A table's own slots (a lazy table's are only what it holds), without iterating it.
    static void Slots(FILE *f, SQTable *t)
    {
        for (SQInteger i = 0; i < t->_numofnodes; i++) {
            SQTable::_HashNode &n = t->_nodes[i];
            if (sq_type(n.key) == OT_NULL) continue;
            fputs("s ", f);
            census_value(f, n.key.Obj());
            fputc(' ', f);
            census_value(f, n.val);
            fputc('\n', f);
        }
    }
    static SQInteger TableNodeSize() { return sizeof(SQTable::_HashNode); }
    static void StringSet(SQSharedState *ss, std::unordered_set<SQRawObjectVal> &out)
    {
        SQStringTable *st = ss->_stringtable;
        for (SQUnsignedInteger i = 0; i < st->_numofslots; i++)
            for (SQString *s = st->_strings[i]; s; s = s->_next) out.insert((SQRawObjectVal)s);
    }
    // The string table's chains: its slots, the longest chain, the strings looked at
    // finding every string once (the sum of each one's place in its chain), and the
    // strings sharing a chain with another of the same length (those compare bytes).
    static void Chains(SQSharedState *ss, SQInteger &slots, SQInteger &longest, SQInteger &probes, SQInteger &samelen)
    {
        SQStringTable *st = ss->_stringtable;
        slots = (SQInteger)st->_numofslots;
        longest = probes = samelen = 0;
        for (SQUnsignedInteger i = 0; i < st->_numofslots; i++) {
            SQInteger n = 0;
            for (SQString *s = st->_strings[i]; s; s = s->_next) {
                probes += ++n;
                for (SQString *o = st->_strings[i]; o; o = o->_next)
                    if (o != s && o->_len == s->_len) { samelen++; break; }
            }
            if (n > longest) longest = n;
        }
    }
    static void Strings(SQSharedState *ss, SQInteger &count, SQInteger &bytes)
    {
        SQStringTable *st = ss->_stringtable;
        count = 0;
        bytes = (SQInteger)(st->_numofslots * sizeof(SQString *));
        for (SQUnsignedInteger i = 0; i < st->_numofslots; i++) {
            for (SQString *s = st->_strings[i]; s; s = s->_next) {
                count++;
                bytes += (SQInteger)(sizeof(SQString) + (s->_len + 1) * sizeof(SQChar));
            }
        }
    }
};

static void census_escape(FILE *f, const SQChar *s, SQInteger len)
{
    for (SQInteger i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c <= ' ' || c == '%' || c >= 0x7f) fprintf(f, "%%%02X", c);
        else fputc(c, f);
    }
    if (len == 0) fputs("%", f);
}

static void census_value(FILE *f, const SQObject &o)
{
    switch (sq_type(o)) {
    case OT_NULL: fputs("n", f); break;
    case OT_INTEGER: fprintf(f, "i%lld", (long long)_integer(o)); break;
    case OT_FLOAT: fputs("f", f); break;
    case OT_BOOL: fprintf(f, "b%d", _integer(o) ? 1 : 0); break;
    case OT_STRING: fputs("s", f); census_escape(f, _stringval(o), _string(o)->_len); break;
    case OT_TABLE: fprintf(f, "T%p", (void *)_table(o)); break;
    case OT_ARRAY: fprintf(f, "A%p", (void *)_array(o)); break;
    case OT_CLOSURE: fprintf(f, "C%p", (void *)_closure(o)); break;
    case OT_NATIVECLOSURE: fprintf(f, "N%p", (void *)_nativeclosure(o)); break;
    case OT_WEAKREF: fprintf(f, "W%p", (void *)_weakref(o)->_obj._unVal.pRefCounted); break;
    default: fprintf(f, "o%x:%p", (unsigned)sq_type(o), (void *)o._unVal.pRefCounted); break;
    }
}

static void *census_env(SQWeakRef *env)
{
    if (!env || sq_type(env->_obj) == OT_NULL) return NULL;
    return (void *)env->_obj._unVal.pRefCounted;
}

SQRESULT sq_census(HSQUIRRELVM v, const SQChar *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return SQ_ERROR;
    SQSharedState *ss = _ss(v);
    SQInteger scount, sbytes;
    SQCensus::Strings(ss, scount, sbytes);
    SQInteger slots, longest, probes, samelen;
    SQCensus::Chains(ss, slots, longest, probes, samelen);
    fprintf(f, "S %lld %lld %lld %lld %lld %lld\n", (long long)scount, (long long)sbytes, (long long)slots,
        (long long)longest, (long long)probes, (long long)samelen);
#ifdef SQ_CENSUS
    // The interned strings, to print tracked keys that are strings by their text.
    std::unordered_set<SQRawObjectVal> strings;
    if (census_tables) SQCensus::StringSet(ss, strings);
#endif
    fprintf(f, "Z table=%d node=%lld closure=%d nclosure=%d array=%d proto=%d weakref=%d outer=%d string=%d\n",
        (int)sizeof(SQTable), (long long)SQCensus::TableNodeSize(), (int)sizeof(SQClosure),
        (int)sizeof(SQNativeClosure), (int)sizeof(SQArray), (int)sizeof(SQFunctionProto),
        (int)sizeof(SQWeakRef), (int)sizeof(SQOuter), (int)sizeof(SQString));
    for (SQCollectable *c = ss->_gc_chain; c; c = c->_next) {
        int weak = c->_weakref ? 1 : 0;
        switch (c->GetType()) {
        case OT_TABLE: {
            SQTable *t = (SQTable *)c;
            SQCensus::Header(f, t, weak);
#ifdef SQ_CENSUS
            int on = sq_census_on;
            sq_census_on = 0;
            if (census_tables) {
                auto it = census_tables->find(t);
                if (it != census_tables->end()) {
                    if (it->second.flags) fprintf(f, "x %d\n", it->second.flags);
                    if (!it->second.iterated_at.empty()) fprintf(f, "r %s\n", it->second.iterated_at.c_str());
                    if (!it->second.cloned_at.empty()) fprintf(f, "q %s\n", it->second.cloned_at.c_str());
                    census_keys(f, "g", it->second.gets, t, strings);
                    census_keys(f, "w", it->second.writes, t, strings);
                }
            }
            sq_census_on = on;
#endif
            SQCensus::Slots(f, t);
            break;
        }
        case OT_ARRAY: {
            SQArray *a = (SQArray *)c;
            fprintf(f, "A %p %lld %lld %lld %d\n", (void *)a,
                (long long)(sizeof(SQArray) + a->_values.capacity() * sizeof(SQObjectPtr)),
                (long long)a->Size(), (long long)a->_values.capacity(), weak);
            break;
        }
        case OT_CLOSURE: {
            SQClosure *cl = (SQClosure *)c;
            SQFunctionProto *p = cl->_function;
            fprintf(f, "C %p %lld %p %p %lld %lld %d %d\n", (void *)cl, (long long)_CALC_CLOSURE_SIZE(p),
                (void *)p, census_env(cl->_env), (long long)p->_noutervalues, (long long)p->_ndefaultparams, weak,
                census_was_fetched(cl));
            break;
        }
        case OT_NATIVECLOSURE: {
            SQNativeClosure *nc = (SQNativeClosure *)c;
            fprintf(f, "N %p %lld %p %lld %d %d ", (void *)nc,
                (long long)(_CALC_NATVIVECLOSURE_SIZE(nc->_noutervalues) + nc->_typecheck.capacity() * sizeof(SQInteger)),
                census_env(nc->_env), (long long)nc->_noutervalues, weak,
                census_was_fetched(nc));
            census_value(f, nc->_name);
            fputc('\n', f);
            break;
        }
        case OT_FUNCPROTO: {
            SQFunctionProto *p = (SQFunctionProto *)c;
            fprintf(f, "F %p %lld %lld %lld %lld %lld %lld %lld %lld %lld ", (void *)p,
                (long long)(_FUNC_SIZE(p->_ninstructions, p->_nliterals, p->_nparameters, p->_nfunctions,
                    p->_noutervalues, p->_ndefaultparams) + (p->_debuginfo ? p->_debuginfo->Size() : 0)),
                (long long)p->_ninstructions, (long long)p->_nliterals, (long long)p->_nparameters,
                (long long)p->_nfunctions, (long long)p->_noutervalues, (long long)p->_nlineinfos,
                (long long)p->_nlocalvarinfos, (long long)p->_ndefaultparams);
            census_value(f, p->_name);
            fputc(' ', f);
            census_value(f, p->_sourcename);
            fputc('\n', f);
            if (p->_debuginfo) fprintf(f, "G %lld\n", (long long)p->_debuginfo->Size());
            break;
        }
        case OT_OUTER:
            fprintf(f, "O %p %lld %d\n", (void *)c, (long long)sizeof(SQOuter), weak);
            break;
        case OT_USERDATA: {
            SQUserData *u = (SQUserData *)c;
            fprintf(f, "U %p %lld %d\n", (void *)u, (long long)(sizeof(SQUserData) + u->_size), weak);
            break;
        }
        case OT_INSTANCE: {
            SQInstance *i = (SQInstance *)c;
            fprintf(f, "I %p %lld %d\n", (void *)i, (long long)i->_memsize, weak);
            break;
        }
        case OT_CLASS: {
            SQClass *k = (SQClass *)c;
            fprintf(f, "K %p %lld %d\n", (void *)k,
                (long long)(sizeof(SQClass) + k->_defaultvalues.capacity() * sizeof(SQClassMember)
                    + k->_methods.capacity() * sizeof(SQClassMember)), weak);
            break;
        }
        default:
            fprintf(f, "X %p %x %d\n", (void *)c, (unsigned)c->GetType(), weak);
            break;
        }
    }
    fclose(f);
    return SQ_OK;
}

#ifdef SQ_CENSUS
// A tracked table's keys: strings by their text, others by their raw value.
static void census_keys(FILE *f, const char *tag, const std::unordered_set<SQRawObjectVal> &keys, SQTable *t,
    const std::unordered_set<SQRawObjectVal> &strings)
{
    (void)t;
    for (SQRawObjectVal k : keys) {
        fprintf(f, "%s ", tag);
        if (strings.count(k)) {
            SQString *str = (SQString *)k;
            fputc('s', f);
            census_escape(f, str->_val, str->_len);
        }
        else fprintf(f, "#%llx", (unsigned long long)k);
        fputc('\n', f);
    }
}
#endif
