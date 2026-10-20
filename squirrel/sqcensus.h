/*  see copyright notice in squirrel.h */
#ifndef _SQCENSUS_H_
#define _SQCENSUS_H_

// Access tracking for the heap census (measurement only): which tables were read,
// written, iterated or cloned, by key, and which closures a table read handed out.
// Built only with SQ_CENSUS defined (squirrel-sys's `census` feature), and then on only
// while sq_censustrack turns it on; otherwise the hooks below are nothing and
// sq_censustrack does nothing. Its state is the process's: track one VM at a time.
#ifdef SQ_CENSUS
struct SQTable;
struct SQObjectPtr;
extern int sq_census_on;
void sq_census_get(SQTable *t, const SQObjectPtr &key, const SQObjectPtr &val);
void sq_census_write(SQTable *t, const SQObjectPtr &key);
void sq_census_iterate(SQTable *t);
void sq_census_clone(SQTable *t);
// A slot added (4) or removed (8) after the table was made, while tracked.
void sq_census_reshape(SQTable *t, int flag);
void sq_census_forget(void *o);
#define SQ_CENSUS_HOOK(...) do { if (sq_census_on) { __VA_ARGS__; } } while (0)
#define SQ_CENSUS_FORGET(o) sq_census_forget(o)
#define SQ_CENSUS_PAUSE() int _census_saved = sq_census_on; sq_census_on = 0
#define SQ_CENSUS_RESUME() sq_census_on = _census_saved
#else
#define SQ_CENSUS_HOOK(...) do { } while (0)
#define SQ_CENSUS_FORGET(o) do { } while (0)
#define SQ_CENSUS_PAUSE() do { } while (0)
#define SQ_CENSUS_RESUME() do { } while (0)
#endif

#endif //_SQCENSUS_H_
