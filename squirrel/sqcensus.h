/*  see copyright notice in squirrel.h */
#ifndef _SQCENSUS_H_
#define _SQCENSUS_H_

// Access tracking for the heap census (measurement only; off unless sq_censustrack turns
// it on): which tables were read, written, iterated or cloned, by key, and which closures
// a table read handed out.
struct SQTable;
struct SQObjectPtr;
extern int sq_census_on;
void sq_census_get(SQTable *t, const SQObjectPtr &key, const SQObjectPtr &val);
void sq_census_write(SQTable *t, const SQObjectPtr &key);
void sq_census_iterate(SQTable *t);
void sq_census_clone(SQTable *t);
void sq_census_forget(void *o);

#endif //_SQCENSUS_H_
