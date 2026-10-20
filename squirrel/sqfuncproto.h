/*  see copyright notice in squirrel.h */
#ifndef _SQFUNCTION_H_
#define _SQFUNCTION_H_

#include "sqopcodes.h"

enum SQOuterType {
    otLOCAL = 0,
    otOUTER = 1
};

struct SQOuterVar
{

    SQOuterVar(){}
    SQOuterVar(const SQObjectPtr &name,const SQObjectPtr &src,SQOuterType t)
    {
        _name = name;
        _src=src;
        _type=t;
    }
    SQOuterVar(const SQOuterVar &ov)
    {
        _type=ov._type;
        _src=ov._src;
        _name=ov._name;
    }
    SQOuterType _type;
    SQObjectPtr _name;
    SQObjectPtr _src;
};

struct SQLocalVarInfo
{
    SQLocalVarInfo():_start_op(0),_end_op(0),_pos(0){}
    SQLocalVarInfo(const SQLocalVarInfo &lvi)
    {
        _name=lvi._name;
        _start_op=lvi._start_op;
        _end_op=lvi._end_op;
        _pos=lvi._pos;
    }
    SQObjectPtr _name;
    SQUnsignedInteger _start_op;
    SQUnsignedInteger _end_op;
    SQUnsignedInteger _pos;
};

struct SQLineInfo { SQInteger _line;SQInteger _op; };

// A function's debug information, compact: its locals' names, then the line table and
// their positions and live ranges as varints of zigzagged differences (the same values as
// SQLineInfo and SQLocalVarInfo), read as a stream where they are looked up and whole when
// saved. One allocation: this header, the names, the bytes.
struct SQDebugInfo
{
    SQInteger _nlocals;
    SQInteger _nlines;
    unsigned int _nbytes;
    // Where the locals start in Bytes(), after the line table.
    unsigned int _localsoff : 31;
    // The line table's ops never decrease (as the compiler almost always emits them), so
    // a line lookup can stop at the first op past the one it looks for.
    unsigned int _linessorted : 1;
    SQObjectPtr *Names() { return (SQObjectPtr *)(this + 1); }
    unsigned char *Bytes() { return (unsigned char *)(Names() + _nlocals); }
    static SQDebugInfo *Create(const SQLocalVarInfo *locals, SQInteger nlocals, const SQLineInfo *lines, SQInteger nlines);
    void Release();
    SQInteger Size() const { return (SQInteger)sizeof(SQDebugInfo) + _nlocals * (SQInteger)sizeof(SQObjectPtr) + (SQInteger)_nbytes; }
    // Decode into `out` (_nlocals and _nlines entries).
    void Locals(SQLocalVarInfo *out);
    void Lines(SQLineInfo *out);
};

// A walk over a function's locals that are alive at an op (SQFunctionProto::NextLocal).
struct SQLocalCursor
{
    SQLocalCursor() : _p(NULL), _i(0) {}
    const unsigned char *_p;
    SQInteger _i;
};

typedef sqvector<SQOuterVar> SQOuterVarVec;
typedef sqvector<SQLocalVarInfo> SQLocalVarInfoVec;
typedef sqvector<SQLineInfo> SQLineInfoVec;

// The debug information (line and local infos) is held apart, compact (SQDebugInfo).
#define _FUNC_SIZE(ni,nl,nparams,nfuncs,nouters,defparams) (sizeof(SQFunctionProto) \
        +((ni-1)*sizeof(SQInstruction))+(nl*sizeof(SQObjectPtr)) \
        +(nparams*sizeof(SQObjectPtr))+(nfuncs*sizeof(SQObjectPtr)) \
        +(nouters*sizeof(SQOuterVar))+(defparams*sizeof(SQInteger)))


struct SQFunctionProto : public CHAINABLE_OBJ
{
private:
    SQFunctionProto(SQSharedState *ss);
    ~SQFunctionProto();

public:
    static SQFunctionProto *Create(SQSharedState *ss,SQInteger ninstructions,
        SQInteger nliterals,SQInteger nparameters,
        SQInteger nfunctions,SQInteger noutervalues,
        SQInteger nlineinfos,SQInteger nlocalvarinfos,SQInteger ndefaultparams)
    {
        SQFunctionProto *f;
        //I compact the whole class and members in a single memory allocation
        f = (SQFunctionProto *)sq_vm_malloc(_FUNC_SIZE(ninstructions,nliterals,nparameters,nfunctions,noutervalues,ndefaultparams));
        new (f) SQFunctionProto(ss);
        f->_ninstructions = ninstructions;
        f->_literals = (SQObjectPtr*)&f->_instructions[ninstructions];
        f->_nliterals = nliterals;
        f->_parameters = (SQObjectPtr*)&f->_literals[nliterals];
        f->_nparameters = nparameters;
        f->_functions = (SQObjectPtr*)&f->_parameters[nparameters];
        f->_nfunctions = nfunctions;
        f->_outervalues = (SQOuterVar*)&f->_functions[nfunctions];
        f->_noutervalues = noutervalues;
        f->_nlineinfos = nlineinfos;
        f->_nlocalvarinfos = nlocalvarinfos;
        f->_debuginfo = NULL;
        f->_defaultparams = (SQInteger *)&f->_outervalues[noutervalues];
        f->_ndefaultparams = ndefaultparams;

        _CONSTRUCT_VECTOR(SQObjectPtr,f->_nliterals,f->_literals);
        _CONSTRUCT_VECTOR(SQObjectPtr,f->_nparameters,f->_parameters);
        _CONSTRUCT_VECTOR(SQObjectPtr,f->_nfunctions,f->_functions);
        _CONSTRUCT_VECTOR(SQOuterVar,f->_noutervalues,f->_outervalues);
        return f;
    }
    void Release(){
        _DESTRUCT_VECTOR(SQObjectPtr,_nliterals,_literals);
        _DESTRUCT_VECTOR(SQObjectPtr,_nparameters,_parameters);
        _DESTRUCT_VECTOR(SQObjectPtr,_nfunctions,_functions);
        _DESTRUCT_VECTOR(SQOuterVar,_noutervalues,_outervalues);
        if (_debuginfo) _debuginfo->Release();
        SQInteger size = _FUNC_SIZE(_ninstructions,_nliterals,_nparameters,_nfunctions,_noutervalues,_ndefaultparams);
        this->~SQFunctionProto();
        sq_vm_free(this,size);
    }

    const SQChar* GetLocal(SQVM *v,SQUnsignedInteger stackbase,SQUnsignedInteger nseq,SQUnsignedInteger nop);
    // The next local (from `c`, which starts at the first) alive at `nop`: its name and
    // stack position. False when there are no more.
    bool NextLocal(SQLocalCursor &c,SQUnsignedInteger nop,const SQObjectPtr *&name,SQUnsignedInteger &pos);
    SQInteger GetLine(SQInstruction *curr);
    // The first entry of the line table (the line the function starts on), or 0.
    SQInteger FirstLine();
    // Sets the debug information, `_nlocalvarinfos` locals and `_nlineinfos` lines.
    void SetDebugInfo(const SQLocalVarInfo *locals, const SQLineInfo *lines);
    bool Save(SQVM *v,SQUserPointer up,SQWRITEFUNC write);
    static bool Load(SQVM *v,SQUserPointer up,SQREADFUNC read,SQObjectPtr &ret);
#ifndef NO_GARBAGE_COLLECTOR
    void Mark(SQCollectable **chain);
    void Finalize(){ _NULL_SQOBJECT_VECTOR(_literals,_nliterals); }
    SQObjectType GetType() {return OT_FUNCPROTO;}
#endif
    SQObjectPtr _sourcename;
    SQObjectPtr _name;
    SQInteger _stacksize;
    bool _bgenerator;
    // Battle Brothers compatibility: float arithmetic in this function is computed
    // in single precision (the original engine's SQFloat was 32-bit). Values stay
    // stored as SQFloat. Set via sq_setf32math, never serialized.
    bool _f32math;
    SQInteger _varparams;

    SQInteger _nlocalvarinfos;
    SQInteger _nlineinfos;
    SQDebugInfo *_debuginfo;

    SQInteger _nliterals;
    SQObjectPtr *_literals;

    SQInteger _nparameters;
    SQObjectPtr *_parameters;

    SQInteger _nfunctions;
    SQObjectPtr *_functions;

    SQInteger _noutervalues;
    SQOuterVar *_outervalues;

    SQInteger _ndefaultparams;
    SQInteger *_defaultparams;

    SQInteger _ninstructions;
    SQInstruction _instructions[1];
};

#endif //_SQFUNCTION_H_
