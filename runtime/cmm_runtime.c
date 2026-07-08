/* cmm_runtime.c - implementation of the cmm runtime. */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#  define _DEFAULT_SOURCE 1
#endif
#ifdef _WIN32
  /* Winsock2 must be included before <windows.h> (pulled in by the header)
   * so that WSADATA/WSAStartup and the v2 socket API are in scope everywhere,
   * not just in the networking section. WIN32_LEAN_AND_MEAN keeps <windows.h>
   * from also dragging in the conflicting v1 <winsock.h>. */
  #ifndef WIN32_LEAN_AND_MEAN
  #  define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <wincrypt.h>
#endif
#include "cmm_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <math.h>
#include <time.h>
#include <ctype.h>

/* ======================================================================== */
/* Region allocator. Each function call runs in its own region (arena); the  */
/* region is freed when the call returns, after its result has been          */
/* relocated into the caller's region. A global root region holds program-   */
/* lifetime data (the entry object, @class-variable state, thread results).  */
/* ======================================================================== */

#if defined(_MSC_VER)
  #define CMM_TLS __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
  #define CMM_TLS __thread
#else
  #define CMM_TLS _Thread_local
#endif

typedef struct ArenaBlock {
    struct ArenaBlock *next;
    size_t used, cap;
    char *data;
} ArenaBlock;

struct CmmArena {
    ArenaBlock  *blocks;
    CmmArena   *parent;
    cmm_mutex_t lock;     /* only the root region is touched by >1 thread */
};

static CmmArena      *g_root = NULL;       /* program-lifetime region */
static CMM_TLS CmmArena *t_cur = NULL;    /* current region for this thread */
static int             g_inited = 0;

static ArenaBlock *arena_block_new(CmmArena *a, size_t need) {
    size_t cap = 1u << 16;            /* 64 KiB default block */
    if (need > cap) cap = need;
    ArenaBlock *b = (ArenaBlock *)malloc(sizeof(ArenaBlock));
    if (!b) { fprintf(stderr, "lang: out of memory\n"); exit(1); }
    b->data = (char *)malloc(cap);
    if (!b->data) { fprintf(stderr, "lang: out of memory\n"); exit(1); }
    b->used = 0; b->cap = cap; b->next = a->blocks;
    a->blocks = b;
    return b;
}

static void *arena_alloc(CmmArena *a, size_t n) {
    n = (n + 15u) & ~((size_t)15u);   /* 16-byte align */
    cmm_mutex_lock(&a->lock);
    if (!a->blocks || a->blocks->used + n > a->blocks->cap)
        arena_block_new(a, n);
    void *p = a->blocks->data + a->blocks->used;
    a->blocks->used += n;
    cmm_mutex_unlock(&a->lock);
    memset(p, 0, n);
    return p;
}

static CmmArena *arena_create(CmmArena *parent) {
    CmmArena *a = (CmmArena *)malloc(sizeof(CmmArena));
    if (!a) { fprintf(stderr, "lang: out of memory\n"); exit(1); }
    a->blocks = NULL;
    a->parent = parent;
    cmm_mutex_init(&a->lock);
    return a;
}

static void arena_destroy(CmmArena *a) {
    ArenaBlock *b = a->blocks;
    while (b) { ArenaBlock *n = b->next; free(b->data); free(b); b = n; }
    free(a);
}

void *cmm_alloc(size_t n) { return arena_alloc(t_cur, n); }

/* ---- CMM_DEBUG: startup banner + crash handler (debug builds only) ---- */
#ifdef CMM_DEBUG
#include <signal.h>
static void cmm_dbg_signal(int sig){
    fprintf(stderr, "\n[cmm debug] fatal signal %d - this is a -g/-O0 build; run it under "
                    "gdb or lldb to see the .cmm source line (statements are #line-mapped).\n", sig);
    signal(sig, SIG_DFL);
    raise(sig);
}
#if defined(__GNUC__) || defined(__clang__)
__attribute__((constructor))
#endif
static void cmm_dbg_init(void){
    signal(SIGSEGV, cmm_dbg_signal);
    signal(SIGABRT, cmm_dbg_signal);
#ifdef SIGFPE
    signal(SIGFPE, cmm_dbg_signal);
#endif
    fprintf(stderr, "[cmm debug] debug runtime active - source-mapped via #line, symbols via -g\n");
}
#endif

/* forward declarations (definitions appear later in this file) */
static void list_push(CmmList *l, CmmValue v);
static void dict_put(CmmDict *d, CmmValue key, CmmValue val);
#ifdef CMM_HAVE_TLS
static void cmm_tls_init(void);
#endif

/* ======================================================================== */
/* Mutexes / threads (portable)                                             */
/* ======================================================================== */

void cmm_mutex_init(cmm_mutex_t *m) {
#ifdef _WIN32
    InitializeCriticalSection(m);
#else
    pthread_mutex_init(m, NULL);
#endif
}
void cmm_mutex_lock(cmm_mutex_t *m) {
#ifdef _WIN32
    EnterCriticalSection(m);
#else
    pthread_mutex_lock(m);
#endif
}
void cmm_mutex_unlock(cmm_mutex_t *m) {
#ifdef _WIN32
    LeaveCriticalSection(m);
#else
    pthread_mutex_unlock(m);
#endif
}

#ifdef _WIN32
typedef struct { void *(*fn)(void *); void *arg; } WinTramp;
static DWORD WINAPI win_trampoline(LPVOID p) {
    WinTramp *t = (WinTramp *)p;
    t->fn(t->arg);
    return 0;
}
#endif

CmmJob *cmm_job_new(void) {
    CmmJob *j = (CmmJob *)cmm_alloc(sizeof(CmmJob));
    j->result = cmm_empty();
    j->started = 0;
    j->arena = t_cur;
    return j;
}

void cmm_thread_start(CmmJob *job, void *(*fn)(void *), void *arg) {
#ifdef _WIN32
    WinTramp *t = (WinTramp *)cmm_alloc(sizeof(WinTramp));
    t->fn = fn; t->arg = arg;
    job->thread = CreateThread(NULL, 0, win_trampoline, t, 0, NULL);
    job->started = (job->thread != NULL);
    if (!job->started) { fn(arg); }     /* fall back to synchronous */
#else
    if (pthread_create(&job->thread, NULL, fn, arg) == 0)
        job->started = 1;
    else
        fn(arg);                         /* synchronous fallback */
#endif
}

CmmValue cmm_job_value(CmmJob *job) {
    CmmValue v; v.tag = CV_JOB; v.job = job; return v;
}

CmmValue cmm_job_wait(CmmValue jobv) {
    if (jobv.tag != CV_JOB) return cmm_empty();
    CmmJob *j = jobv.job;
    if (j->started) {
#ifdef _WIN32
        WaitForSingleObject(j->thread, INFINITE);
        CloseHandle(j->thread);
#else
        pthread_join(j->thread, NULL);
#endif
        j->started = 0;
    }
    return j->result;
}

/* ======================================================================== */
/* init                                                                     */
/* ======================================================================== */

void cmm_init(void) {
    if (g_inited) return;
    g_inited = 1;
    g_root = arena_create(NULL);
    t_cur = g_root;
    srand((unsigned)time(NULL));
#ifdef _WIN32
    {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
#endif
    /* TLS is initialized lazily on the first HTTPS/TLS call, so a program that
       never makes one pays nothing and startup can't fault in mbedTLS. */
}

/* A worker thread starts with no region; root it at the program region so
 * its result is program-lifetime (the waiter reads it after the thread ends). */
void cmm_thread_attach(void) {
    t_cur = g_root;
}

/* ---- relocate / copy across regions ----------------------------------- */
/* relocate: used on return. Move only values that live in the dying region
 * into the parent; share everything already in a longer-lived region. */
static CmmValue relocate(CmmValue v, CmmArena *dying) {
    switch (v.tag) {
        case CV_STRING:
            if (!v.s || v.s->arena != dying) return v;
            return cmm_str_n(v.s->data, v.s->len);
        case CV_LIST: {
            if (!v.list || v.list->arena != dying) return v;
            CmmValue nl = cmm_new_list();
            for (size_t i = 0; i < v.list->len; i++)
                list_push(nl.list, relocate(v.list->items[i], dying));
            return nl;
        }
        case CV_DICT: {
            if (!v.dict || v.dict->arena != dying) return v;
            CmmValue nd = cmm_new_dict();
            for (size_t i = 0; i < v.dict->len; i++) {
                CmmValue k; k.tag = CV_STRING; k.s = v.dict->entries[i].key;
                dict_put(nd.dict, relocate(k, dying),
                                  relocate(v.dict->entries[i].val, dying));
            }
            return nd;
        }
        case CV_OBJECT: {
            if (!v.obj || v.obj->arena != dying) return v;
            CmmObject *o = cmm_new_object(v.obj->class_id, v.obj->nfields,
                                            v.obj->is_empty);
            for (size_t i = 0; i < v.obj->nfields; i++)
                o->fields[i] = relocate(v.obj->fields[i], dying);
            return cmm_object_value(o);
        }
        case CV_SOCKET: {
            if (!v.sock || v.sock->arena != dying) return v;
            CmmSocket *s = (CmmSocket *)cmm_alloc(sizeof(CmmSocket));
            *s = *v.sock; s->arena = t_cur;
            CmmValue r; r.tag = CV_SOCKET; r.sock = s; return r;
        }
        default: return v;   /* primitives, jobs */
    }
}

/* copy_rec: used when storing into a longer-lived container/field. Copy any
 * value not already in the target region so the container owns its data. */
static CmmValue copy_rec(CmmValue v, CmmArena *target) {
    switch (v.tag) {
        case CV_STRING:
            if (!v.s || v.s->arena == target) return v;
            return cmm_str_n(v.s->data, v.s->len);
        case CV_LIST: {
            if (!v.list || v.list->arena == target) return v;
            CmmValue nl = cmm_new_list();
            for (size_t i = 0; i < v.list->len; i++)
                list_push(nl.list, copy_rec(v.list->items[i], target));
            return nl;
        }
        case CV_DICT: {
            if (!v.dict || v.dict->arena == target) return v;
            CmmValue nd = cmm_new_dict();
            for (size_t i = 0; i < v.dict->len; i++) {
                CmmValue k; k.tag = CV_STRING; k.s = v.dict->entries[i].key;
                dict_put(nd.dict, copy_rec(k, target),
                                  copy_rec(v.dict->entries[i].val, target));
            }
            return nd;
        }
        case CV_OBJECT:
            /* Objects are shared mutable state: keep the reference so distinct
             * threads/scopes see the same instance (required for @locks). */
            return v;
        default: return v;
    }
}

static CmmValue copy_into(CmmValue v, CmmArena *target) {
    CmmArena *save = t_cur; t_cur = target;
    CmmValue r = copy_rec(v, target);
    t_cur = save;
    return r;
}

void cmm_frame_enter(void) {
    t_cur = arena_create(t_cur);
}

CmmValue cmm_frame_leave(CmmValue ret) {
    CmmArena *dying = t_cur;
    CmmArena *parent = dying->parent ? dying->parent : g_root;
    t_cur = parent;                       /* new allocations land in parent  */
    CmmValue out = relocate(ret, dying); /* uses t_cur == parent            */
    arena_destroy(dying);
    return out;
}

void cmm_field_set(CmmValue self, int idx, CmmValue v) {
    if (self.tag != CV_OBJECT || !self.obj) return;
    if (idx < 0 || (size_t)idx >= self.obj->nfields) return;
    self.obj->fields[idx] = copy_into(v, self.obj->arena);
}

/* ======================================================================== */
/* Value constructors                                                       */
/* ======================================================================== */

CmmValue cmm_empty(void){ CmmValue v; v.tag = CV_EMPTY; v.i = 0; return v; }
CmmValue cmm_int(int64_t x){ CmmValue v; v.tag = CV_INT; v.i = x; return v; }
CmmValue cmm_float(double x){ CmmValue v; v.tag = CV_FLOAT; v.f = x; return v; }
CmmValue cmm_bool(int x){ CmmValue v; v.tag = CV_BOOL; v.b = x ? 1 : 0; return v; }

CmmValue cmm_str_n(const char *p, size_t n) {
    CmmString *s = (CmmString *)cmm_alloc(sizeof(CmmString));
    s->data = (char *)cmm_alloc(n + 1);
    if (p && n) memcpy(s->data, p, n);
    s->data[n] = 0;
    s->len = n;
    s->arena = t_cur;
    CmmValue v; v.tag = CV_STRING; v.s = s; return v;
}
CmmValue cmm_str(const char *c) { return cmm_str_n(c, c ? strlen(c) : 0); }

CmmValue cmm_new_list(void) {
    CmmList *l = (CmmList *)cmm_alloc(sizeof(CmmList));
    l->len = 0; l->cap = 4;
    l->arena = t_cur;
    l->items = (CmmValue *)cmm_alloc(sizeof(CmmValue) * l->cap);
    CmmValue v; v.tag = CV_LIST; v.list = l; return v;
}
CmmValue cmm_new_dict(void) {
    CmmDict *d = (CmmDict *)cmm_alloc(sizeof(CmmDict));
    d->len = 0; d->cap = 4;
    d->arena = t_cur;
    d->entries = (CmmDictEntry *)cmm_alloc(sizeof(CmmDictEntry) * d->cap);
    CmmValue v; v.tag = CV_DICT; v.dict = d; return v;
}

CmmObject *cmm_new_object(int class_id, size_t nfields, int is_empty) {
    CmmObject *o = (CmmObject *)cmm_alloc(sizeof(CmmObject));
    o->class_id = class_id;
    o->is_empty = is_empty;
    o->nfields = nfields;
    o->arena = t_cur;
    o->fields = nfields ? (CmmValue *)cmm_alloc(sizeof(CmmValue) * nfields)
                        : NULL;
    o->locks = nfields ? (cmm_mutex_t *)cmm_alloc(sizeof(cmm_mutex_t) * nfields)
                       : NULL;
    for (size_t k = 0; k < nfields; k++) {
        o->fields[k] = cmm_empty();
        cmm_mutex_init(&o->locks[k]);
    }
    return o;
}

CmmValue cmm_object_value(CmmObject *o) {
    CmmValue v; v.tag = CV_OBJECT; v.obj = o; return v;
}

/* ======================================================================== */
/* Truthiness / emptiness / stringify                                       */
/* ======================================================================== */

int cmm_truthy(CmmValue v) {
    switch (v.tag) {
        case CV_EMPTY:  return 0;
        case CV_INT:    return v.i != 0;
        case CV_FLOAT:  return v.f != 0.0;
        case CV_BOOL:   return v.b;
        case CV_STRING: return v.s && v.s->len > 0;
        case CV_LIST:   return v.list && v.list->len > 0;
        case CV_DICT:   return v.dict && v.dict->len > 0;
        case CV_OBJECT: return v.obj && !v.obj->is_empty;
        default:        return 1;
    }
}

int cmm_is_empty(CmmValue v) {
    switch (v.tag) {
        case CV_EMPTY:  return 1;
        case CV_INT:    return v.i == 0;
        case CV_FLOAT:  return v.f == 0.0;
        case CV_BOOL:   return v.b == 0;
        case CV_STRING: return !v.s || v.s->len == 0;
        case CV_LIST:   return !v.list || v.list->len == 0;
        case CV_DICT:   return !v.dict || v.dict->len == 0;
        case CV_OBJECT: return !v.obj || v.obj->is_empty;
        case CV_SOCKET: return !v.sock || !v.sock->open;
        default:        return 0;
    }
}

/* small dynamic char buffer for building strings */
typedef struct { char *p; size_t len, cap; } SB;
static void sb_init(SB *b){ b->cap = 32; b->len = 0; b->p = (char*)malloc(b->cap); b->p[0]=0; }
static void sb_putn(SB *b, const char *s, size_t n){
    if (b->len + n + 1 > b->cap){ while(b->len+n+1>b->cap) b->cap*=2; b->p=(char*)realloc(b->p,b->cap);}
    memcpy(b->p+b->len, s, n); b->len += n; b->p[b->len]=0;
}
static void sb_put(SB *b, const char *s){ sb_putn(b, s, strlen(s)); }
static CmmValue sb_to_str(SB *b){ CmmValue v = cmm_str_n(b->p, b->len); free(b->p); return v; }

static void stringify_into(SB *b, CmmValue v, int quoted);

static void stringify_list(SB *b, CmmList *l){
    sb_put(b, "[");
    for (size_t k=0;k<l->len;k++){ if(k) sb_put(b, ", "); stringify_into(b, l->items[k], 1); }
    sb_put(b, "]");
}
static void stringify_dict(SB *b, CmmDict *d){
    sb_put(b, "{");
    for (size_t k=0;k<d->len;k++){
        if(k) sb_put(b, ", ");
        sb_put(b, "\""); sb_putn(b, d->entries[k].key->data, d->entries[k].key->len); sb_put(b, "\": ");
        stringify_into(b, d->entries[k].val, 1);
    }
    sb_put(b, "}");
}
static void stringify_into(SB *b, CmmValue v, int quoted){
    char tmp[64];
    switch (v.tag){
        case CV_EMPTY:  sb_put(b, quoted ? "null" : ""); break;
        case CV_INT:    snprintf(tmp,sizeof tmp,"%lld",(long long)v.i); sb_put(b,tmp); break;
        case CV_FLOAT:  snprintf(tmp,sizeof tmp,"%g",v.f); sb_put(b,tmp); break;
        case CV_BOOL:   sb_put(b, v.b?"true":"false"); break;
        case CV_STRING:
            if (quoted){ sb_put(b,"\""); if(v.s) sb_putn(b,v.s->data,v.s->len); sb_put(b,"\""); }
            else if (v.s) sb_putn(b, v.s->data, v.s->len);
            break;
        case CV_LIST:   stringify_list(b, v.list); break;
        case CV_DICT:   stringify_dict(b, v.dict); break;
        case CV_OBJECT: sb_put(b, "<object>"); break;
        case CV_SOCKET: sb_put(b, "<socket>"); break;
        case CV_JOB:    sb_put(b, "<job>"); break;
    }
}

CmmString *cmm_to_string(CmmValue v) {
    if (v.tag==CV_STRING) return v.s ? v.s : cmm_str_n("",0).s;  /* avoid SB rebuild */
    SB b; sb_init(&b);
    stringify_into(&b, v, 0);
    CmmValue s = sb_to_str(&b);
    return s.s;
}

void cmm_print(CmmValue v, int newline) {
    CmmString *s = cmm_to_string(v);
    fwrite(s->data, 1, s->len, stdout);
    if (newline) fputc('\n', stdout);
    fflush(stdout);
}

/* ======================================================================== */
/* numeric coercion helpers                                                 */
/* ======================================================================== */

static double as_double(CmmValue v){
    switch(v.tag){ case CV_INT: return (double)v.i; case CV_FLOAT: return v.f;
                   case CV_BOOL: return v.b; default: return 0.0; }
}
static int64_t as_int(CmmValue v){
    switch(v.tag){ case CV_INT: return v.i; case CV_FLOAT: return (int64_t)v.f;
                   case CV_BOOL: return v.b; default: return 0; }
}
static int both_int(CmmValue a, CmmValue b){
    return (a.tag==CV_INT||a.tag==CV_BOOL) && (b.tag==CV_INT||b.tag==CV_BOOL);
}

/* ======================================================================== */
/* operators                                                                */
/* ======================================================================== */

CmmValue cmm_add(CmmValue a, CmmValue b){
    if (a.tag==CV_STRING || b.tag==CV_STRING){
        CmmString *sa = cmm_to_string(a), *sb = cmm_to_string(b);
        CmmValue r = cmm_str_n(NULL, sa->len + sb->len);
        memcpy(r.s->data, sa->data, sa->len);
        memcpy(r.s->data + sa->len, sb->data, sb->len);
        return r;
    }
    if (both_int(a,b)) return cmm_int(as_int(a)+as_int(b));
    return cmm_float(as_double(a)+as_double(b));
}
CmmValue cmm_sub(CmmValue a, CmmValue b){
    if (both_int(a,b)) return cmm_int(as_int(a)-as_int(b));
    return cmm_float(as_double(a)-as_double(b));
}
CmmValue cmm_mul(CmmValue a, CmmValue b){
    if (both_int(a,b)) return cmm_int(as_int(a)*as_int(b));
    return cmm_float(as_double(a)*as_double(b));
}
CmmValue cmm_div(CmmValue a, CmmValue b){
    if (both_int(a,b)){ int64_t d=as_int(b); return cmm_int(d? as_int(a)/d : 0); }
    double d=as_double(b); return cmm_float(d!=0.0? as_double(a)/d : 0.0);
}
CmmValue cmm_mod(CmmValue a, CmmValue b){
    int64_t d=as_int(b); return cmm_int(d? as_int(a)%d : 0);
}
CmmValue cmm_neg(CmmValue a){
    if (a.tag==CV_FLOAT) return cmm_float(-a.f);
    return cmm_int(-as_int(a));
}

static int values_equal(CmmValue a, CmmValue b){
    if (a.tag==CV_STRING && b.tag==CV_STRING)
        return a.s->len==b.s->len && memcmp(a.s->data,b.s->data,a.s->len)==0;
    if ((a.tag==CV_INT||a.tag==CV_FLOAT||a.tag==CV_BOOL) &&
        (b.tag==CV_INT||b.tag==CV_FLOAT||b.tag==CV_BOOL)){
        if (both_int(a,b)) return as_int(a)==as_int(b);
        return as_double(a)==as_double(b);
    }
    if (a.tag==CV_EMPTY || b.tag==CV_EMPTY) return cmm_is_empty(a)&&cmm_is_empty(b);
    if (a.tag==CV_OBJECT && b.tag==CV_OBJECT) return a.obj==b.obj;
    return 0;
}
CmmValue cmm_eq(CmmValue a, CmmValue b){ return cmm_bool(values_equal(a,b)); }
CmmValue cmm_ne(CmmValue a, CmmValue b){ return cmm_bool(!values_equal(a,b)); }

static int cmp(CmmValue a, CmmValue b){
    if (a.tag==CV_STRING && b.tag==CV_STRING){
        int c = memcmp(a.s->data,b.s->data, a.s->len<b.s->len?a.s->len:b.s->len);
        if (c) return c<0?-1:1;
        return a.s->len<b.s->len?-1:(a.s->len>b.s->len?1:0);
    }
    double x=as_double(a), y=as_double(b);
    return x<y?-1:(x>y?1:0);
}
CmmValue cmm_lt(CmmValue a, CmmValue b){ return cmm_bool(cmp(a,b)<0); }
CmmValue cmm_le(CmmValue a, CmmValue b){ return cmm_bool(cmp(a,b)<=0); }
CmmValue cmm_gt(CmmValue a, CmmValue b){ return cmm_bool(cmp(a,b)>0); }
CmmValue cmm_ge(CmmValue a, CmmValue b){ return cmm_bool(cmp(a,b)>=0); }
CmmValue cmm_and(CmmValue a, CmmValue b){ return cmm_bool(cmm_truthy(a)&&cmm_truthy(b)); }
CmmValue cmm_or (CmmValue a, CmmValue b){ return cmm_bool(cmm_truthy(a)||cmm_truthy(b)); }
CmmValue cmm_not(CmmValue a){ return cmm_bool(!cmm_truthy(a)); }

/* ======================================================================== */
/* List internals                                                           */
/* ======================================================================== */

static void list_push(CmmList *l, CmmValue v){
    if (l->len + 1 > l->cap){
        size_t nc = l->cap ? l->cap*2 : 4;
        CmmValue *ni = (CmmValue*)arena_alloc(l->arena, sizeof(CmmValue)*nc);
        if (l->items) memcpy(ni, l->items, sizeof(CmmValue)*l->len);
        l->items = ni; l->cap = nc;
    }
    l->items[l->len++] = v;
}

/* ======================================================================== */
/* Dict internals                                                           */
/* ======================================================================== */

static int dict_find(CmmDict *d, const char *k, size_t n){
    for (size_t i=0;i<d->len;i++)
        if (d->entries[i].key->len==n && memcmp(d->entries[i].key->data,k,n)==0)
            return (int)i;
    return -1;
}
/* store key (must already live in d->arena) + val directly, no copy */
static void dict_put_key(CmmDict *d, CmmString *ks, CmmValue val){
    int idx = dict_find(d, ks->data, ks->len);
    if (idx>=0){ d->entries[idx].val = val; return; }
    if (d->len+1 > d->cap){
        size_t nc = d->cap? d->cap*2 : 4;
        CmmDictEntry *ne = (CmmDictEntry*)arena_alloc(d->arena, sizeof(CmmDictEntry)*nc);
        if (d->entries) memcpy(ne, d->entries, sizeof(CmmDictEntry)*d->len);
        d->entries = ne; d->cap = nc;
    }
    d->entries[d->len].key = ks;
    d->entries[d->len].val = val;
    d->len++;
}
static void dict_put(CmmDict *d, CmmValue key, CmmValue val){
    CmmString *src = cmm_to_string(key);
    CmmArena *save = t_cur; t_cur = d->arena;          /* copy key into d's region */
    CmmString *ks = cmm_str_n(src->data, src->len).s;
    t_cur = save;
    dict_put_key(d, ks, val);
}

/* ======================================================================== */
/* Indexing + iteration                                                     */
/* ======================================================================== */

CmmValue cmm_index_get(CmmValue c, CmmValue index){
    if (c.tag==CV_LIST){
        int64_t i = as_int(index);
        if (i<0 || (size_t)i>=c.list->len) return cmm_empty();
        return c.list->items[i];
    }
    if (c.tag==CV_DICT){
        CmmString *k = cmm_to_string(index);
        int idx = dict_find(c.dict, k->data, k->len);
        return idx>=0 ? c.dict->entries[idx].val : cmm_empty();
    }
    if (c.tag==CV_STRING){
        int64_t i = as_int(index);
        if (i<0 || (size_t)i>=c.s->len) return cmm_str("");
        return cmm_str_n(c.s->data + i, 1);
    }
    return cmm_empty();
}
void cmm_index_set(CmmValue c, CmmValue index, CmmValue val){
    if (c.tag==CV_LIST){
        int64_t i = as_int(index);
        if (i<0) return;
        val = copy_into(val, c.list->arena);
        while ((size_t)i >= c.list->len) list_push(c.list, cmm_empty());
        c.list->items[i] = val;
    } else if (c.tag==CV_DICT){
        dict_put(c.dict, index, copy_into(val, c.dict->arena));
    }
}

size_t cmm_iter_count(CmmValue v){
    switch(v.tag){
        case CV_LIST:   return v.list->len;
        case CV_DICT:   return v.dict->len;
        case CV_STRING: return v.s->len;
        default:        return 0;
    }
}
CmmValue cmm_iter_at(CmmValue v, size_t i){
    switch(v.tag){
        case CV_LIST:   return v.list->items[i];
        case CV_DICT:   return (CmmValue){ .tag=CV_STRING, .s=v.dict->entries[i].key };
        case CV_STRING: return cmm_str_n(v.s->data+i, 1);
        default:        return cmm_empty();
    }
}

/* ======================================================================== */
/* Object locks                                                             */
/* ======================================================================== */

void cmm_object_lock(CmmObject *o, const int *idx, int n){
    /* idx already provided in deterministic (ascending) order by codegen */
    for (int k=0;k<n;k++) cmm_mutex_lock(&o->locks[idx[k]]);
}
void cmm_object_unlock(CmmObject *o, const int *idx, int n){
    for (int k=n-1;k>=0;k--) cmm_mutex_unlock(&o->locks[idx[k]]);
}

/* ======================================================================== */
/* String methods                                                           */
/* ======================================================================== */

CmmValue cmm_string_length(CmmValue s){ return cmm_int(s.tag==CV_STRING? (int64_t)s.s->len:0); }
CmmValue cmm_string_substring(CmmValue s, CmmValue a, CmmValue b){
    if (s.tag!=CV_STRING) return cmm_str("");
    int64_t start=as_int(a), end=as_int(b), n=(int64_t)s.s->len;
    if (start<0) start=0; if (end>n) end=n; if (end<start) end=start;
    return cmm_str_n(s.s->data+start, (size_t)(end-start));
}
static int find_sub(const char *h, size_t hn, const char *n, size_t nn){
    if (nn==0) return 0;
    if (nn>hn) return -1;
    for (size_t i=0;i+nn<=hn;i++) if (memcmp(h+i,n,nn)==0) return (int)i;
    return -1;
}
CmmValue cmm_string_indexof(CmmValue s, CmmValue sub){
    if (s.tag!=CV_STRING) return cmm_int(-1);
    CmmString *t = cmm_to_string(sub);
    return cmm_int(find_sub(s.s->data,s.s->len,t->data,t->len));
}
CmmValue cmm_string_contains(CmmValue s, CmmValue sub){
    if (s.tag!=CV_STRING) return cmm_bool(0);
    CmmString *t = cmm_to_string(sub);
    return cmm_bool(find_sub(s.s->data,s.s->len,t->data,t->len)>=0);
}
CmmValue cmm_string_startswith(CmmValue s, CmmValue sub){
    if (s.tag!=CV_STRING) return cmm_bool(0);
    CmmString *t = cmm_to_string(sub);
    if (t->len>s.s->len) return cmm_bool(0);
    return cmm_bool(memcmp(s.s->data,t->data,t->len)==0);
}
CmmValue cmm_string_endswith(CmmValue s, CmmValue sub){
    if (s.tag!=CV_STRING) return cmm_bool(0);
    CmmString *t = cmm_to_string(sub);
    if (t->len>s.s->len) return cmm_bool(0);
    return cmm_bool(memcmp(s.s->data+s.s->len-t->len,t->data,t->len)==0);
}
CmmValue cmm_string_upper(CmmValue s){
    if (s.tag!=CV_STRING) return s;
    CmmValue r = cmm_str_n(s.s->data, s.s->len);
    for (size_t i=0;i<r.s->len;i++) r.s->data[i]=(char)toupper((unsigned char)r.s->data[i]);
    return r;
}
CmmValue cmm_string_lower(CmmValue s){
    if (s.tag!=CV_STRING) return s;
    CmmValue r = cmm_str_n(s.s->data, s.s->len);
    for (size_t i=0;i<r.s->len;i++) r.s->data[i]=(char)tolower((unsigned char)r.s->data[i]);
    return r;
}
CmmValue cmm_string_trim(CmmValue s){
    if (s.tag!=CV_STRING) return s;
    const char *p=s.s->data; size_t n=s.s->len;
    while(n && isspace((unsigned char)p[0])){p++;n--;}
    while(n && isspace((unsigned char)p[n-1])) n--;
    return cmm_str_n(p,n);
}
CmmValue cmm_string_split(CmmValue s, CmmValue sep){
    CmmValue out = cmm_new_list();
    if (s.tag!=CV_STRING) return out;
    CmmString *d = cmm_to_string(sep);
    if (d->len==0){ list_push(out.list, cmm_str_n(s.s->data,s.s->len)); return out; }
    const char *p=s.s->data; size_t n=s.s->len, start=0;
    for (size_t i=0;i+d->len<=n;){
        if (memcmp(p+i,d->data,d->len)==0){
            list_push(out.list, cmm_str_n(p+start, i-start));
            i += d->len; start = i;
        } else i++;
    }
    list_push(out.list, cmm_str_n(p+start, n-start));
    return out;
}
CmmValue cmm_string_replace(CmmValue s, CmmValue a, CmmValue b){
    if (s.tag!=CV_STRING) return s;
    CmmString *from=cmm_to_string(a), *to=cmm_to_string(b);
    if (from->len==0) return cmm_str_n(s.s->data,s.s->len);
    SB sb; sb_init(&sb);
    const char *p=s.s->data; size_t n=s.s->len, i=0;
    while (i<n){
        if (i+from->len<=n && memcmp(p+i,from->data,from->len)==0){
            sb_putn(&sb,to->data,to->len); i+=from->len;
        } else { sb_putn(&sb,p+i,1); i++; }
    }
    return sb_to_str(&sb);
}
CmmValue cmm_string_toint(CmmValue s){
    if (s.tag!=CV_STRING) return cmm_int(as_int(s));
    return cmm_int((int64_t)strtoll(s.s->data,NULL,10));
}
CmmValue cmm_string_tofloat(CmmValue s){
    if (s.tag!=CV_STRING) return cmm_float(as_double(s));
    return cmm_float(strtod(s.s->data,NULL));
}
CmmValue cmm_identity(CmmValue s){ return s; }

/* ======================================================================== */
/* List methods                                                             */
/* ======================================================================== */

CmmValue cmm_list_add(CmmValue l, CmmValue v){ if(l.tag==CV_LIST) list_push(l.list, copy_into(v, l.list->arena)); return cmm_empty(); }
CmmValue cmm_list_remove(CmmValue l, CmmValue idx){
    if (l.tag!=CV_LIST) return cmm_empty();
    int64_t i=as_int(idx);
    if (i<0||(size_t)i>=l.list->len) return cmm_empty();
    for (size_t k=(size_t)i;k+1<l.list->len;k++) l.list->items[k]=l.list->items[k+1];
    l.list->len--;
    return cmm_empty();
}
CmmValue cmm_list_get(CmmValue l, CmmValue idx){ return cmm_index_get(l,idx); }
CmmValue cmm_list_set(CmmValue l, CmmValue idx, CmmValue v){ cmm_index_set(l,idx,v); return cmm_empty(); }
CmmValue cmm_list_length(CmmValue l){ return cmm_int(l.tag==CV_LIST?(int64_t)l.list->len:0); }
CmmValue cmm_list_contains(CmmValue l, CmmValue v){
    if (l.tag!=CV_LIST) return cmm_bool(0);
    for (size_t k=0;k<l.list->len;k++) if (values_equal(l.list->items[k],v)) return cmm_bool(1);
    return cmm_bool(0);
}
CmmValue cmm_list_clear(CmmValue l){ if(l.tag==CV_LIST) l.list->len=0; return cmm_empty(); }

/* ======================================================================== */
/* Dict methods                                                             */
/* ======================================================================== */

CmmValue cmm_dict_get(CmmValue d, CmmValue k){ return cmm_index_get(d,k); }
CmmValue cmm_dict_set(CmmValue d, CmmValue k, CmmValue v){ if(d.tag==CV_DICT) dict_put(d.dict,k,copy_into(v,d.dict->arena)); return cmm_empty(); }
CmmValue cmm_dict_has(CmmValue d, CmmValue k){
    if (d.tag!=CV_DICT) return cmm_bool(0);
    CmmString *ks=cmm_to_string(k);
    return cmm_bool(dict_find(d.dict,ks->data,ks->len)>=0);
}
CmmValue cmm_dict_remove(CmmValue d, CmmValue k){
    if (d.tag!=CV_DICT) return cmm_empty();
    CmmString *ks=cmm_to_string(k);
    int idx=dict_find(d.dict,ks->data,ks->len);
    if (idx<0) return cmm_empty();
    for (size_t i=(size_t)idx;i+1<d.dict->len;i++) d.dict->entries[i]=d.dict->entries[i+1];
    d.dict->len--;
    return cmm_empty();
}
CmmValue cmm_dict_keys(CmmValue d){
    CmmValue out=cmm_new_list();
    if (d.tag==CV_DICT) for (size_t i=0;i<d.dict->len;i++){
        CmmValue kv; kv.tag=CV_STRING; kv.s=d.dict->entries[i].key; list_push(out.list,kv);
    }
    return out;
}
CmmValue cmm_dict_length(CmmValue d){ return cmm_int(d.tag==CV_DICT?(int64_t)d.dict->len:0); }

/* Data delegates to dict behaviour */
CmmValue cmm_data_get(CmmValue d, CmmValue k){ return cmm_index_get(d,k); }
CmmValue cmm_data_set(CmmValue d, CmmValue k, CmmValue v){ if(d.tag==CV_DICT) dict_put(d.dict,k,v); return cmm_empty(); }
CmmValue cmm_data_has(CmmValue d, CmmValue k){ return cmm_dict_has(d,k); }
CmmValue cmm_data_length(CmmValue d){
    if (d.tag==CV_DICT) return cmm_int((int64_t)d.dict->len);
    if (d.tag==CV_LIST) return cmm_int((int64_t)d.list->len);
    if (d.tag==CV_STRING) return cmm_int((int64_t)d.s->len);
    return cmm_int(0);
}
CmmValue cmm_data_keys(CmmValue d){ return cmm_dict_keys(d); }

/* ======================================================================== */
/* numeric conversions                                                      */
/* ======================================================================== */

CmmValue cmm_int_tostr(CmmValue v){ return (CmmValue){.tag=CV_STRING,.s=cmm_to_string(v)}; }
CmmValue cmm_int_tofloat(CmmValue v){ return cmm_float(as_double(v)); }
CmmValue cmm_float_tostr(CmmValue v){ return (CmmValue){.tag=CV_STRING,.s=cmm_to_string(v)}; }
CmmValue cmm_float_toint(CmmValue v){ return cmm_int(as_int(v)); }
CmmValue cmm_bool_tostr(CmmValue v){ return cmm_str(cmm_truthy(v)?"true":"false"); }

/* ======================================================================== */
/* Console / Math                                                           */
/* ======================================================================== */

CmmValue cmm_console_print(CmmValue v){ cmm_print(v,0); return cmm_empty(); }
CmmValue cmm_console_println(CmmValue v){ cmm_print(v,1); return cmm_empty(); }
CmmValue cmm_console_read(void){
    SB b; sb_init(&b); int c;
    while((c=fgetc(stdin))!=EOF && c!='\n'){ char ch=(char)c; sb_putn(&b,&ch,1); }
    return sb_to_str(&b);
}

CmmValue cmm_math_sqrt(CmmValue v){ return cmm_float(sqrt(as_double(v))); }
CmmValue cmm_math_abs(CmmValue v){ return v.tag==CV_INT?cmm_int(v.i<0?-v.i:v.i):cmm_float(fabs(as_double(v))); }
CmmValue cmm_math_pow(CmmValue a, CmmValue b){ return cmm_float(pow(as_double(a),as_double(b))); }
CmmValue cmm_math_floor(CmmValue v){ return cmm_int((int64_t)floor(as_double(v))); }
CmmValue cmm_math_ceil(CmmValue v){ return cmm_int((int64_t)ceil(as_double(v))); }
CmmValue cmm_math_min(CmmValue a, CmmValue b){ return cmp(a,b)<=0?a:b; }
CmmValue cmm_math_max(CmmValue a, CmmValue b){ return cmp(a,b)>=0?a:b; }
CmmValue cmm_math_random(void){ return cmm_float((double)rand()/((double)RAND_MAX+1.0)); }
CmmValue cmm_math_pi(void){ return cmm_float(3.14159265358979323846); }

/* ======================================================================== */
/* File                                                                     */
/* ======================================================================== */

CmmValue cmm_file_read(CmmValue path){
    CmmString *p = cmm_to_string(path);
    FILE *f = fopen(p->data, "rb");
    if (!f) return cmm_str("");
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    if (n<0) n=0;
    CmmValue r = cmm_str_n(NULL,(size_t)n);
    if (n) { size_t got = fread(r.s->data,1,(size_t)n,f); r.s->len=got; r.s->data[got]=0; }
    fclose(f);
    return r;
}
static int file_put(const char *path, CmmValue data, const char *mode){
    FILE *f = fopen(path, mode);
    if (!f) return 0;
    CmmString *s = cmm_to_string(data);
    fwrite(s->data,1,s->len,f);
    fclose(f);
    return 1;
}
CmmValue cmm_file_write(CmmValue path, CmmValue data){ return cmm_bool(file_put(cmm_to_string(path)->data,data,"wb")); }
CmmValue cmm_file_append(CmmValue path, CmmValue data){ return cmm_bool(file_put(cmm_to_string(path)->data,data,"ab")); }
CmmValue cmm_file_exists(CmmValue path){
    FILE *f=fopen(cmm_to_string(path)->data,"rb");
    if (f){ fclose(f); return cmm_bool(1);} return cmm_bool(0);
}

/* ======================================================================== */
/* Date                                                                     */
/* ======================================================================== */

CmmValue cmm_date_now(void){ return cmm_int((int64_t)time(NULL)); }
CmmValue cmm_date_format(CmmValue epoch, CmmValue fmt){
    time_t t = (time_t)as_int(epoch);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[256];
    CmmString *f = cmm_to_string(fmt);
    size_t n = strftime(buf, sizeof buf, f->data, &tmv);
    return cmm_str_n(buf, n);
}
/* Current UTC time as an AWS SigV4 timestamp: YYYYMMDDTHHMMSSZ. */
CmmValue cmm_date_amz(void){
    time_t t=time(NULL); struct tm g;
#ifdef _WIN32
    gmtime_s(&g,&t);
#else
    gmtime_r(&t,&g);
#endif
    char buf[24]; size_t n=strftime(buf,sizeof buf,"%Y%m%dT%H%M%SZ",&g);
    return cmm_str_n(buf,n);
}

/* ===== PHP-style date() formatting ===================================== */
static const char *PHP_DAYS[7]={"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
static const char *PHP_DAYS_SH[7]={"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
static const char *PHP_MONS[12]={"January","February","March","April","May","June","July","August","September","October","November","December"};
static const char *PHP_MONS_SH[12]={"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
static int php_is_leap(int y){ return (y%4==0 && y%100!=0) || (y%400==0); }
static int php_days_in_month(int y,int m0){
    static const int dm[12]={31,28,31,30,31,30,31,31,30,31,30,31};
    if(m0==1 && php_is_leap(y)) return 29;
    return dm[m0];
}
static int php_weeks_in_year(int y){
    int p=(y + y/4 - y/100 + y/400)%7;
    int p1=((y-1) + (y-1)/4 - (y-1)/100 + (y-1)/400)%7;
    return (p==4 || p1==3) ? 53 : 52;
}
static void php_iso_week(const struct tm *t,int *isoY,int *isoW){
    int y=t->tm_year+1900;
    int ordinal=t->tm_yday+1;
    int iso_wday=(t->tm_wday==0)?7:t->tm_wday;
    int week=(ordinal - iso_wday + 10)/7;
    if(week<1){ *isoY=y-1; *isoW=php_weeks_in_year(y-1); return; }
    if(week>52){ if(week>php_weeks_in_year(y)){ *isoY=y+1; *isoW=1; return; } }
    *isoY=y; *isoW=week;
}
static void php_off(SB *out,long off,int colon){
    char s=off<0?'-':'+'; long a=off<0?-off:off; char tmp[16];
    if(colon) snprintf(tmp,sizeof tmp,"%c%02ld:%02ld",s,a/3600,(a%3600)/60);
    else      snprintf(tmp,sizeof tmp,"%c%02ld%02ld",s,a/3600,(a%3600)/60);
    sb_put(out,tmp);
}
static void php_date_fmt(const char *fmt,size_t flen,const struct tm *t,long gmtoff,const char *zone,int64_t epoch,SB *out){
    char tmp[40];
    for(size_t i=0;i<flen;i++){
        char c=fmt[i];
        if(c=='\\'){ if(i+1<flen){ sb_putn(out,&fmt[i+1],1); i++; } continue; }
        int y=t->tm_year+1900;
        switch(c){
        case 'd': snprintf(tmp,sizeof tmp,"%02d",t->tm_mday); sb_put(out,tmp); break;
        case 'j': snprintf(tmp,sizeof tmp,"%d",t->tm_mday); sb_put(out,tmp); break;
        case 'D': sb_put(out,PHP_DAYS_SH[t->tm_wday]); break;
        case 'l': sb_put(out,PHP_DAYS[t->tm_wday]); break;
        case 'N': snprintf(tmp,sizeof tmp,"%d",t->tm_wday==0?7:t->tm_wday); sb_put(out,tmp); break;
        case 'w': snprintf(tmp,sizeof tmp,"%d",t->tm_wday); sb_put(out,tmp); break;
        case 'S': { int dd=t->tm_mday; const char*suf="th";
            if(dd<11||dd>13){ int u=dd%10; if(u==1)suf="st"; else if(u==2)suf="nd"; else if(u==3)suf="rd"; }
            sb_put(out,suf); } break;
        case 'z': snprintf(tmp,sizeof tmp,"%d",t->tm_yday); sb_put(out,tmp); break;
        case 'W': { int iy,iw; php_iso_week(t,&iy,&iw); snprintf(tmp,sizeof tmp,"%02d",iw); sb_put(out,tmp); } break;
        case 'o': { int iy,iw; php_iso_week(t,&iy,&iw); snprintf(tmp,sizeof tmp,"%d",iy); sb_put(out,tmp); } break;
        case 'F': sb_put(out,PHP_MONS[t->tm_mon]); break;
        case 'M': sb_put(out,PHP_MONS_SH[t->tm_mon]); break;
        case 'm': snprintf(tmp,sizeof tmp,"%02d",t->tm_mon+1); sb_put(out,tmp); break;
        case 'n': snprintf(tmp,sizeof tmp,"%d",t->tm_mon+1); sb_put(out,tmp); break;
        case 't': snprintf(tmp,sizeof tmp,"%d",php_days_in_month(y,t->tm_mon)); sb_put(out,tmp); break;
        case 'L': snprintf(tmp,sizeof tmp,"%d",php_is_leap(y)?1:0); sb_put(out,tmp); break;
        case 'Y': snprintf(tmp,sizeof tmp,"%d",y); sb_put(out,tmp); break;
        case 'y': snprintf(tmp,sizeof tmp,"%02d",((y%100)+100)%100); sb_put(out,tmp); break;
        case 'a': sb_put(out,t->tm_hour<12?"am":"pm"); break;
        case 'A': sb_put(out,t->tm_hour<12?"AM":"PM"); break;
        case 'g': { int h=t->tm_hour%12; if(h==0)h=12; snprintf(tmp,sizeof tmp,"%d",h); sb_put(out,tmp);} break;
        case 'G': snprintf(tmp,sizeof tmp,"%d",t->tm_hour); sb_put(out,tmp); break;
        case 'h': { int h=t->tm_hour%12; if(h==0)h=12; snprintf(tmp,sizeof tmp,"%02d",h); sb_put(out,tmp);} break;
        case 'H': snprintf(tmp,sizeof tmp,"%02d",t->tm_hour); sb_put(out,tmp); break;
        case 'i': snprintf(tmp,sizeof tmp,"%02d",t->tm_min); sb_put(out,tmp); break;
        case 's': snprintf(tmp,sizeof tmp,"%02d",t->tm_sec); sb_put(out,tmp); break;
        case 'U': snprintf(tmp,sizeof tmp,"%lld",(long long)epoch); sb_put(out,tmp); break;
        case 'u': sb_put(out,"000000"); break;
        case 'v': sb_put(out,"000"); break;
        case 'T': sb_put(out,(zone&&zone[0])?zone:"UTC"); break;
        case 'e': sb_put(out,(zone&&zone[0])?zone:"UTC"); break;
        case 'Z': snprintf(tmp,sizeof tmp,"%ld",gmtoff); sb_put(out,tmp); break;
        case 'O': php_off(out,gmtoff,0); break;
        case 'P': php_off(out,gmtoff,1); break;
        case 'c': snprintf(tmp,sizeof tmp,"%04d-%02d-%02dT%02d:%02d:%02d",y,t->tm_mon+1,t->tm_mday,t->tm_hour,t->tm_min,t->tm_sec); sb_put(out,tmp); php_off(out,gmtoff,1); break;
        case 'r': snprintf(tmp,sizeof tmp,"%s, %02d %s %04d %02d:%02d:%02d ",PHP_DAYS_SH[t->tm_wday],t->tm_mday,PHP_MONS_SH[t->tm_mon],y,t->tm_hour,t->tm_min,t->tm_sec); sb_put(out,tmp); php_off(out,gmtoff,0); break;
        default: sb_putn(out,&c,1); break;
        }
    }
}
CmmValue cmm_date_date(CmmValue fmt, CmmValue epoch){
    time_t t=(time_t)as_int(epoch); struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv,&t);
#else
    localtime_r(&t,&tmv);
#endif
    long off=0; const char *zone="";
#if defined(__GLIBC__) || defined(__APPLE__)
    off=tmv.tm_gmtoff; zone=tmv.tm_zone;
#endif
    CmmString *f=cmm_to_string(fmt); SB b; sb_init(&b);
    php_date_fmt(f->data,f->len,&tmv,off,zone,(int64_t)t,&b);
    return sb_to_str(&b);
}
CmmValue cmm_date_gmdate(CmmValue fmt, CmmValue epoch){
    time_t t=(time_t)as_int(epoch); struct tm tmv;
#ifdef _WIN32
    gmtime_s(&tmv,&t);
#else
    gmtime_r(&t,&tmv);
#endif
    CmmString *f=cmm_to_string(fmt); SB b; sb_init(&b);
    php_date_fmt(f->data,f->len,&tmv,0,"UTC",(int64_t)t,&b);
    return sb_to_str(&b);
}


/* ======================================================================== */
/* Networking platform layer (Json / Socket / Http) — Part 3               */
/* winsock2 must precede any winsock.h; windows.h (via header) used         */
/* WIN32_LEAN_AND_MEAN so it did not pull winsock.h in, so this is safe.    */
/* ======================================================================== */
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef SOCKET sockfd_t;
  #define SOCK_BAD   INVALID_SOCKET
  #define CLOSESOCK(fd) closesocket(fd)
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netdb.h>
  #include <unistd.h>
  typedef int sockfd_t;
  #define SOCK_BAD   (-1)
  #define CLOSESOCK(fd) close(fd)
#endif

#ifdef CMM_HAVE_TLS
  #include "mbedtls/net_sockets.h"
  #include "mbedtls/ssl.h"
  #include "mbedtls/entropy.h"
  #include "mbedtls/ctr_drbg.h"
  #include "mbedtls/x509_crt.h"
  #include "mbedtls/sha256.h"
  #include "mbedtls/md.h"
#ifndef CMM_TLS_NO_VERIFY
  #include "cmm_ca_certs.h"
#endif
  static int g_tls_ready = 0;
  static mbedtls_entropy_context  g_tls_entropy;
  static mbedtls_ctr_drbg_context g_tls_drbg;
  static mbedtls_x509_crt         g_tls_ca;
  static mbedtls_ssl_config       g_tls_conf;
  static void cmm_tls_init(void){
      if (g_tls_ready) return;
      mbedtls_entropy_init(&g_tls_entropy);
      mbedtls_ctr_drbg_init(&g_tls_drbg);
      mbedtls_x509_crt_init(&g_tls_ca);
      mbedtls_ssl_config_init(&g_tls_conf);
      mbedtls_ctr_drbg_seed(&g_tls_drbg, mbedtls_entropy_func, &g_tls_entropy,
                            (const unsigned char*)"cmm-tls", 7);
      mbedtls_ssl_config_defaults(&g_tls_conf, MBEDTLS_SSL_IS_CLIENT,
                                  MBEDTLS_SSL_TRANSPORT_STREAM,
                                  MBEDTLS_SSL_PRESET_DEFAULT);
#ifdef CMM_TLS_NO_VERIFY
      /* Built with --no-verify: no CA bundle compiled in, and the peer
       * certificate is NOT checked. Smallest binary; use only when the
       * transport is already trusted or verification happens elsewhere. */
      mbedtls_ssl_conf_authmode(&g_tls_conf, MBEDTLS_SSL_VERIFY_NONE);
#else
      /* Bundled Mozilla CA roots (DER) -> real certificate verification,
       * no external file needed (the roots are compiled into the binary). */
      for (unsigned _i = 0; _i < CMM_CA_DER_CNT; _i++)
          mbedtls_x509_crt_parse_der(&g_tls_ca,
              CMM_CA_DER + CMM_CA_DER_OFF[_i], CMM_CA_DER_LEN[_i]);
      int insecure = (getenv("CMMC_TLS_INSECURE") != NULL);
      mbedtls_ssl_conf_authmode(&g_tls_conf,
          insecure ? MBEDTLS_SSL_VERIFY_NONE : MBEDTLS_SSL_VERIFY_REQUIRED);
      mbedtls_ssl_conf_ca_chain(&g_tls_conf, &g_tls_ca, NULL);
#endif
      mbedtls_ssl_conf_rng(&g_tls_conf, mbedtls_ctr_drbg_random, &g_tls_drbg);
      g_tls_ready = 1;
  }
#endif

/* ---- Json -------------------------------------------------------------- */
/* ---- Json: high-performance encode/decode ----------------------------- */
static unsigned char JSON_ESC[256];
static int JSON_TBL=0;
static void json_tbl(void){ if(JSON_TBL) return; for(int i=0;i<32;i++) JSON_ESC[i]=1; JSON_ESC[(unsigned)'"']=1; JSON_ESC[(unsigned)'\\']=1; JSON_TBL=1; }

static void sb_i64(SB *b, int64_t x){
    char t[24]; int i=24; int neg=(x<0);
    uint64_t u = neg ? (uint64_t)(-(x+1))+1u : (uint64_t)x;
    do { t[--i]=(char)('0'+(u%10u)); u/=10u; } while(u);
    if(neg) t[--i]='-';
    sb_putn(b,t+i,(size_t)(24-i));
}
static void sb_dbl(SB *b, double x){
    char t[40];
    if(x!=x){ sb_putn(b,"null",4); return; }          /* NaN -> null */
    if(x==(double)(int64_t)x && x<9.2e18 && x>-9.2e18){ sb_i64(b,(int64_t)x); return; }
    int prec; for(prec=15; prec<17; prec++){ snprintf(t,sizeof t,"%.*g",prec,x); if(strtod(t,NULL)==x) break; }
    if(prec>=17) snprintf(t,sizeof t,"%.17g",x);
    sb_put(b,t);
}
/* write a JSON string literal, bulk-copying runs that need no escaping */
static void json_str(SB *b, const char *p, size_t n){
    sb_putn(b,"\"",1);
    size_t i=0;
    while(i<n){
        size_t s=i;
        while(i<n && !JSON_ESC[(unsigned char)p[i]]) i++;
        if(i>s) sb_putn(b,p+s,i-s);
        if(i<n){
            unsigned char c=(unsigned char)p[i++];
            switch(c){
                case '"':  sb_putn(b,"\\\"",2); break;
                case '\\': sb_putn(b,"\\\\",2); break;
                case '\n': sb_putn(b,"\\n",2);  break;
                case '\r': sb_putn(b,"\\r",2);  break;
                case '\t': sb_putn(b,"\\t",2);  break;
                case '\b': sb_putn(b,"\\b",2);  break;
                case '\f': sb_putn(b,"\\f",2);  break;
                default: { static const char hx[]="0123456789abcdef";
                           char u[6]={'\\','u','0','0',hx[c>>4],hx[c&0xF]};
                           sb_putn(b,u,6); }
            }
        }
    }
    sb_putn(b,"\"",1);
}
static void json_render(SB *b, CmmValue v){
    switch (v.tag){
        case CV_EMPTY:  sb_putn(b,"null",4); break;
        case CV_INT:    sb_i64(b,v.i); break;
        case CV_FLOAT:  sb_dbl(b,v.f); break;
        case CV_BOOL:   if(v.b) sb_putn(b,"true",4); else sb_putn(b,"false",5); break;
        case CV_STRING: if(v.s) json_str(b,v.s->data,v.s->len); else sb_putn(b,"\"\"",2); break;
        case CV_LIST: {
            sb_putn(b,"[",1);
            if(v.list) for(size_t k=0;k<v.list->len;k++){ if(k) sb_putn(b,",",1); json_render(b,v.list->items[k]); }
            sb_putn(b,"]",1); break;
        }
        case CV_DICT: {
            sb_putn(b,"{",1);
            if(v.dict) for(size_t k=0;k<v.dict->len;k++){
                if(k) sb_putn(b,",",1);
                json_str(b,v.dict->entries[k].key->data,v.dict->entries[k].key->len);
                sb_putn(b,":",1);
                json_render(b,v.dict->entries[k].val);
            }
            sb_putn(b,"}",1); break;
        }
        default: sb_putn(b,"null",4); break;
    }
}
CmmValue cmm_json_encode(CmmValue v){ json_tbl(); SB b; sb_init(&b); json_render(&b,v); return sb_to_str(&b); }

static void json_indent(SB *b, int d){ for(int i=0;i<d;i++) sb_putn(b,"  ",2); }
static void json_pretty(SB *b, CmmValue v, int depth){
    switch(v.tag){
        case CV_LIST: {
            if(!v.list || v.list->len==0){ sb_putn(b,"[]",2); break; }
            sb_putn(b,"[\n",2);
            for(size_t k=0;k<v.list->len;k++){ if(k) sb_putn(b,",\n",2); json_indent(b,depth+1); json_pretty(b,v.list->items[k],depth+1); }
            sb_putn(b,"\n",1); json_indent(b,depth); sb_putn(b,"]",1); break;
        }
        case CV_DICT: {
            if(!v.dict || v.dict->len==0){ sb_putn(b,"{}",2); break; }
            sb_putn(b,"{\n",2);
            for(size_t k=0;k<v.dict->len;k++){ if(k) sb_putn(b,",\n",2); json_indent(b,depth+1);
                json_str(b,v.dict->entries[k].key->data,v.dict->entries[k].key->len); sb_putn(b,": ",2);
                json_pretty(b,v.dict->entries[k].val,depth+1); }
            sb_putn(b,"\n",1); json_indent(b,depth); sb_putn(b,"}",1); break;
        }
        default: json_render(b,v);
    }
}
CmmValue cmm_json_pretty(CmmValue v){ json_tbl(); SB b; sb_init(&b); json_pretty(&b,v,0); return sb_to_str(&b); }

/* ---- parser ---- */
typedef struct { const char *p; const char *end; } JP;
static void jp_ws(JP *j){ const char *p=j->p, *e=j->end;
    while(p<e){ char c=*p; if(c==' '||c=='\t'||c=='\n'||c=='\r') p++; else break; } j->p=p; }
static unsigned hex4(const char **pp, const char *end){
    unsigned cp=0; const char *p=*pp;
    for(int k=0;k<4 && p<end;k++){ char h=*p++; cp<<=4;
        if(h>='0'&&h<='9') cp|=(unsigned)(h-'0');
        else if(h>='a'&&h<='f') cp|=(unsigned)(h-'a'+10);
        else if(h>='A'&&h<='F') cp|=(unsigned)(h-'A'+10); }
    *pp=p; return cp;
}
static CmmValue jp_string(JP *j){
    const char *p=j->p+1, *e=j->end, *start=p;
    while(p<e){ unsigned char c=(unsigned char)*p; if(c=='"'||c=='\\') break; p++; }
    if(p<e && *p=='"'){ CmmValue r=cmm_str_n(start,(size_t)(p-start)); j->p=p+1; return r; }
    /* escaped path */
    SB b; sb_init(&b); p=start;
    while(p<e){
        const char *run=p;
        while(p<e && *p!='"' && *p!='\\') p++;
        if(p>run) sb_putn(&b,run,(size_t)(p-run));
        if(p>=e || *p=='"') break;
        p++; if(p>=e) break; char esc=*p++;
        switch(esc){
            case 'n': sb_putn(&b,"\n",1); break; case 't': sb_putn(&b,"\t",1); break;
            case 'r': sb_putn(&b,"\r",1); break; case 'b': sb_putn(&b,"\b",1); break;
            case 'f': sb_putn(&b,"\f",1); break; case '/': sb_putn(&b,"/",1); break;
            case '"': sb_putn(&b,"\"",1); break; case '\\': sb_putn(&b,"\\",1); break;
            case 'u': {
                unsigned cp=hex4(&p,e);
                if(cp>=0xD800&&cp<=0xDBFF && p+2<=e && p[0]=='\\'&&p[1]=='u'){ p+=2; unsigned lo=hex4(&p,e);
                    cp=0x10000u+((cp-0xD800u)<<10)+(lo-0xDC00u); }
                char u[4];
                if(cp<0x80){ u[0]=(char)cp; sb_putn(&b,u,1); }
                else if(cp<0x800){ u[0]=(char)(0xC0|(cp>>6)); u[1]=(char)(0x80|(cp&0x3F)); sb_putn(&b,u,2); }
                else if(cp<0x10000){ u[0]=(char)(0xE0|(cp>>12)); u[1]=(char)(0x80|((cp>>6)&0x3F)); u[2]=(char)(0x80|(cp&0x3F)); sb_putn(&b,u,3); }
                else { u[0]=(char)(0xF0|(cp>>18)); u[1]=(char)(0x80|((cp>>12)&0x3F)); u[2]=(char)(0x80|((cp>>6)&0x3F)); u[3]=(char)(0x80|(cp&0x3F)); sb_putn(&b,u,4); }
                break;
            }
            default: { char c=esc; sb_putn(&b,&c,1); }
        }
    }
    if(p<e && *p=='"') p++;
    j->p=p; return sb_to_str(&b);
}
static CmmValue jp_number(JP *j){
    const char *p=j->p, *e=j->end, *start=p; int isf=0;
    if(p<e && *p=='-') p++;
    while(p<e && *p>='0'&&*p<='9') p++;
    if(p<e && *p=='.'){ isf=1; p++; while(p<e && *p>='0'&&*p<='9') p++; }
    if(p<e && (*p=='e'||*p=='E')){ isf=1; p++; if(p<e&&(*p=='+'||*p=='-'))p++; while(p<e && *p>='0'&&*p<='9') p++; }
    j->p=p;
    if(!isf){
        const char *q=start; int neg=0; if(*q=='-'){neg=1;q++;}
        int64_t val=0; while(q<p){ val=val*10+(int64_t)(*q-'0'); q++; }
        return cmm_int(neg?-val:val);
    }
    char tmp[64]; size_t n=(size_t)(p-start); if(n>=sizeof tmp)n=sizeof tmp-1; memcpy(tmp,start,n); tmp[n]=0;
    return cmm_float(strtod(tmp,NULL));
}
static CmmValue jp_value(JP *j){
    jp_ws(j);
    if(j->p>=j->end) return cmm_empty();
    char c=*j->p;
    switch(c){
        case '"': return jp_string(j);
        case '{': {
            j->p++; CmmValue d=cmm_new_dict(); jp_ws(j);
            if(j->p<j->end && *j->p=='}'){ j->p++; return d; }
            for(;;){
                jp_ws(j);
                if(j->p>=j->end || *j->p!='"') break;
                CmmValue key=jp_string(j); jp_ws(j);
                if(j->p<j->end && *j->p==':') j->p++;
                CmmValue val=jp_value(j);
                dict_put_key(d.dict,key.s,val);
                jp_ws(j);
                if(j->p<j->end && *j->p==','){ j->p++; continue; }
                if(j->p<j->end && *j->p=='}') j->p++;
                break;
            }
            return d;
        }
        case '[': {
            j->p++; CmmValue a=cmm_new_list(); jp_ws(j);
            if(j->p<j->end && *j->p==']'){ j->p++; return a; }
            for(;;){
                CmmValue v=jp_value(j); list_push(a.list,v); jp_ws(j);
                if(j->p<j->end && *j->p==','){ j->p++; continue; }
                if(j->p<j->end && *j->p==']') j->p++;
                break;
            }
            return a;
        }
        case 't': if(j->end-j->p>=4 && !memcmp(j->p,"true",4)){ j->p+=4; return cmm_bool(1);} j->p++; return cmm_empty();
        case 'f': if(j->end-j->p>=5 && !memcmp(j->p,"false",5)){ j->p+=5; return cmm_bool(0);} j->p++; return cmm_empty();
        case 'n': if(j->end-j->p>=4 && !memcmp(j->p,"null",4)){ j->p+=4; return cmm_empty();} j->p++; return cmm_empty();
        default:
            if(c=='-'||(c>='0'&&c<='9')) return jp_number(j);
            j->p++; return cmm_empty();
    }
}
CmmValue cmm_json_decode(CmmValue s){
    CmmString *str=cmm_to_string(s);
    JP j; j.p=str->data; j.end=str->data+str->len;
    return jp_value(&j);
}

/* ---- Sockets ----------------------------------------------------------- */
static sockfd_t tcp_connect(const char *host, int port){
    struct addrinfo hints, *res=NULL; memset(&hints,0,sizeof hints);
    hints.ai_family=AF_UNSPEC; hints.ai_socktype=SOCK_STREAM;
    char portstr[16]; snprintf(portstr,sizeof portstr,"%d",port);
    if (getaddrinfo(host,portstr,&hints,&res)!=0 || !res) return SOCK_BAD;
    sockfd_t fd = socket(res->ai_family,res->ai_socktype,res->ai_protocol);
    if (fd==SOCK_BAD){ freeaddrinfo(res); return SOCK_BAD; }
    if (connect(fd,res->ai_addr,(int)res->ai_addrlen)!=0){
        freeaddrinfo(res); CLOSESOCK(fd); return SOCK_BAD;
    }
    freeaddrinfo(res);
    return fd;
}
static CmmValue make_socket(sockfd_t fd){
    CmmSocket *sk=(CmmSocket*)cmm_alloc(sizeof(CmmSocket));
    sk->fd=(
#ifdef _WIN32
        uintptr_t
#else
        int
#endif
    )fd;
    sk->open=1;
    sk->arena=t_cur;
    CmmValue v; v.tag=CV_SOCKET; v.sock=sk; return v;
}
CmmValue cmm_socket_connect(CmmValue host, CmmValue port){
    const char *h = cmm_to_string(host)->data;
    int p = (port.tag==CV_INT) ? (int)port.i : atoi(cmm_to_string(port)->data);
    sockfd_t fd = tcp_connect(h, p);
    if (fd==SOCK_BAD) return cmm_empty();
    return make_socket(fd);
}
CmmValue cmm_socket_write(CmmValue sock, CmmValue data){
    if (sock.tag!=CV_SOCKET || !sock.sock || !sock.sock->open) return cmm_bool(0);
    CmmString *s = cmm_to_string(data);
    sockfd_t fd=(sockfd_t)sock.sock->fd;
    size_t sent=0;
    while (sent<s->len){
        int w=(int)send(fd, s->data+sent, (int)(s->len-sent), 0);
        if (w<=0) return cmm_bool(0);
        sent+=(size_t)w;
    }
    return cmm_bool(1);
}
CmmValue cmm_socket_read(CmmValue sock, CmmValue n){
    if (sock.tag!=CV_SOCKET || !sock.sock || !sock.sock->open) return cmm_str("");
    int want=(int)as_int(n); if (want<=0) want=4096;
    char *buf=(char*)cmm_alloc((size_t)want+1);
    int r=(int)recv((sockfd_t)sock.sock->fd, buf, want, 0);
    if (r<=0) return cmm_str("");
    return cmm_str_n(buf,(size_t)r);
}
CmmValue cmm_socket_readall(CmmValue sock){
    if (sock.tag!=CV_SOCKET || !sock.sock) return cmm_str("");
    sockfd_t fd=(sockfd_t)sock.sock->fd;
    SB b; sb_init(&b); char chunk[4096]; int r;
    while ((r=(int)recv(fd,chunk,sizeof chunk,0))>0) sb_putn(&b,chunk,(size_t)r);
    return sb_to_str(&b);
}
CmmValue cmm_socket_close(CmmValue sock){
    if (sock.tag==CV_SOCKET && sock.sock && sock.sock->open){
        CLOSESOCK((sockfd_t)sock.sock->fd);
        sock.sock->open=0;
    }
    return cmm_empty();
}

/* ---- Http (http:// and, when built with TLS, https://) ----------------- */
/* Parse scheme/host/port/path. Returns 1 ok; *https set for https scheme. */
static int url_split(const char *u, int *https, char *host, size_t hostsz,
                     int *port, const char **path){
    const char *hs;
    if (strncmp(u,"https://",8)==0){ *https=1; hs=u+8; *port=443; }
    else if (strncmp(u,"http://",7)==0){ *https=0; hs=u+7; *port=80; }
    else return 0;
    const char *slash=strchr(hs,'/');
    if (slash){ size_t hl=(size_t)(slash-hs); if(hl>=hostsz)hl=hostsz-1; memcpy(host,hs,hl); host[hl]=0; *path=slash; }
    else { snprintf(host,hostsz,"%s",hs); *path="/"; }
    char *colon=strchr(host,':'); if(colon){ *colon=0; *port=atoi(colon+1); }
    return 1;
}
static const char *mem_find(const char *h, size_t hn, const char *n, size_t nn){
    if (nn==0 || hn<nn) return NULL;
    for (size_t i=0;i+nn<=hn;i++) if (memcmp(h+i,n,nn)==0) return h+i;
    return NULL;
}
/* case-insensitive substring (for header names) */
static int ci_contains(const char *h, size_t hn, const char *needle){
    size_t nn=strlen(needle);
    if (hn<nn) return 0;
    for (size_t i=0;i+nn<=hn;i++){
        size_t j=0; for (;j<nn;j++){ if (tolower((unsigned char)h[i+j])!=tolower((unsigned char)needle[j])) break; }
        if (j==nn) return 1;
    }
    return 0;
}
/* Decode HTTP/1.1 chunked transfer-encoding into out. */
static void decode_chunked(const char *p, const char *end, SB *out){
    while (p < end){
        char *stop=NULL;
        long sz = strtol(p, &stop, 16);
        if (stop==p) break;
        p = stop;
        while (p<end && *p!='\n') p++;     /* skip to end of size line */
        if (p<end) p++;                    /* past '\n' */
        if (sz<=0) break;
        if (p+sz>end) sz = (long)(end-p);
        sb_putn(out, p, (size_t)sz);
        p += sz;
        while (p<end && (*p=='\r'||*p=='\n')) p++;  /* trailing CRLF */
    }
}
static CmmValue http_body(CmmValue resp){
    CmmString *r = cmm_to_string(resp);
    const char *p = r->data; size_t n = r->len;
    const char *sep = mem_find(p, n, "\r\n\r\n", 4);
    if (!sep) return resp;
    size_t headlen = (size_t)(sep - p);
    const char *body = sep + 4;
    size_t bodylen = n - (size_t)(body - p);
    if (ci_contains(p, headlen, "transfer-encoding: chunked")){
        SB b; sb_init(&b);
        decode_chunked(body, body + bodylen, &b);
        return sb_to_str(&b);
    }
    return cmm_str_n(body, bodylen);
}

/* Send a prebuilt request over plain TCP and read the whole response. */
static CmmValue http_exchange_plain(const char *host, int port,
                                     const char *req, size_t reqlen){
    sockfd_t fd = tcp_connect(host, port);
    if (fd==SOCK_BAD) return cmm_str("");
    size_t sent=0;
    while (sent<reqlen){ int w=(int)send(fd, req+sent, (int)(reqlen-sent), 0);
        if (w<=0){ CLOSESOCK(fd); return cmm_str(""); } sent+=(size_t)w; }
    SB b; sb_init(&b); char chunk[4096]; int r;
    while ((r=(int)recv(fd,chunk,sizeof chunk,0))>0) sb_putn(&b,chunk,(size_t)r);
    CLOSESOCK(fd);
    return sb_to_str(&b);
}

#ifdef CMM_HAVE_TLS
static CmmValue http_exchange_tls(const char *host, int port,
                                   const char *req, size_t reqlen){
    cmm_tls_init();
    sockfd_t fd = tcp_connect(host, port);
    if (fd==SOCK_BAD) return cmm_str("");
    mbedtls_ssl_context ssl; mbedtls_ssl_init(&ssl);
    mbedtls_net_context net; net.fd = (int)fd;
    if (mbedtls_ssl_setup(&ssl, &g_tls_conf)!=0){
        mbedtls_ssl_free(&ssl); CLOSESOCK(fd); return cmm_str(""); }
    mbedtls_ssl_set_hostname(&ssl, host);   /* SNI + verified against cert */
    mbedtls_ssl_set_bio(&ssl, &net, mbedtls_net_send, mbedtls_net_recv, NULL);
    int hs;
    while ((hs=mbedtls_ssl_handshake(&ssl))!=0){
        if (hs!=MBEDTLS_ERR_SSL_WANT_READ && hs!=MBEDTLS_ERR_SSL_WANT_WRITE){
            mbedtls_ssl_free(&ssl); CLOSESOCK(fd); return cmm_str(""); } }
    if (mbedtls_ssl_get_verify_result(&ssl)!=0 && getenv("CMMC_TLS_INSECURE")==NULL){
        mbedtls_ssl_free(&ssl); CLOSESOCK(fd); return cmm_str(""); }
    size_t sent=0;
    while (sent<reqlen){
        int w=mbedtls_ssl_write(&ssl,(const unsigned char*)req+sent,reqlen-sent);
        if (w<=0){ if(w==MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            mbedtls_ssl_free(&ssl); CLOSESOCK(fd); return cmm_str(""); }
        sent+=(size_t)w; }
    SB b; sb_init(&b); unsigned char chunk[4096]; int r;
    while ((r=mbedtls_ssl_read(&ssl,chunk,sizeof chunk))>0) sb_putn(&b,(char*)chunk,(size_t)r);
    mbedtls_ssl_close_notify(&ssl); mbedtls_ssl_free(&ssl); CLOSESOCK(fd);
    return sb_to_str(&b);
}
#endif

static CmmValue http_exchange(int https, const char *host, int port,
                               const char *req, size_t reqlen){
    if (https){
#ifdef CMM_HAVE_TLS
        return http_exchange_tls(host, port, req, reqlen);
#else
        return cmm_str("");   /* https requested but TLS not compiled in */
#endif
    }
    return http_exchange_plain(host, port, req, reqlen);
}

CmmValue cmm_http_get(CmmValue url){
    char host[256]; int port; int https; const char *path;
    if (!url_split(cmm_to_string(url)->data, &https, host, sizeof host, &port, &path))
        return cmm_str("");
    char req[1024];
    int n = snprintf(req,sizeof req,
        "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: lang/1.0\r\nConnection: close\r\n\r\n",
        path, host);
    CmmValue resp = http_exchange(https, host, port, req, (size_t)n);
    return http_body(resp);
}
CmmValue cmm_http_post(CmmValue url, CmmValue body){
    char host[256]; int port; int https; const char *path;
    if (!url_split(cmm_to_string(url)->data, &https, host, sizeof host, &port, &path))
        return cmm_str("");
    CmmString *b = cmm_to_string(body);
    SB req; sb_init(&req); char line[512];
    snprintf(line,sizeof line,
        "POST %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: lang/1.0\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n", path, host, b->len);
    sb_put(&req,line); sb_putn(&req,b->data,b->len);
    /* copy out of SB into a stable buffer for the exchange */
    size_t reqlen = req.len;
    char *buf = (char*)malloc(reqlen);
    memcpy(buf, req.p, reqlen); free(req.p);
    CmmValue resp = http_exchange(https, host, port, buf, reqlen);
    free(buf);
    return http_body(resp);
}

/* ======================================================================== */
/* System + process primitives (Sys namespace) and HTTP request with        */
/* custom method/headers. These are the irreducible syscalls the cmm-level   */
/* standard library is built on top of.                                      */
/* ======================================================================== */
#ifndef _WIN32
#  include <sys/wait.h>
#endif

CmmValue cmm_sys_exit(CmmValue code){
    fflush(stdout); fflush(stderr);
    exit((int)as_int(code));
    return cmm_empty();              /* unreachable */
}

CmmValue cmm_sys_run(CmmValue cmd){  /* run a shell command, capture stdout */
    const char *c = cmm_to_string(cmd)->data;
#ifdef _WIN32
    FILE *p=_popen(c,"r");
#else
    FILE *p=popen(c,"r");
#endif
    if(!p) return cmm_str("");
    SB b; sb_init(&b); char buf[4096]; size_t r;
    while((r=fread(buf,1,sizeof buf,p))>0) sb_putn(&b,buf,r);
#ifdef _WIN32
    _pclose(p);
#else
    pclose(p);
#endif
    return sb_to_str(&b);
}

CmmValue cmm_sys_shell(CmmValue cmd){ /* run inheriting stdio; return code   */
    int rc=system(cmm_to_string(cmd)->data);
#ifndef _WIN32
    if(rc!=-1 && WIFEXITED(rc)) rc=WEXITSTATUS(rc);
#endif
    return cmm_int(rc);
}

CmmValue cmm_sys_env(CmmValue name){
    const char *v=getenv(cmm_to_string(name)->data);
    return cmm_str(v?v:"");
}

/* command-line arguments: argv captured by the generated main() */
static int    cmm_g_argc = 0;
static char **cmm_g_argv = NULL;
void cmm_set_args(int argc, char **argv){ cmm_g_argc = argc; cmm_g_argv = argv; }
CmmValue cmm_sys_args(void){
    CmmValue lst = cmm_new_list();
    for (int i=0; i<cmm_g_argc; i++){
        const char *a = cmm_g_argv[i] ? cmm_g_argv[i] : "";
        list_push(lst.list, cmm_str(a));
    }
    return lst;
}

/* working directory */
#ifdef _WIN32
  #include <direct.h>
  #define CMM_GETCWD _getcwd
  #define CMM_CHDIR  _chdir
#else
  #define CMM_GETCWD getcwd
  #define CMM_CHDIR  chdir
#endif
CmmValue cmm_sys_os(void){
#if defined(_WIN32)
    return cmm_str("windows");
#elif defined(__APPLE__)
    return cmm_str("macos");
#elif defined(__linux__)
    return cmm_str("linux");
#else
    return cmm_str("unknown");
#endif
}
CmmValue cmm_sys_arch(void){
#if defined(__x86_64__)||defined(_M_X64)
    return cmm_str("x86_64");
#elif defined(__aarch64__)||defined(_M_ARM64)
    return cmm_str("aarch64");
#elif defined(__arm__)||defined(_M_ARM)
    return cmm_str("arm");
#elif defined(__i386__)||defined(_M_IX86)
    return cmm_str("x86");
#else
    return cmm_str("unknown");
#endif
}
CmmValue cmm_sys_platform(void){
    CmmValue os=cmm_sys_os(), ar=cmm_sys_arch();
    CmmString*o=cmm_to_string(os),*a=cmm_to_string(ar);
    CmmValue r=cmm_str_n(NULL,o->len+1+a->len);
    memcpy(r.s->data,o->data,o->len); r.s->data[o->len]='-';
    memcpy(r.s->data+o->len+1,a->data,a->len);
    return r;
}

CmmValue cmm_sys_cwd(void){
    char buf[4096];
    if (CMM_GETCWD(buf, sizeof buf)) return cmm_str(buf);
    return cmm_str("");
}
CmmValue cmm_sys_chdir(CmmValue path){
    return cmm_bool(CMM_CHDIR(cmm_to_string(path)->data) == 0);
}

/* method, url, headers (List[String] of "Key: Value"), body (""=none) */
CmmValue cmm_http_request(CmmValue method, CmmValue url, CmmValue headers, CmmValue body){
    char host[256]; int port; int https; const char *path;
    if(!url_split(cmm_to_string(url)->data,&https,host,sizeof host,&port,&path))
        return cmm_str("");
    const char *m=cmm_to_string(method)->data;
    CmmString *b=cmm_to_string(body);
    SB req; sb_init(&req); char line[1024];
    snprintf(line,sizeof line,
        "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: cmm/1.0\r\nConnection: close\r\n",
        m,path,host);
    sb_put(&req,line);
    if(headers.tag==CV_LIST && headers.list){
        for(size_t i=0;i<headers.list->len;i++){
            CmmString *h=cmm_to_string(headers.list->items[i]);
            sb_putn(&req,h->data,h->len); sb_put(&req,"\r\n");
        }
    }
    if(b->len>0){ snprintf(line,sizeof line,"Content-Length: %zu\r\n",b->len); sb_put(&req,line); }
    sb_put(&req,"\r\n");
    if(b->len>0) sb_putn(&req,b->data,b->len);
    size_t reqlen=req.len;
    char *buf=(char*)malloc(reqlen?reqlen:1);
    memcpy(buf,req.p,reqlen); free(req.p);
    CmmValue resp=http_exchange(https,host,port,buf,reqlen);
    free(buf);
    return http_body(resp);
}

CmmValue cmm_file_delete(CmmValue path){
    int rc = remove(cmm_to_string(path)->data);
    return cmm_bool(rc==0);
}

/* ======================================================================== */
/* Zip: build a ZIP archive (STORE method) from a list of entries.           */
/* Each entry is a dict { name, content, mode?|exec? }. Unix permissions are  */
/* stored in the external attributes so an extracted `bootstrap` keeps its    */
/* executable bit -- which AWS Lambda requires. No compression dependency.    */
/* ======================================================================== */
static uint32_t crc32_buf(const unsigned char *p, size_t n){
    static uint32_t T[256]; static int init=0;
    if(!init){ for(uint32_t i=0;i<256;i++){ uint32_t c=i;
        for(int k=0;k<8;k++) c=(c&1)?(0xEDB88320u^(c>>1)):(c>>1); T[i]=c; } init=1; }
    uint32_t c=0xFFFFFFFFu;
    for(size_t i=0;i<n;i++) c=T[(c^p[i])&0xFFu]^(c>>8);
    return c^0xFFFFFFFFu;
}
static void zip_le16(SB*b,unsigned v){ char t[2]={(char)(v&0xFF),(char)((v>>8)&0xFF)}; sb_putn(b,t,2); }
static void zip_le32(SB*b,uint32_t v){
    char t[4]={(char)(v&0xFF),(char)((v>>8)&0xFF),(char)((v>>16)&0xFF),(char)((v>>24)&0xFF)};
    sb_putn(b,t,4);
}

/* ====================================================================
   Embedded DEFLATE inflate (puff.c, Copyright (C) 2002-2013 Mark Adler,
   public-domain reference inflater) — used by Zip.unzip below.
   ==================================================================== */
#include <setjmp.h>
#ifndef NIL
#define NIL ((unsigned char *)0)
#endif
#define local static            /* for local function definitions */

/*
 * Maximums for allocations and loops.  It is not useful to change these --
 * they are fixed by the deflate format.
 */
#define MAXBITS 15              /* maximum bits in a code */
#define MAXLCODES 286           /* maximum number of literal/length codes */
#define MAXDCODES 30            /* maximum number of distance codes */
#define MAXCODES (MAXLCODES+MAXDCODES)  /* maximum codes lengths to read */
#define FIXLCODES 288           /* number of fixed literal/length codes */

/* input and output state */
struct state {
    /* output state */
    unsigned char *out;         /* output buffer */
    unsigned long outlen;       /* available space at out */
    unsigned long outcnt;       /* bytes written to out so far */

    /* input state */
    const unsigned char *in;    /* input buffer */
    unsigned long inlen;        /* available input at in */
    unsigned long incnt;        /* bytes read so far */
    int bitbuf;                 /* bit buffer */
    int bitcnt;                 /* number of bits in bit buffer */

    /* input limit error return state for bits() and decode() */
    jmp_buf env;
};

/*
 * Return need bits from the input stream.  This always leaves less than
 * eight bits in the buffer.  bits() works properly for need == 0.
 *
 * Format notes:
 *
 * - Bits are stored in bytes from the least significant bit to the most
 *   significant bit.  Therefore bits are dropped from the bottom of the bit
 *   buffer, using shift right, and new bytes are appended to the top of the
 *   bit buffer, using shift left.
 */
local int bits(struct state *s, int need)
{
    long val;           /* bit accumulator (can use up to 20 bits) */

    /* load at least need bits into val */
    val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->incnt == s->inlen)
            longjmp(s->env, 1);         /* out of input */
        val |= (long)(s->in[s->incnt++]) << s->bitcnt;  /* load eight bits */
        s->bitcnt += 8;
    }

    /* drop need bits and update buffer, always zero to seven bits left */
    s->bitbuf = (int)(val >> need);
    s->bitcnt -= need;

    /* return need bits, zeroing the bits above that */
    return (int)(val & ((1L << need) - 1));
}

/*
 * Process a stored block.
 *
 * Format notes:
 *
 * - After the two-bit stored block type (00), the stored block length and
 *   stored bytes are byte-aligned for fast copying.  Therefore any leftover
 *   bits in the byte that has the last bit of the type, as many as seven, are
 *   discarded.  The value of the discarded bits are not defined and should not
 *   be checked against any expectation.
 *
 * - The second inverted copy of the stored block length does not have to be
 *   checked, but it's probably a good idea to do so anyway.
 *
 * - A stored block can have zero length.  This is sometimes used to byte-align
 *   subsets of the compressed data for random access or partial recovery.
 */
local int stored(struct state *s)
{
    unsigned len;       /* length of stored block */

    /* discard leftover bits from current byte (assumes s->bitcnt < 8) */
    s->bitbuf = 0;
    s->bitcnt = 0;

    /* get length and check against its one's complement */
    if (s->incnt + 4 > s->inlen)
        return 2;                               /* not enough input */
    len = s->in[s->incnt++];
    len |= s->in[s->incnt++] << 8;
    if (s->in[s->incnt++] != (~len & 0xff) ||
        s->in[s->incnt++] != ((~len >> 8) & 0xff))
        return -2;                              /* didn't match complement! */

    /* copy len bytes from in to out */
    if (s->incnt + len > s->inlen)
        return 2;                               /* not enough input */
    if (s->out != NIL) {
        if (s->outcnt + len > s->outlen)
            return 1;                           /* not enough output space */
        while (len--)
            s->out[s->outcnt++] = s->in[s->incnt++];
    }
    else {                                      /* just scanning */
        s->outcnt += len;
        s->incnt += len;
    }

    /* done with a valid stored block */
    return 0;
}

/*
 * Huffman code decoding tables.  count[1..MAXBITS] is the number of symbols of
 * each length, which for a canonical code are stepped through in order.
 * symbol[] are the symbol values in canonical order, where the number of
 * entries is the sum of the counts in count[].  The decoding process can be
 * seen in the function decode() below.
 */
struct huffman {
    short *count;       /* number of symbols of each length */
    short *symbol;      /* canonically ordered symbols */
};

/*
 * Decode a code from the stream s using huffman table h.  Return the symbol or
 * a negative value if there is an error.  If all of the lengths are zero, i.e.
 * an empty code, or if the code is incomplete and an invalid code is received,
 * then -10 is returned after reading MAXBITS bits.
 *
 * Format notes:
 *
 * - The codes as stored in the compressed data are bit-reversed relative to
 *   a simple integer ordering of codes of the same lengths.  Hence below the
 *   bits are pulled from the compressed data one at a time and used to
 *   build the code value reversed from what is in the stream in order to
 *   permit simple integer comparisons for decoding.  A table-based decoding
 *   scheme (as used in zlib) does not need to do this reversal.
 *
 * - The first code for the shortest length is all zeros.  Subsequent codes of
 *   the same length are simply integer increments of the previous code.  When
 *   moving up a length, a zero bit is appended to the code.  For a complete
 *   code, the last code of the longest length will be all ones.
 *
 * - Incomplete codes are handled by this decoder, since they are permitted
 *   in the deflate format.  See the format notes for fixed() and dynamic().
 */
#ifdef SLOW
local int decode(struct state *s, const struct huffman *h)
{
    int len;            /* current number of bits in code */
    int code;           /* len bits being decoded */
    int first;          /* first code of length len */
    int count;          /* number of codes of length len */
    int index;          /* index of first code of length len in symbol table */

    code = first = index = 0;
    for (len = 1; len <= MAXBITS; len++) {
        code |= bits(s, 1);             /* get next bit */
        count = h->count[len];
        if (code - count < first)       /* if length len, return symbol */
            return h->symbol[index + (code - first)];
        index += count;                 /* else update for next length */
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -10;                         /* ran out of codes */
}

/*
 * A faster version of decode() for real applications of this code.   It's not
 * as readable, but it makes puff() twice as fast.  And it only makes the code
 * a few percent larger.
 */
#else /* !SLOW */
local int decode(struct state *s, const struct huffman *h)
{
    int len;            /* current number of bits in code */
    int code;           /* len bits being decoded */
    int first;          /* first code of length len */
    int count;          /* number of codes of length len */
    int index;          /* index of first code of length len in symbol table */
    int bitbuf;         /* bits from stream */
    int left;           /* bits left in next or left to process */
    short *next;        /* next number of codes */

    bitbuf = s->bitbuf;
    left = s->bitcnt;
    code = first = index = 0;
    len = 1;
    next = h->count + 1;
    while (1) {
        while (left--) {
            code |= bitbuf & 1;
            bitbuf >>= 1;
            count = *next++;
            if (code - count < first) { /* if length len, return symbol */
                s->bitbuf = bitbuf;
                s->bitcnt = (s->bitcnt - len) & 7;
                return h->symbol[index + (code - first)];
            }
            index += count;             /* else update for next length */
            first += count;
            first <<= 1;
            code <<= 1;
            len++;
        }
        left = (MAXBITS+1) - len;
        if (left == 0)
            break;
        if (s->incnt == s->inlen)
            longjmp(s->env, 1);         /* out of input */
        bitbuf = s->in[s->incnt++];
        if (left > 8)
            left = 8;
    }
    return -10;                         /* ran out of codes */
}
#endif /* SLOW */

/*
 * Given the list of code lengths length[0..n-1] representing a canonical
 * Huffman code for n symbols, construct the tables required to decode those
 * codes.  Those tables are the number of codes of each length, and the symbols
 * sorted by length, retaining their original order within each length.  The
 * return value is zero for a complete code set, negative for an over-
 * subscribed code set, and positive for an incomplete code set.  The tables
 * can be used if the return value is zero or positive, but they cannot be used
 * if the return value is negative.  If the return value is zero, it is not
 * possible for decode() using that table to return an error--any stream of
 * enough bits will resolve to a symbol.  If the return value is positive, then
 * it is possible for decode() using that table to return an error for received
 * codes past the end of the incomplete lengths.
 *
 * Not used by decode(), but used for error checking, h->count[0] is the number
 * of the n symbols not in the code.  So n - h->count[0] is the number of
 * codes.  This is useful for checking for incomplete codes that have more than
 * one symbol, which is an error in a dynamic block.
 *
 * Assumption: for all i in 0..n-1, 0 <= length[i] <= MAXBITS
 * This is assured by the construction of the length arrays in dynamic() and
 * fixed() and is not verified by construct().
 *
 * Format notes:
 *
 * - Permitted and expected examples of incomplete codes are one of the fixed
 *   codes and any code with a single symbol which in deflate is coded as one
 *   bit instead of zero bits.  See the format notes for fixed() and dynamic().
 *
 * - Within a given code length, the symbols are kept in ascending order for
 *   the code bits definition.
 */
local int construct(struct huffman *h, const short *length, int n)
{
    int symbol;         /* current symbol when stepping through length[] */
    int len;            /* current length when stepping through h->count[] */
    int left;           /* number of possible codes left of current length */
    short offs[MAXBITS+1];      /* offsets in symbol table for each length */

    /* count number of codes of each length */
    for (len = 0; len <= MAXBITS; len++)
        h->count[len] = 0;
    for (symbol = 0; symbol < n; symbol++)
        (h->count[length[symbol]])++;   /* assumes lengths are within bounds */
    if (h->count[0] == n)               /* no codes! */
        return 0;                       /* complete, but decode() will fail */

    /* check for an over-subscribed or incomplete set of lengths */
    left = 1;                           /* one possible code of zero length */
    for (len = 1; len <= MAXBITS; len++) {
        left <<= 1;                     /* one more bit, double codes left */
        left -= h->count[len];          /* deduct count from possible codes */
        if (left < 0)
            return left;                /* over-subscribed--return negative */
    }                                   /* left > 0 means incomplete */

    /* generate offsets into symbol table for each length for sorting */
    offs[1] = 0;
    for (len = 1; len < MAXBITS; len++)
        offs[len + 1] = offs[len] + h->count[len];

    /*
     * put symbols in table sorted by length, by symbol order within each
     * length
     */
    for (symbol = 0; symbol < n; symbol++)
        if (length[symbol] != 0)
            h->symbol[offs[length[symbol]]++] = symbol;

    /* return zero for complete set, positive for incomplete set */
    return left;
}

/*
 * Decode literal/length and distance codes until an end-of-block code.
 *
 * Format notes:
 *
 * - Compressed data that is after the block type if fixed or after the code
 *   description if dynamic is a combination of literals and length/distance
 *   pairs terminated by and end-of-block code.  Literals are simply Huffman
 *   coded bytes.  A length/distance pair is a coded length followed by a
 *   coded distance to represent a string that occurs earlier in the
 *   uncompressed data that occurs again at the current location.
 *
 * - Literals, lengths, and the end-of-block code are combined into a single
 *   code of up to 286 symbols.  They are 256 literals (0..255), 29 length
 *   symbols (257..285), and the end-of-block symbol (256).
 *
 * - There are 256 possible lengths (3..258), and so 29 symbols are not enough
 *   to represent all of those.  Lengths 3..10 and 258 are in fact represented
 *   by just a length symbol.  Lengths 11..257 are represented as a symbol and
 *   some number of extra bits that are added as an integer to the base length
 *   of the length symbol.  The number of extra bits is determined by the base
 *   length symbol.  These are in the static arrays below, lens[] for the base
 *   lengths and lext[] for the corresponding number of extra bits.
 *
 * - The reason that 258 gets its own symbol is that the longest length is used
 *   often in highly redundant files.  Note that 258 can also be coded as the
 *   base value 227 plus the maximum extra value of 31.  While a good deflate
 *   should never do this, it is not an error, and should be decoded properly.
 *
 * - If a length is decoded, including its extra bits if any, then it is
 *   followed a distance code.  There are up to 30 distance symbols.  Again
 *   there are many more possible distances (1..32768), so extra bits are added
 *   to a base value represented by the symbol.  The distances 1..4 get their
 *   own symbol, but the rest require extra bits.  The base distances and
 *   corresponding number of extra bits are below in the static arrays dist[]
 *   and dext[].
 *
 * - Literal bytes are simply written to the output.  A length/distance pair is
 *   an instruction to copy previously uncompressed bytes to the output.  The
 *   copy is from distance bytes back in the output stream, copying for length
 *   bytes.
 *
 * - Distances pointing before the beginning of the output data are not
 *   permitted.
 *
 * - Overlapped copies, where the length is greater than the distance, are
 *   allowed and common.  For example, a distance of one and a length of 258
 *   simply copies the last byte 258 times.  A distance of four and a length of
 *   twelve copies the last four bytes three times.  A simple forward copy
 *   ignoring whether the length is greater than the distance or not implements
 *   this correctly.  You should not use memcpy() since its behavior is not
 *   defined for overlapped arrays.  You should not use memmove() or bcopy()
 *   since though their behavior -is- defined for overlapping arrays, it is
 *   defined to do the wrong thing in this case.
 */
local int codes(struct state *s,
                const struct huffman *lencode,
                const struct huffman *distcode)
{
    int symbol;         /* decoded symbol */
    int len;            /* length for copy */
    unsigned dist;      /* distance for copy */
    static const short lens[29] = { /* Size base for length codes 257..285 */
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const short lext[29] = { /* Extra bits for length codes 257..285 */
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const short dists[30] = { /* Offset base for distance codes 0..29 */
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
        8193, 12289, 16385, 24577};
    static const short dext[30] = { /* Extra bits for distance codes 0..29 */
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11,
        12, 12, 13, 13};

    /* decode literals and length/distance pairs */
    do {
        symbol = decode(s, lencode);
        if (symbol < 0)
            return symbol;              /* invalid symbol */
        if (symbol < 256) {             /* literal: symbol is the byte */
            /* write out the literal */
            if (s->out != NIL) {
                if (s->outcnt == s->outlen)
                    return 1;
                s->out[s->outcnt] = symbol;
            }
            s->outcnt++;
        }
        else if (symbol > 256) {        /* length */
            /* get and compute length */
            symbol -= 257;
            if (symbol >= 29)
                return -10;             /* invalid fixed code */
            len = lens[symbol] + bits(s, lext[symbol]);

            /* get and check distance */
            symbol = decode(s, distcode);
            if (symbol < 0)
                return symbol;          /* invalid symbol */
            dist = dists[symbol] + bits(s, dext[symbol]);
#ifndef INFLATE_ALLOW_INVALID_DISTANCE_TOOFAR_ARRR
            if (dist > s->outcnt)
                return -11;     /* distance too far back */
#endif

            /* copy length bytes from distance bytes back */
            if (s->out != NIL) {
                if (s->outcnt + len > s->outlen)
                    return 1;
                while (len--) {
                    s->out[s->outcnt] =
#ifdef INFLATE_ALLOW_INVALID_DISTANCE_TOOFAR_ARRR
                        dist > s->outcnt ?
                            0 :
#endif
                            s->out[s->outcnt - dist];
                    s->outcnt++;
                }
            }
            else
                s->outcnt += len;
        }
    } while (symbol != 256);            /* end of block symbol */

    /* done with a valid fixed or dynamic block */
    return 0;
}

/*
 * Process a fixed codes block.
 *
 * Format notes:
 *
 * - This block type can be useful for compressing small amounts of data for
 *   which the size of the code descriptions in a dynamic block exceeds the
 *   benefit of custom codes for that block.  For fixed codes, no bits are
 *   spent on code descriptions.  Instead the code lengths for literal/length
 *   codes and distance codes are fixed.  The specific lengths for each symbol
 *   can be seen in the "for" loops below.
 *
 * - The literal/length code is complete, but has two symbols that are invalid
 *   and should result in an error if received.  This cannot be implemented
 *   simply as an incomplete code since those two symbols are in the "middle"
 *   of the code.  They are eight bits long and the longest literal/length\
 *   code is nine bits.  Therefore the code must be constructed with those
 *   symbols, and the invalid symbols must be detected after decoding.
 *
 * - The fixed distance codes also have two invalid symbols that should result
 *   in an error if received.  Since all of the distance codes are the same
 *   length, this can be implemented as an incomplete code.  Then the invalid
 *   codes are detected while decoding.
 */
local int fixed(struct state *s)
{
    static int virgin = 1;
    static short lencnt[MAXBITS+1], lensym[FIXLCODES];
    static short distcnt[MAXBITS+1], distsym[MAXDCODES];
    static struct huffman lencode, distcode;

    /* build fixed huffman tables if first call (may not be thread safe) */
    if (virgin) {
        int symbol;
        short lengths[FIXLCODES];

        /* construct lencode and distcode */
        lencode.count = lencnt;
        lencode.symbol = lensym;
        distcode.count = distcnt;
        distcode.symbol = distsym;

        /* literal/length table */
        for (symbol = 0; symbol < 144; symbol++)
            lengths[symbol] = 8;
        for (; symbol < 256; symbol++)
            lengths[symbol] = 9;
        for (; symbol < 280; symbol++)
            lengths[symbol] = 7;
        for (; symbol < FIXLCODES; symbol++)
            lengths[symbol] = 8;
        construct(&lencode, lengths, FIXLCODES);

        /* distance table */
        for (symbol = 0; symbol < MAXDCODES; symbol++)
            lengths[symbol] = 5;
        construct(&distcode, lengths, MAXDCODES);

        /* do this just once */
        virgin = 0;
    }

    /* decode data until end-of-block code */
    return codes(s, &lencode, &distcode);
}

/*
 * Process a dynamic codes block.
 *
 * Format notes:
 *
 * - A dynamic block starts with a description of the literal/length and
 *   distance codes for that block.  New dynamic blocks allow the compressor to
 *   rapidly adapt to changing data with new codes optimized for that data.
 *
 * - The codes used by the deflate format are "canonical", which means that
 *   the actual bits of the codes are generated in an unambiguous way simply
 *   from the number of bits in each code.  Therefore the code descriptions
 *   are simply a list of code lengths for each symbol.
 *
 * - The code lengths are stored in order for the symbols, so lengths are
 *   provided for each of the literal/length symbols, and for each of the
 *   distance symbols.
 *
 * - If a symbol is not used in the block, this is represented by a zero as the
 *   code length.  This does not mean a zero-length code, but rather that no
 *   code should be created for this symbol.  There is no way in the deflate
 *   format to represent a zero-length code.
 *
 * - The maximum number of bits in a code is 15, so the possible lengths for
 *   any code are 1..15.
 *
 * - The fact that a length of zero is not permitted for a code has an
 *   interesting consequence.  Normally if only one symbol is used for a given
 *   code, then in fact that code could be represented with zero bits.  However
 *   in deflate, that code has to be at least one bit.  So for example, if
 *   only a single distance base symbol appears in a block, then it will be
 *   represented by a single code of length one, in particular one 0 bit.  This
 *   is an incomplete code, since if a 1 bit is received, it has no meaning,
 *   and should result in an error.  So incomplete distance codes of one symbol
 *   should be permitted, and the receipt of invalid codes should be handled.
 *
 * - It is also possible to have a single literal/length code, but that code
 *   must be the end-of-block code, since every dynamic block has one.  This
 *   is not the most efficient way to create an empty block (an empty fixed
 *   block is fewer bits), but it is allowed by the format.  So incomplete
 *   literal/length codes of one symbol should also be permitted.
 *
 * - If there are only literal codes and no lengths, then there are no distance
 *   codes.  This is represented by one distance code with zero bits.
 *
 * - The list of up to 286 length/literal lengths and up to 30 distance lengths
 *   are themselves compressed using Huffman codes and run-length encoding.  In
 *   the list of code lengths, a 0 symbol means no code, a 1..15 symbol means
 *   that length, and the symbols 16, 17, and 18 are run-length instructions.
 *   Each of 16, 17, and 18 are followed by extra bits to define the length of
 *   the run.  16 copies the last length 3 to 6 times.  17 represents 3 to 10
 *   zero lengths, and 18 represents 11 to 138 zero lengths.  Unused symbols
 *   are common, hence the special coding for zero lengths.
 *
 * - The symbols for 0..18 are Huffman coded, and so that code must be
 *   described first.  This is simply a sequence of up to 19 three-bit values
 *   representing no code (0) or the code length for that symbol (1..7).
 *
 * - A dynamic block starts with three fixed-size counts from which is computed
 *   the number of literal/length code lengths, the number of distance code
 *   lengths, and the number of code length code lengths (ok, you come up with
 *   a better name!) in the code descriptions.  For the literal/length and
 *   distance codes, lengths after those provided are considered zero, i.e. no
 *   code.  The code length code lengths are received in a permuted order (see
 *   the order[] array below) to make a short code length code length list more
 *   likely.  As it turns out, very short and very long codes are less likely
 *   to be seen in a dynamic code description, hence what may appear initially
 *   to be a peculiar ordering.
 *
 * - Given the number of literal/length code lengths (nlen) and distance code
 *   lengths (ndist), then they are treated as one long list of nlen + ndist
 *   code lengths.  Therefore run-length coding can and often does cross the
 *   boundary between the two sets of lengths.
 *
 * - So to summarize, the code description at the start of a dynamic block is
 *   three counts for the number of code lengths for the literal/length codes,
 *   the distance codes, and the code length codes.  This is followed by the
 *   code length code lengths, three bits each.  This is used to construct the
 *   code length code which is used to read the remainder of the lengths.  Then
 *   the literal/length code lengths and distance lengths are read as a single
 *   set of lengths using the code length codes.  Codes are constructed from
 *   the resulting two sets of lengths, and then finally you can start
 *   decoding actual compressed data in the block.
 *
 * - For reference, a "typical" size for the code description in a dynamic
 *   block is around 80 bytes.
 */
local int dynamic(struct state *s)
{
    int nlen, ndist, ncode;             /* number of lengths in descriptor */
    int index;                          /* index of lengths[] */
    int err;                            /* construct() return value */
    short lengths[MAXCODES];            /* descriptor code lengths */
    short lencnt[MAXBITS+1], lensym[MAXLCODES];         /* lencode memory */
    short distcnt[MAXBITS+1], distsym[MAXDCODES];       /* distcode memory */
    struct huffman lencode, distcode;   /* length and distance codes */
    static const short order[19] =      /* permutation of code length codes */
        {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

    /* construct lencode and distcode */
    lencode.count = lencnt;
    lencode.symbol = lensym;
    distcode.count = distcnt;
    distcode.symbol = distsym;

    /* get number of lengths in each table, check lengths */
    nlen = bits(s, 5) + 257;
    ndist = bits(s, 5) + 1;
    ncode = bits(s, 4) + 4;
    if (nlen > MAXLCODES || ndist > MAXDCODES)
        return -3;                      /* bad counts */

    /* read code length code lengths (really), missing lengths are zero */
    for (index = 0; index < ncode; index++)
        lengths[order[index]] = bits(s, 3);
    for (; index < 19; index++)
        lengths[order[index]] = 0;

    /* build huffman table for code lengths codes (use lencode temporarily) */
    err = construct(&lencode, lengths, 19);
    if (err != 0)               /* require complete code set here */
        return -4;

    /* read length/literal and distance code length tables */
    index = 0;
    while (index < nlen + ndist) {
        int symbol;             /* decoded value */
        int len;                /* last length to repeat */

        symbol = decode(s, &lencode);
        if (symbol < 0)
            return symbol;          /* invalid symbol */
        if (symbol < 16)                /* length in 0..15 */
            lengths[index++] = symbol;
        else {                          /* repeat instruction */
            len = 0;                    /* assume repeating zeros */
            if (symbol == 16) {         /* repeat last length 3..6 times */
                if (index == 0)
                    return -5;          /* no last length! */
                len = lengths[index - 1];       /* last length */
                symbol = 3 + bits(s, 2);
            }
            else if (symbol == 17)      /* repeat zero 3..10 times */
                symbol = 3 + bits(s, 3);
            else                        /* == 18, repeat zero 11..138 times */
                symbol = 11 + bits(s, 7);
            if (index + symbol > nlen + ndist)
                return -6;              /* too many lengths! */
            while (symbol--)            /* repeat last or zero symbol times */
                lengths[index++] = len;
        }
    }

    /* check for end-of-block code -- there better be one! */
    if (lengths[256] == 0)
        return -9;

    /* build huffman table for literal/length codes */
    err = construct(&lencode, lengths, nlen);
    if (err && (err < 0 || nlen != lencode.count[0] + lencode.count[1]))
        return -7;      /* incomplete code ok only for single length 1 code */

    /* build huffman table for distance codes */
    err = construct(&distcode, lengths + nlen, ndist);
    if (err && (err < 0 || ndist != distcode.count[0] + distcode.count[1]))
        return -8;      /* incomplete code ok only for single length 1 code */

    /* decode data until end-of-block code */
    return codes(s, &lencode, &distcode);
}

/*
 * Inflate source to dest.  On return, destlen and sourcelen are updated to the
 * size of the uncompressed data and the size of the deflate data respectively.
 * On success, the return value of puff() is zero.  If there is an error in the
 * source data, i.e. it is not in the deflate format, then a negative value is
 * returned.  If there is not enough input available or there is not enough
 * output space, then a positive error is returned.  In that case, destlen and
 * sourcelen are not updated to facilitate retrying from the beginning with the
 * provision of more input data or more output space.  In the case of invalid
 * inflate data (a negative error), the dest and source pointers are updated to
 * facilitate the debugging of deflators.
 *
 * puff() also has a mode to determine the size of the uncompressed output with
 * no output written.  For this dest must be (unsigned char *)0.  In this case,
 * the input value of *destlen is ignored, and on return *destlen is set to the
 * size of the uncompressed output.
 *
 * The return codes are:
 *
 *   2:  available inflate data did not terminate
 *   1:  output space exhausted before completing inflate
 *   0:  successful inflate
 *  -1:  invalid block type (type == 3)
 *  -2:  stored block length did not match one's complement
 *  -3:  dynamic block code description: too many length or distance codes
 *  -4:  dynamic block code description: code lengths codes incomplete
 *  -5:  dynamic block code description: repeat lengths with no first length
 *  -6:  dynamic block code description: repeat more than specified lengths
 *  -7:  dynamic block code description: invalid literal/length code lengths
 *  -8:  dynamic block code description: invalid distance code lengths
 *  -9:  dynamic block code description: missing end-of-block code
 * -10:  invalid literal/length or distance code in fixed or dynamic block
 * -11:  distance is too far back in fixed or dynamic block
 *
 * Format notes:
 *
 * - Three bits are read for each block to determine the kind of block and
 *   whether or not it is the last block.  Then the block is decoded and the
 *   process repeated if it was not the last block.
 *
 * - The leftover bits in the last byte of the deflate data after the last
 *   block (if it was a fixed or dynamic block) are undefined and have no
 *   expected values to check.
 */
static int puff(unsigned char *dest,           /* pointer to destination pointer */
         unsigned long *destlen,        /* amount of output space */
         const unsigned char *source,   /* pointer to source data pointer */
         unsigned long *sourcelen)      /* amount of input available */
{
    struct state s;             /* input/output state */
    int last, type;             /* block information */
    int err;                    /* return value */

    /* initialize output state */
    s.out = dest;
    s.outlen = *destlen;                /* ignored if dest is NIL */
    s.outcnt = 0;

    /* initialize input state */
    s.in = source;
    s.inlen = *sourcelen;
    s.incnt = 0;
    s.bitbuf = 0;
    s.bitcnt = 0;

    /* return if bits() or decode() tries to read past available input */
    if (setjmp(s.env) != 0)             /* if came back here via longjmp() */
        err = 2;                        /* then skip do-loop, return error */
    else {
        /* process blocks until last block or error */
        do {
            last = bits(&s, 1);         /* one if last block */
            type = bits(&s, 2);         /* block type 0..3 */
            err = type == 0 ?
                    stored(&s) :
                    (type == 1 ?
                        fixed(&s) :
                        (type == 2 ?
                            dynamic(&s) :
                            -1));       /* type == 3, invalid */
            if (err != 0)
                break;                  /* return with error */
        } while (!last);
    }

    /* update the lengths and return */
    if (err <= 0) {
        *destlen = s.outcnt;
        *sourcelen = s.incnt;
    }
    return err;
}
#undef local
#undef NIL

/* ---- Zip.unzip: extract a .zip (STORE + DEFLATE) into a directory ------- */
static unsigned zz_u16(const unsigned char*p){ return (unsigned)p[0]|((unsigned)p[1]<<8); }
static unsigned zz_u32(const unsigned char*p){ return (unsigned)p[0]|((unsigned)p[1]<<8)|((unsigned)p[2]<<16)|((unsigned)p[3]<<24); }
static void zz_mkdir1(const char*p){
#ifdef _WIN32
    _mkdir(p);
#else
    mkdir(p,0755);
#endif
}
static void zz_mkparents(const char*path){
    char tmp[4096]; size_t n=strlen(path); if(n>=sizeof tmp) return;
    memcpy(tmp,path,n+1);
    for(size_t i=1;i<n;i++){ if(tmp[i]=='/'||tmp[i]=='\\'){ char c=tmp[i]; tmp[i]=0; zz_mkdir1(tmp); tmp[i]=c; } }
}
CmmValue cmm_zip_unzip(CmmValue zpath, CmmValue ddir){
    const char*zp=cmm_to_string(zpath)->data;
    const char*dd=cmm_to_string(ddir)->data;
    FILE*f=fopen(zp,"rb"); if(!f) return cmm_int(-1);
    fseek(f,0,SEEK_END); long fl=ftell(f); fseek(f,0,SEEK_SET);
    if(fl<22){ fclose(f); return cmm_int(-1); }
    unsigned char*buf=(unsigned char*)malloc((size_t)fl);
    if(!buf){ fclose(f); return cmm_int(-1); }
    if(fread(buf,1,(size_t)fl,f)!=(size_t)fl){ free(buf); fclose(f); return cmm_int(-1); }
    fclose(f);
    long eo=-1, lo=fl-22-65536; if(lo<0) lo=0;
    for(long i=fl-22; i>=lo; i--)
        if(buf[i]==0x50&&buf[i+1]==0x4b&&buf[i+2]==0x05&&buf[i+3]==0x06){ eo=i; break; }
    if(eo<0){ free(buf); return cmm_int(-1); }
    unsigned total=zz_u16(buf+eo+10);
    unsigned cdoff=zz_u32(buf+eo+16);
    zz_mkdir1(dd);
    long p=(long)cdoff; int count=0;
    for(unsigned e=0;e<total;e++){
        if(p+46>fl) break;
        if(!(buf[p]==0x50&&buf[p+1]==0x4b&&buf[p+2]==0x01&&buf[p+3]==0x02)) break;
        unsigned method=zz_u16(buf+p+10), csize=zz_u32(buf+p+20), usize=zz_u32(buf+p+24);
        unsigned nlen=zz_u16(buf+p+28), elen=zz_u16(buf+p+30), clen=zz_u16(buf+p+32);
        unsigned exattr=zz_u32(buf+p+38), loff=zz_u32(buf+p+42);
        const char*nm=(const char*)(buf+p+46);
        char outp[4096];
        int w=snprintf(outp,sizeof outp,"%s/%.*s",dd,(int)nlen,nm);
        long adv=46+(long)nlen+(long)elen+(long)clen;
        if(w<0||w>=(int)sizeof outp||strstr(outp,"..")){ p+=adv; continue; }
        if(nlen>0 && nm[nlen-1]=='/'){ zz_mkparents(outp); zz_mkdir1(outp); p+=adv; continue; }
        zz_mkparents(outp);
        if((long)loff+30>fl){ p+=adv; continue; }
        unsigned lnlen=zz_u16(buf+loff+26), lelen=zz_u16(buf+loff+28);
        unsigned char*src=buf+loff+30+lnlen+lelen;
        unsigned char*dst=(unsigned char*)malloc(usize?usize:1);
        int ok=(dst!=NULL);
        if(ok){
            if(method==0){ memcpy(dst,src,csize); }
            else if(method==8){ unsigned long dl=usize, sl=csize;
                if(puff(dst,&dl,src,&sl)!=0||dl!=usize) ok=0; }
            else ok=0;
        }
        if(ok){
            FILE*of=fopen(outp,"wb");
            if(of){ if(usize) fwrite(dst,1,usize,of); fclose(of);
#ifndef _WIN32
                unsigned mode=exattr>>16; if(mode&0111){ unsigned m=mode&0777; chmod(outp,m?m:0755); }
#endif
                count++;
            }
        }
        free(dst); p+=adv;
    }
    free(buf);
    return cmm_int(count);
}

CmmValue cmm_zip_build(CmmValue entries){
    if(entries.tag!=CV_LIST || !entries.list) return cmm_str("");
    size_t ne = entries.list->len;
    typedef struct { const char *name; size_t namelen; uint32_t crc, size, offset; unsigned umode; } ZE;
    ZE *ze = (ZE*)malloc(sizeof(ZE)*(ne?ne:1));
    SB out; sb_init(&out);
    size_t count=0;
    for(size_t i=0;i<ne;i++){
        CmmValue ent = entries.list->items[i];
        CmmString *name    = cmm_to_string(cmm_index_get(ent, cmm_str("name")));
        CmmValue   cv      = cmm_index_get(ent, cmm_str("content"));
        CmmString *content = cmm_to_string(cv);
        CmmValue   mv      = cmm_index_get(ent, cmm_str("mode"));
        CmmValue   ev      = cmm_index_get(ent, cmm_str("exec"));
        unsigned perm = (mv.tag==CV_INT) ? (unsigned)mv.i : (cmm_truthy(ev) ? 0755u : 0644u);
        unsigned umode = perm | 0100000u;            /* S_IFREG | perms */
        uint32_t crc = crc32_buf((const unsigned char*)content->data, content->len);
        uint32_t off = (uint32_t)out.len;
        /* local file header */
        zip_le32(&out,0x04034b50u); zip_le16(&out,20); zip_le16(&out,0); zip_le16(&out,0);
        zip_le16(&out,0); zip_le16(&out,0x21);        /* time, date (1980-01-01) */
        zip_le32(&out,crc);
        zip_le32(&out,(uint32_t)content->len); zip_le32(&out,(uint32_t)content->len);
        zip_le16(&out,(unsigned)name->len); zip_le16(&out,0);
        sb_putn(&out,name->data,name->len);
        sb_putn(&out,content->data,content->len);
        ze[count].name=name->data; ze[count].namelen=name->len; ze[count].crc=crc;
        ze[count].size=(uint32_t)content->len; ze[count].offset=off; ze[count].umode=umode;
        count++;
    }
    uint32_t cd_off = (uint32_t)out.len;
    for(size_t i=0;i<count;i++){
        ZE *z=&ze[i];
        zip_le32(&out,0x02014b50u);
        zip_le16(&out,(3u<<8)|20u);                   /* version made by: unix */
        zip_le16(&out,20); zip_le16(&out,0); zip_le16(&out,0);
        zip_le16(&out,0); zip_le16(&out,0x21);
        zip_le32(&out,z->crc); zip_le32(&out,z->size); zip_le32(&out,z->size);
        zip_le16(&out,(unsigned)z->namelen); zip_le16(&out,0); zip_le16(&out,0);
        zip_le16(&out,0); zip_le16(&out,0);
        zip_le32(&out,(uint32_t)(z->umode<<16));      /* external attrs: unix mode */
        zip_le32(&out,z->offset);
        sb_putn(&out,z->name,z->namelen);
    }
    uint32_t cd_size = (uint32_t)out.len - cd_off;
    zip_le32(&out,0x06054b50u); zip_le16(&out,0); zip_le16(&out,0);
    zip_le16(&out,(unsigned)count); zip_le16(&out,(unsigned)count);
    zip_le32(&out,cd_size); zip_le32(&out,cd_off); zip_le16(&out,0);
    free(ze);
    return sb_to_str(&out);
}

/* ======================================================================== */
/* AWS Lambda custom-runtime client.                                         */
/* When the compiled program is the `bootstrap` of a provided.al2/al2023     */
/* function, it talks to the Lambda Runtime API over plain HTTP at           */
/* $AWS_LAMBDA_RUNTIME_API. This is a thin client over the existing socket   */
/* code -- it does not reimplement the AWS runtime. stdout/stderr already go  */
/* to CloudWatch; Lambda.log writes a line to stderr.                        */
/* ======================================================================== */
#ifndef _WIN32
#  include <sys/resource.h>
#endif

static char      g_lam_reqid[160]  = {0};
static char      g_lam_arn[600]    = {0};
static char      g_lam_trace[300]  = {0};
static long long g_lam_deadline    = 0;

static int lam_ci_eq(const char *a, const char *b, size_t n){
    for(size_t i=0;i<n;i++){
        char x=a[i], y=b[i];
        if(x>='A'&&x<='Z') x=(char)(x-'A'+'a');
        if(y>='A'&&y<='Z') y=(char)(y-'A'+'a');
        if(x!=y) return 0;
    }
    return 1;
}
static int lambda_endpoint(char *host, size_t hn, int *port){
    const char *api=getenv("AWS_LAMBDA_RUNTIME_API");
    if(!api||!*api) return 0;
    const char *colon=strrchr(api,':');
    if(colon){ size_t n=(size_t)(colon-api); if(n>=hn)n=hn-1; memcpy(host,api,n); host[n]=0; *port=atoi(colon+1); }
    else { size_t n=strlen(api); if(n>=hn)n=hn-1; memcpy(host,api,n); host[n]=0; *port=80; }
    return 1;
}
/* copy a response header value (case-insensitive name) out of the head */
static void lam_hdr(const char *resp, const char *name, char *out, size_t outsz){
    out[0]=0;
    size_t namelen=strlen(name);
    const char *head_end=strstr(resp,"\r\n\r\n");
    const char *p=strstr(resp,"\r\n");           /* skip the status line */
    if(p) p+=2; else return;
    while(p && (!head_end || p<head_end)){
        const char *nl=strstr(p,"\r\n");
        size_t linelen = nl ? (size_t)(nl-p) : strlen(p);
        if(linelen>namelen && p[namelen]==':' && lam_ci_eq(p,name,namelen)){
            const char *v=p+namelen+1; while(*v==' '||*v=='\t') v++;
            size_t vlen = nl ? (size_t)(nl-v) : strlen(v);
            if(vlen>=outsz) vlen=outsz-1; memcpy(out,v,vlen); out[vlen]=0; return;
        }
        if(!nl) break; p=nl+2;
    }
}

/* block until the next invocation; capture context; return the event body */
CmmValue cmm_lambda_next(void){
    char host[256]; int port;
    if(!lambda_endpoint(host,sizeof host,&port)) return cmm_str("");
    char req[512];
    int n=snprintf(req,sizeof req,
        "GET /2018-06-01/runtime/invocation/next HTTP/1.1\r\nHost: %s\r\n"
        "Accept: */*\r\nConnection: close\r\n\r\n", host);
    CmmValue resp=http_exchange_plain(host,port,req,(size_t)n);
    CmmString *r=cmm_to_string(resp);
    lam_hdr(r->data,"Lambda-Runtime-Aws-Request-Id",       g_lam_reqid, sizeof g_lam_reqid);
    lam_hdr(r->data,"Lambda-Runtime-Invoked-Function-Arn", g_lam_arn,   sizeof g_lam_arn);
    lam_hdr(r->data,"Lambda-Runtime-Trace-Id",             g_lam_trace, sizeof g_lam_trace);
    char dl[64]; lam_hdr(r->data,"Lambda-Runtime-Deadline-Ms",dl,sizeof dl);
    g_lam_deadline = strtoll(dl,NULL,10);
#ifndef _WIN32
    if(g_lam_trace[0]) setenv("_X_AMZN_TRACE_ID",g_lam_trace,1);   /* for X-Ray */
#endif
    return http_body(resp);
}

static int lambda_post(const char *url, const char *errtype, CmmString *body){
    char host[256]; int port;
    if(!lambda_endpoint(host,sizeof host,&port)) return 0;
    SB req; sb_init(&req); char line[700];
    snprintf(line,sizeof line,
        "POST %s HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\n",url,host);
    sb_put(&req,line);
    if(errtype){ snprintf(line,sizeof line,"Lambda-Runtime-Function-Error-Type: %s\r\n",errtype); sb_put(&req,line); }
    snprintf(line,sizeof line,"Content-Length: %zu\r\nConnection: close\r\n\r\n",body->len);
    sb_put(&req,line); sb_putn(&req,body->data,body->len);
    size_t reqlen=req.len; char *buf=(char*)malloc(reqlen?reqlen:1);
    memcpy(buf,req.p,reqlen); free(req.p);
    CmmValue resp=http_exchange_plain(host,port,buf,reqlen); free(buf);
    CmmString *r=cmm_to_string(resp);
    return (r->len>=12) && (r->data[9]=='2');     /* HTTP/1.x 2xx */
}

CmmValue cmm_lambda_success(CmmValue body){
    char url[256];
    snprintf(url,sizeof url,"/2018-06-01/runtime/invocation/%s/response",g_lam_reqid);
    return cmm_bool(lambda_post(url,NULL,cmm_to_string(body)));
}
static CmmValue lambda_error_at(const char *url, CmmValue type, CmmValue msg){
    CmmString *t=cmm_to_string(type), *m=cmm_to_string(msg);
    json_tbl();                          /* ensure the escape table is built */
    SB j; sb_init(&j);
    sb_put(&j,"{\"errorType\":");  json_str(&j,t->data,t->len);
    sb_put(&j,",\"errorMessage\":"); json_str(&j,m->data,m->len);
    sb_put(&j,"}");
    CmmValue jv=sb_to_str(&j);
    return cmm_bool(lambda_post(url,t->data,jv.s));
}
CmmValue cmm_lambda_failure(CmmValue type, CmmValue msg){
    char url[256];
    snprintf(url,sizeof url,"/2018-06-01/runtime/invocation/%s/error",g_lam_reqid);
    return lambda_error_at(url,type,msg);
}
CmmValue cmm_lambda_init_error(CmmValue type, CmmValue msg){
    return lambda_error_at("/2018-06-01/runtime/init/error",type,msg);
}

CmmValue cmm_lambda_request_id(void){ return cmm_str(g_lam_reqid); }
CmmValue cmm_lambda_deadline(void){   return cmm_int(g_lam_deadline); }
CmmValue cmm_lambda_arn(void){        return cmm_str(g_lam_arn); }
CmmValue cmm_lambda_trace(void){      return cmm_str(g_lam_trace); }

/* write a log line to stderr (captured by CloudWatch Logs) */
CmmValue cmm_lambda_log(CmmValue msg){
    CmmString *m=cmm_to_string(msg);
    fwrite(m->data,1,m->len,stderr); fputc('\n',stderr); fflush(stderr);
    return cmm_empty();
}

/* peak resident set size of this process, in bytes */
CmmValue cmm_sys_peak_rss(void){
    long long bytes=0;
#ifndef _WIN32
    FILE *f=fopen("/proc/self/status","r");
    if(f){ char line[256];
        while(fgets(line,sizeof line,f)){
            if(strncmp(line,"VmHWM:",6)==0){ long long kb=0; sscanf(line+6,"%lld",&kb); bytes=kb*1024; break; }
        }
        fclose(f);
    }
    if(bytes==0){ struct rusage ru; if(getrusage(RUSAGE_SELF,&ru)==0) bytes=(long long)ru.ru_maxrss*1024; }
#endif
    return cmm_int(bytes);
}

/* === injected: standalone crypto + base64 + status-aware http === */
/* ======================================================================== */
/* Standalone crypto: SHA-256, SHA-1, HMAC-SHA256 (public-domain style).     */
/* Independent of the TLS library so signing (SigV4) and MySQL auth work     */
/* whether or not a program links mbedTLS.                                   */
/* ======================================================================== */
static void cx_sha256(const unsigned char *m, size_t len, unsigned char out[32]){
    static const uint32_t K[64]={
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    size_t total=len; unsigned char blk[64]; size_t i=0;
    #define ROR(x,n) (((x)>>(n))|((x)<<(32-(n))))
    while(1){
        size_t chunk = (len-i>=64)?64:(len-i);
        if(chunk==64){ memcpy(blk,m+i,64); i+=64; }
        else {
            memcpy(blk,m+i,chunk); blk[chunk]=0x80; memset(blk+chunk+1,0,64-chunk-1);
            if(chunk>=56){
                uint32_t w[64]; for(int t=0;t<16;t++) w[t]=(blk[t*4]<<24)|(blk[t*4+1]<<16)|(blk[t*4+2]<<8)|blk[t*4+3];
                for(int t=16;t<64;t++){ uint32_t s0=ROR(w[t-15],7)^ROR(w[t-15],18)^(w[t-15]>>3), s1=ROR(w[t-2],17)^ROR(w[t-2],19)^(w[t-2]>>10); w[t]=w[t-16]+s0+w[t-7]+s1; }
                uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
                for(int t=0;t<64;t++){ uint32_t S1=ROR(e,6)^ROR(e,11)^ROR(e,25),ch=(e&f)^((~e)&g),t1=hh+S1+ch+K[t]+w[t],S0=ROR(a,2)^ROR(a,13)^ROR(a,22),mj=(a&b)^(a&c)^(b&c),t2=S0+mj; hh=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2; }
                h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
                memset(blk,0,64);
            }
            uint64_t bits=(uint64_t)total*8;
            for(int t=0;t<8;t++) blk[63-t]=(unsigned char)(bits>>(t*8));
            i=len+1;
        }
        uint32_t w[64]; for(int t=0;t<16;t++) w[t]=(blk[t*4]<<24)|(blk[t*4+1]<<16)|(blk[t*4+2]<<8)|blk[t*4+3];
        for(int t=16;t<64;t++){ uint32_t s0=ROR(w[t-15],7)^ROR(w[t-15],18)^(w[t-15]>>3), s1=ROR(w[t-2],17)^ROR(w[t-2],19)^(w[t-2]>>10); w[t]=w[t-16]+s0+w[t-7]+s1; }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for(int t=0;t<64;t++){ uint32_t S1=ROR(e,6)^ROR(e,11)^ROR(e,25),ch=(e&f)^((~e)&g),t1=hh+S1+ch+K[t]+w[t],S0=ROR(a,2)^ROR(a,13)^ROR(a,22),mj=(a&b)^(a&c)^(b&c),t2=S0+mj; hh=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2; }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
        if(i>len) break;
    }
    #undef ROR
    for(int t=0;t<8;t++){ out[t*4]=h[t]>>24; out[t*4+1]=h[t]>>16; out[t*4+2]=h[t]>>8; out[t*4+3]=h[t]; }
}
static void cx_sha1(const unsigned char *m, size_t len, unsigned char out[20]){
    uint32_t h[5]={0x67452301,0xEFCDAB89,0x98BADCFE,0x10325476,0xC3D2E1F0};
    size_t newlen=((len+8)/64+1)*64; unsigned char *msg=(unsigned char*)malloc(newlen);
    memcpy(msg,m,len); msg[len]=0x80; memset(msg+len+1,0,newlen-len-1);
    uint64_t bits=(uint64_t)len*8; for(int t=0;t<8;t++) msg[newlen-1-t]=(unsigned char)(bits>>(t*8));
    #define ROL(x,n) (((x)<<(n))|((x)>>(32-(n))))
    for(size_t off=0;off<newlen;off+=64){
        uint32_t w[80]; for(int t=0;t<16;t++) w[t]=(msg[off+t*4]<<24)|(msg[off+t*4+1]<<16)|(msg[off+t*4+2]<<8)|msg[off+t*4+3];
        for(int t=16;t<80;t++) w[t]=ROL(w[t-3]^w[t-8]^w[t-14]^w[t-16],1);
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4];
        for(int t=0;t<80;t++){ uint32_t f,k;
            if(t<20){f=(b&c)|((~b)&d);k=0x5A827999;} else if(t<40){f=b^c^d;k=0x6ED9EBA1;}
            else if(t<60){f=(b&c)|(b&d)|(c&d);k=0x8F1BBCDC;} else {f=b^c^d;k=0xCA62C1D6;}
            uint32_t tmp=ROL(a,5)+f+e+k+w[t]; e=d;d=c;c=ROL(b,30);b=a;a=tmp; }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;
    }
    #undef ROL
    free(msg);
    for(int t=0;t<5;t++){ out[t*4]=h[t]>>24; out[t*4+1]=h[t]>>16; out[t*4+2]=h[t]>>8; out[t*4+3]=h[t]; }
}
static void cx_hmac_sha256(const unsigned char *key,size_t kl,const unsigned char *d,size_t n,unsigned char out[32]){
    unsigned char k[64], ki[64], ko[64], kh[32];
    if(kl>64){ cx_sha256(key,kl,kh); memcpy(k,kh,32); memset(k+32,0,32); }
    else { memcpy(k,key,kl); memset(k+kl,0,64-kl); }
    for(int i=0;i<64;i++){ ki[i]=k[i]^0x36; ko[i]=k[i]^0x5c; }
    unsigned char *inner=(unsigned char*)malloc(64+n); memcpy(inner,ki,64); memcpy(inner+64,d,n);
    unsigned char ih[32]; cx_sha256(inner,64+n,ih); free(inner);
    unsigned char outer[96]; memcpy(outer,ko,64); memcpy(outer+64,ih,32);
    cx_sha256(outer,96,out);
}
static void cx_hex(const unsigned char *d,size_t n,char *out){
    static const char hx[]="0123456789abcdef";
    for(size_t i=0;i<n;i++){ out[i*2]=hx[(d[i]>>4)&0xF]; out[i*2+1]=hx[d[i]&0xF]; } out[n*2]=0;
}
static int cx_rand(unsigned char *out,size_t n){
#ifdef _WIN32
    HCRYPTPROV h; if(!CryptAcquireContextA(&h,NULL,NULL,PROV_RSA_FULL,CRYPT_VERIFYCONTEXT)) return 0;
    int ok=CryptGenRandom(h,(DWORD)n,out); CryptReleaseContext(h,0); return ok?1:0;
#else
    FILE *f=fopen("/dev/urandom","rb"); if(!f) return 0;
    size_t r=fread(out,1,n,f); fclose(f); return r==n;
#endif
}
static void cx_b64_encode(SB *out,const unsigned char *p,size_t n){
    static const char B[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i=0; char q[4];
    for(;i+3<=n;i+=3){ uint32_t v=(p[i]<<16)|(p[i+1]<<8)|p[i+2];
        q[0]=B[(v>>18)&63];q[1]=B[(v>>12)&63];q[2]=B[(v>>6)&63];q[3]=B[v&63]; sb_putn(out,q,4); }
    if(n-i==1){ uint32_t v=p[i]<<16; q[0]=B[(v>>18)&63];q[1]=B[(v>>12)&63];q[2]='=';q[3]='='; sb_putn(out,q,4); }
    else if(n-i==2){ uint32_t v=(p[i]<<16)|(p[i+1]<<8); q[0]=B[(v>>18)&63];q[1]=B[(v>>12)&63];q[2]=B[(v>>6)&63];q[3]='='; sb_putn(out,q,4); }
}
static void cx_b64_decode(SB *out,const char *p,size_t n){
    static int T[256], init=0;
    if(!init){ for(int i=0;i<256;i++)T[i]=-1;
        const char *B="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for(int i=0;i<64;i++)T[(unsigned char)B[i]]=i; init=1; }
    int acc=0,bits=0;
    for(size_t i=0;i<n;i++){ int c=T[(unsigned char)p[i]]; if(c<0)continue;
        acc=(acc<<6)|c; bits+=6; if(bits>=8){ bits-=8; char ch=(char)((acc>>bits)&0xFF); sb_putn(out,&ch,1); } }
}

/* ---- Crypto / Base64 / status-aware HTTP natives (always available) ---- */
CmmValue cmm_crypto_sha256hex(CmmValue data){
    CmmString *s=cmm_to_string(data); unsigned char h[32]; char out[65];
    cx_sha256((const unsigned char*)s->data,s->len,h); cx_hex(h,32,out);
    return cmm_str(out);
}
CmmValue cmm_crypto_sha1hex(CmmValue data){
    CmmString *s=cmm_to_string(data); unsigned char h[20]; char out[41];
    cx_sha1((const unsigned char*)s->data,s->len,h); cx_hex(h,20,out);
    return cmm_str(out);
}
CmmValue cmm_crypto_hmac_sha256(CmmValue key, CmmValue data){
    CmmString *k=cmm_to_string(key), *d=cmm_to_string(data); unsigned char h[32];
    cx_hmac_sha256((const unsigned char*)k->data,k->len,(const unsigned char*)d->data,d->len,h);
    return cmm_str_n((const char*)h,32);
}
CmmValue cmm_crypto_hmac_sha256_hex(CmmValue key, CmmValue data){
    CmmString *k=cmm_to_string(key), *d=cmm_to_string(data); unsigned char h[32]; char out[65];
    cx_hmac_sha256((const unsigned char*)k->data,k->len,(const unsigned char*)d->data,d->len,h);
    cx_hex(h,32,out); return cmm_str(out);
}
/* HMAC-SHA1 (raw 20 bytes) — needed for TOTP/HOTP */
static void cx_hmac_sha1(const unsigned char *key,size_t kl,const unsigned char *d,size_t n,unsigned char out[20]){
    unsigned char k[64], ki[64], ko[64], kh[20];
    if(kl>64){ cx_sha1(key,kl,kh); memcpy(k,kh,20); memset(k+20,0,44); }
    else { memcpy(k,key,kl); memset(k+kl,0,64-kl); }
    for(int i=0;i<64;i++){ ki[i]=k[i]^0x36; ko[i]=k[i]^0x5c; }
    unsigned char *inner=(unsigned char*)malloc(64+n); memcpy(inner,ki,64); if(n) memcpy(inner+64,d,n);
    unsigned char ih[20]; cx_sha1(inner,64+n,ih); free(inner);
    unsigned char outer[84]; memcpy(outer,ko,64); memcpy(outer+64,ih,20);
    cx_sha1(outer,84,out);
}
CmmValue cmm_crypto_hmac_sha1(CmmValue key, CmmValue data){
    CmmString *k=cmm_to_string(key), *d=cmm_to_string(data); unsigned char h[20];
    cx_hmac_sha1((const unsigned char*)k->data,k->len,(const unsigned char*)d->data,d->len,h);
    return cmm_str_n((const char*)h,20);
}
/* RFC 4226 HOTP: dynamic truncation of HMAC-SHA1(seed, 8-byte big-endian counter). */
CmmValue cmm_crypto_hotp(CmmValue seed, CmmValue counter, CmmValue digits){
    CmmString *s=cmm_to_string(seed);
    uint64_t c=(uint64_t)as_int(counter);
    int dg=(int)as_int(digits); if(dg<1) dg=6; if(dg>9) dg=9;
    unsigned char cb[8]; for(int i=7;i>=0;i--){ cb[i]=(unsigned char)(c & 0xffu); c>>=8; }
    unsigned char h[20];
    cx_hmac_sha1((const unsigned char*)s->data,s->len,cb,8,h);
    int off=h[19]&0x0f;
    uint32_t bin=(((uint32_t)(h[off]&0x7f))<<24)|(((uint32_t)h[off+1])<<16)
                |(((uint32_t)h[off+2])<<8)|((uint32_t)h[off+3]);
    uint32_t mod=1; for(int i=0;i<dg;i++) mod*=10u;
    char buf[16]; snprintf(buf,sizeof buf,"%0*u",dg,(unsigned)(bin%mod));
    return cmm_str(buf);
}
/* constant-time equality, like PHP hash_equals */
CmmValue cmm_crypto_timing_safe_equal(CmmValue a, CmmValue b){
    CmmString *sa=cmm_to_string(a), *sb=cmm_to_string(b);
    if(sa->len!=sb->len) return cmm_bool(0);
    unsigned char diff=0; for(size_t i=0;i<sa->len;i++) diff|=(unsigned char)(sa->data[i]^sb->data[i]);
    return cmm_bool(diff==0);
}
CmmValue cmm_crypto_hex(CmmValue data){
    CmmString *s=cmm_to_string(data); char *out=(char*)malloc(s->len*2+1);
    cx_hex((const unsigned char*)s->data,s->len,out);
    CmmValue r=cmm_str(out); free(out); return r;
}
CmmValue cmm_crypto_random_hex(CmmValue nbytes){
    long n=(nbytes.tag==CV_INT)?(long)nbytes.i:(long)as_double(nbytes); if(n<0)n=0; if(n>4096)n=4096;
    unsigned char *b=(unsigned char*)malloc(n?(size_t)n:1);
    if(!cx_rand(b,(size_t)n)){ free(b); return cmm_str(""); }
    char *out=(char*)malloc((size_t)n*2+1); cx_hex(b,(size_t)n,out);
    CmmValue r=cmm_str(out); free(b); free(out); return r;
}
CmmValue cmm_base64_encode(CmmValue data){
    CmmString *s=cmm_to_string(data); SB b; sb_init(&b);
    cx_b64_encode(&b,(const unsigned char*)s->data,s->len);
    return sb_to_str(&b);
}
CmmValue cmm_base64_decode(CmmValue data){
    CmmString *s=cmm_to_string(data); SB b; sb_init(&b);
    cx_b64_decode(&b,s->data,s->len);
    return sb_to_str(&b);
}
CmmValue cmm_http_send(CmmValue method, CmmValue url, CmmValue headers, CmmValue body){
    char host[256]; int port; int https; const char *path;
    if(!url_split(cmm_to_string(url)->data,&https,host,sizeof host,&port,&path)){
        CmmValue d=cmm_new_dict();
        cmm_dict_set(d,cmm_str("status"),cmm_int(0));
        cmm_dict_set(d,cmm_str("body"),cmm_str(""));
        return d;
    }
    const char *m=cmm_to_string(method)->data;
    CmmString *b=cmm_to_string(body);
    SB req; sb_init(&req); char line[1024];
    snprintf(line,sizeof line,
        "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: cmm/1.0\r\nConnection: close\r\n",
        m,path,host);
    sb_put(&req,line);
    if(headers.tag==CV_LIST && headers.list){
        for(size_t i=0;i<headers.list->len;i++){
            CmmString *h=cmm_to_string(headers.list->items[i]);
            sb_putn(&req,h->data,h->len); sb_put(&req,"\r\n");
        }
    }
    if(b->len>0){ snprintf(line,sizeof line,"Content-Length: %zu\r\n",b->len); sb_put(&req,line); }
    sb_put(&req,"\r\n");
    if(b->len>0) sb_putn(&req,b->data,b->len);
    size_t reqlen=req.len;
    char *buf=(char*)malloc(reqlen?reqlen:1);
    memcpy(buf,req.p,reqlen); free(req.p);
    CmmValue resp=http_exchange(https,host,port,buf,reqlen);
    free(buf);
    CmmString *r=cmm_to_string(resp); int status=0;
    if(r->len>=12 && memcmp(r->data,"HTTP/",5)==0){
        const char *sp=(const char*)memchr(r->data,' ',r->len);
        if(sp) status=atoi(sp+1);
    }
    CmmValue bodyv=http_body(resp);
    CmmValue d=cmm_new_dict();
    cmm_dict_set(d,cmm_str("status"),cmm_int(status));
    cmm_dict_set(d,cmm_str("body"),bodyv);
    return d;
}


/* ===== Request layer: normalize a Lambda Function URL event, plus a
   local dev server that emits the SAME event shape so app paths are identical.
   Injected before the MySQL section; relies on json/base64/dict/list helpers. */

static const char *rq_memfind(const char *h, size_t hn, const char *n, size_t nn){
    if(nn==0||nn>hn) return NULL;
    for(size_t i=0;i+nn<=hn;i++) if(memcmp(h+i,n,nn)==0) return h+i;
    return NULL;
}
static int rq_hex(int c){ if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; if(c>='A'&&c<='F')return c-'A'+10; return -1; }
static void rq_pctdecode(const char *s, size_t n, SB *out){
    for(size_t i=0;i<n;i++){
        char c=s[i];
        if(c=='+'){ char sp=' '; sb_putn(out,&sp,1); }
        else if(c=='%' && i+2<n && rq_hex((unsigned char)s[i+1])>=0 && rq_hex((unsigned char)s[i+2])>=0){
            char b=(char)((rq_hex((unsigned char)s[i+1])<<4)|rq_hex((unsigned char)s[i+2])); sb_putn(out,&b,1); i+=2;
        } else sb_putn(out,&c,1);
    }
}
/* application/x-www-form-urlencoded (or a raw query string) -> CV_DICT */
static CmmValue rq_parse_query(const char *s, size_t n){
    CmmValue d=cmm_new_dict();
    size_t i=0;
    while(i<n){
        size_t st=i; while(i<n && s[i]!='&') i++;
        const char *pair=s+st; size_t pl=i-st;
        size_t eq=0; while(eq<pl && pair[eq]!='=') eq++;
        SB kb; sb_init(&kb); rq_pctdecode(pair,eq,&kb);
        SB vb; sb_init(&vb); if(eq<pl) rq_pctdecode(pair+eq+1,pl-eq-1,&vb);
        CmmValue key=sb_to_str(&kb), val=sb_to_str(&vb);
        if(key.s && key.s->len) cmm_data_set(d,key,val);
        if(i<n) i++;
    }
    return d;
}
/* pull a quoted or bare attribute value out of a MIME header line */
static void rq_attr(const char *hdr, size_t hn, const char *name, SB *out){
    size_t nn=strlen(name);
    const char *at=rq_memfind(hdr,hn,name,nn);
    if(!at) return;
    const char *p=at+nn; const char *end=hdr+hn;
    while(p<end && (*p==' '||*p=='=')) p++;
    if(p<end && *p=='"'){ p++; while(p<end && *p!='"'){ sb_putn(out,p,1); p++; } }
    else { while(p<end && *p!=';' && *p!='\r' && *p!='\n' && *p!=' '){ sb_putn(out,p,1); p++; } }
}
/* multipart/form-data -> fill `form` (CV_DICT) and `files` (CV_LIST of dicts) */
static void rq_parse_multipart(const char *body, size_t bn, const char *boundary,
                               CmmValue form, CmmValue files){
    char delim[300]; int dl=snprintf(delim,sizeof delim,"--%s",boundary);
    const char *p=rq_memfind(body,bn,delim,(size_t)dl);
    if(!p) return;
    const char *end=body+bn;
    p+=dl;
    while(p<end){
        if(p+2<=end && p[0]=='-' && p[1]=='-') break;         /* closing --boundary-- */
        if(p+2<=end && p[0]=='\r' && p[1]=='\n') p+=2;         /* CRLF after boundary   */
        const char *hend=rq_memfind(p,(size_t)(end-p),"\r\n\r\n",4);
        if(!hend) break;
        size_t hlen=(size_t)(hend-p);
        SB nameB; sb_init(&nameB); rq_attr(p,hlen,"name",&nameB);
        SB fileB; sb_init(&fileB); rq_attr(p,hlen,"filename",&fileB);
        SB ctB;   sb_init(&ctB);   rq_attr(p,hlen,"Content-Type:",&ctB);
        const char *cstart=hend+4;
        const char *nextb=rq_memfind(cstart,(size_t)(end-cstart),delim,(size_t)dl);
        const char *cend = nextb ? nextb : end;
        size_t clen=(size_t)(cend-cstart);
        if(clen>=2 && cend[-2]=='\r' && cend[-1]=='\n') clen-=2;  /* strip trailing CRLF */
        CmmValue nameV=sb_to_str(&nameB);
        CmmValue fileV=sb_to_str(&fileB);
        CmmValue ctV=sb_to_str(&ctB);
        if(fileV.s && fileV.s->len){
            CmmValue f=cmm_new_dict();
            cmm_data_set(f,cmm_str("name"),nameV);
            cmm_data_set(f,cmm_str("filename"),fileV);
            cmm_data_set(f,cmm_str("contentType"),ctV);
            cmm_data_set(f,cmm_str("size"),cmm_int((int64_t)clen));
            cmm_data_set(f,cmm_str("data"),cmm_str_n(cstart,clen));
            list_push(files.list,f);
        } else if(nameV.s && nameV.s->len){
            cmm_data_set(form,nameV,cmm_str_n(cstart,clen));
        }
        if(!nextb) break;
        p=nextb+dl;
    }
}

static CmmValue rq_dget(CmmValue d, const char *k){
    if(d.tag!=CV_DICT) return cmm_empty();
    return cmm_data_get(d,cmm_str(k));
}
static const char *rq_dgets(CmmValue d, const char *k, const char *def){
    CmmValue v=rq_dget(d,k);
    return (v.tag==CV_STRING && v.s) ? v.s->data : def;
}
/* case-insensitive header lookup on a CV_DICT of headers */
static CmmValue rq_header_ci(CmmValue headers, const char *name){
    if(headers.tag!=CV_DICT) return cmm_empty();
    CmmDict *d=headers.dict; size_t nn=strlen(name);
    for(size_t i=0;i<d->len;i++){
        CmmString *k=d->entries[i].key;
        if(k && k->len==nn){
            size_t j=0; for(;j<nn;j++){ int a=k->data[j],b=name[j]; if(a>='A'&&a<='Z')a+=32; if(b>='A'&&b<='Z')b+=32; if(a!=b)break; }
            if(j==nn) return d->entries[i].val;
        }
    }
    return cmm_empty();
}

/* Normalize a Lambda Function URL (v2.0) event JSON into a std request. */
CmmValue cmm_req_parse(CmmValue evstr){
    CmmValue ev=cmm_json_decode(evstr);
    CmmValue rc=rq_dget(ev,"requestContext");
    CmmValue http=rq_dget(rc,"http");
    const char *method=rq_dgets(http,"method", rq_dgets(ev,"httpMethod","GET"));
    const char *path=rq_dgets(ev,"rawPath", rq_dgets(ev,"path","/"));
    const char *rawq=rq_dgets(ev,"rawQueryString","");
    CmmValue headers=rq_dget(ev,"headers"); if(headers.tag!=CV_DICT) headers=cmm_new_dict();

    /* body (+ base64) */
    CmmValue bodyV=rq_dget(ev,"body");
    CmmString *bs = (bodyV.tag==CV_STRING)?bodyV.s:NULL;
    CmmValue isb=rq_dget(ev,"isBase64Encoded");
    if(bs && isb.tag==CV_BOOL && isb.b) bodyV=cmm_base64_decode(bodyV), bs=cmm_to_string(bodyV);
    const char *body = bs?bs->data:""; size_t blen = bs?bs->len:0;

    /* query */
    CmmValue query=rq_parse_query(rawq,strlen(rawq));

    /* cookies: from event "cookies" array (v2) and/or Cookie header */
    CmmValue cookies=cmm_new_dict();
    CmmValue cookarr=rq_dget(ev,"cookies");
    if(cookarr.tag==CV_LIST){
        for(size_t i=0;i<cookarr.list->len;i++){ CmmValue c=cookarr.list->items[i];
            if(c.tag==CV_STRING){ const char *s=c.s->data; size_t n=c.s->len; size_t eq=0; while(eq<n&&s[eq]!='=')eq++;
                if(eq<n) cmm_data_set(cookies,cmm_str_n(s,eq),cmm_str_n(s+eq+1,n-eq-1)); } }
    }
    CmmValue ch=rq_header_ci(headers,"cookie");
    if(ch.tag==CV_STRING){ const char *s=ch.s->data; size_t n=ch.s->len, i=0;
        while(i<n){ while(i<n&&(s[i]==' '||s[i]==';'))i++; size_t st=i; while(i<n&&s[i]!=';')i++;
            size_t eq=st; while(eq<i&&s[eq]!='=')eq++; if(eq<i){ size_t ks=st,ke=eq,vs=eq+1,ve=i; while(ke>ks&&s[ke-1]==' ')ke--;
                cmm_data_set(cookies,cmm_str_n(s+ks,ke-ks),cmm_str_n(s+vs,ve-vs)); } } }

    /* content-type dispatch */
    CmmValue ctV=rq_header_ci(headers,"content-type");
    const char *ct = (ctV.tag==CV_STRING)?ctV.s->data:"";
    CmmValue json=cmm_empty(), form=cmm_empty(), files=cmm_new_list();
    if(rq_memfind(ct,strlen(ct),"application/json",16)){
        if(blen) json=cmm_json_decode(cmm_str_n(body,blen));
    } else if(rq_memfind(ct,strlen(ct),"x-www-form-urlencoded",21)){
        form=rq_parse_query(body,blen);
    } else if(rq_memfind(ct,strlen(ct),"multipart/form-data",19)){
        form=cmm_new_dict();
        SB bb; sb_init(&bb); rq_attr(ct,strlen(ct),"boundary",&bb); CmmValue bv=sb_to_str(&bb);
        if(bv.s && bv.s->len) rq_parse_multipart(body,blen,bv.s->data,form,files);
    }

    CmmValue r=cmm_new_dict();
    cmm_data_set(r,cmm_str("method"),cmm_str(method));
    cmm_data_set(r,cmm_str("path"),cmm_str(path));
    cmm_data_set(r,cmm_str("query"),query);
    cmm_data_set(r,cmm_str("headers"),headers);
    cmm_data_set(r,cmm_str("cookies"),cookies);
    cmm_data_set(r,cmm_str("contentType"),cmm_str(ct));
    cmm_data_set(r,cmm_str("body"),cmm_str_n(body,blen));
    cmm_data_set(r,cmm_str("json"),json);
    cmm_data_set(r,cmm_str("form"),form);
    cmm_data_set(r,cmm_str("files"),files);
    return r;
}

/* ---- local dev server: same event shape as Lambda, over real HTTP -------- */
static sockfd_t g_serve_fd=SOCK_BAD;
static CMM_TLS sockfd_t g_serve_client=SOCK_BAD;   /* per-worker-thread current connection */

CmmValue cmm_serve_listen(CmmValue port){
    int p=(int)as_int(port);
    sockfd_t fd=socket(AF_INET,SOCK_STREAM,0);
    if(fd==SOCK_BAD) return cmm_bool(0);
    int yes=1; setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,(const char*)&yes,sizeof yes);
    struct sockaddr_in a; memset(&a,0,sizeof a);
    a.sin_family=AF_INET; a.sin_addr.s_addr=htonl(INADDR_LOOPBACK); a.sin_port=htons((unsigned short)p);
    if(bind(fd,(struct sockaddr*)&a,sizeof a)!=0){ CLOSESOCK(fd); return cmm_bool(0); }
    if(listen(fd,16)!=0){ CLOSESOCK(fd); return cmm_bool(0); }
    g_serve_fd=fd; return cmm_bool(1);
}

CmmValue cmm_serve_next(void){
    if(g_serve_fd==SOCK_BAD) return cmm_str("");
    sockfd_t c=accept(g_serve_fd,NULL,NULL);
    if(c==SOCK_BAD) return cmm_str("");
    g_serve_client=c;
    SB in; sb_init(&in); char buf[4096]; size_t hend=0; long clen=-1;
    for(;;){
        if(!hend && in.len>=4){
            for(size_t i=3;i<in.len;i++) if(in.p[i-3]=='\r'&&in.p[i-2]=='\n'&&in.p[i-1]=='\r'&&in.p[i]=='\n'){ hend=i+1; break; }
            if(hend){ const char *cl=rq_memfind(in.p,hend,"ontent-Length:",14); if(!cl) cl=rq_memfind(in.p,hend,"ontent-length:",14);
                if(cl){ cl+=14; while(*cl==' ')cl++; clen=strtol(cl,NULL,10); } else clen=0; }
        }
        if(hend){ size_t need=hend+(clen>0?(size_t)clen:0); if(in.len>=need) break; }
        int r=recv(c,buf,sizeof buf,0); if(r<=0) break; sb_putn(&in,buf,r);
    }
    /* request line: METHOD SP target SP HTTP/1.1 */
    const char *s=in.p, *lend=rq_memfind(in.p,in.len,"\r\n",2); if(!lend) lend=in.p+in.len;
    const char *m0=s, *m1=s; while(m1<lend && *m1!=' ') m1++;
    const char *t0=(m1<lend)?m1+1:m1, *t1=t0; while(t1<lend && *t1!=' ') t1++;
    char method[16]={0}; size_t ml=(size_t)(m1-m0); if(ml>15)ml=15; memcpy(method,m0,ml);
    const char *qm=t0; while(qm<t1 && *qm!='?') qm++;
    SB pathB; sb_init(&pathB); sb_putn(&pathB,t0,(size_t)(qm-t0));
    SB qB; sb_init(&qB); if(qm<t1) sb_putn(&qB,qm+1,(size_t)(t1-(qm+1)));
    CmmValue pathV=sb_to_str(&pathB), qV=sb_to_str(&qB);

    /* headers -> object; collect Cookie into a cookies array */
    CmmValue hobj=cmm_new_dict(); CmmValue carr=cmm_new_list();
    const char *hp=lend+2, *hstop=(hend?in.p+hend:in.p+in.len);
    while(hp<hstop){
        const char *he=rq_memfind(hp,(size_t)(hstop-hp),"\r\n",2); if(!he) he=hstop;
        if(he==hp) break;
        const char *colon=hp; while(colon<he && *colon!=':') colon++;
        if(colon<he){ size_t kn=(size_t)(colon-hp); const char *v=colon+1; while(v<he&&*v==' ')v++;
            char lk[128]; size_t lkn=kn<127?kn:127; for(size_t i=0;i<lkn;i++){ int ch2=hp[i]; lk[i]=(ch2>='A'&&ch2<='Z')?ch2+32:ch2; } lk[lkn]=0;
            if(lkn==6 && memcmp(lk,"cookie",6)==0){ /* split into cookie list */
                const char *cs=v; size_t cn=(size_t)(he-v), ci=0;
                while(ci<cn){ while(ci<cn&&(cs[ci]==' '||cs[ci]==';'))ci++; size_t st=ci; while(ci<cn&&cs[ci]!=';')ci++;
                    if(ci>st) list_push(carr.list,cmm_str_n(cs+st,ci-st)); }
            } else cmm_data_set(hobj,cmm_str_n(lk,lkn),cmm_str_n(v,(size_t)(he-v)));
        }
        hp=he+2;
    }
    /* base64 the body so binary survives the JSON round-trip */
    const char *body=(hend?in.p+hend:in.p+in.len); size_t blen=(clen>0)?(size_t)clen:0;
    if(hend && in.len<hend+blen) blen=in.len-hend;
    CmmValue b64=cmm_base64_encode(cmm_str_n(body,blen));

    /* assemble a Lambda Function URL (v2.0) event JSON */
    json_tbl(); SB j; sb_init(&j);
    sb_put(&j,"{\"version\":\"2.0\",\"rawPath\":"); json_str(&j,pathV.s->data,pathV.s->len);
    sb_put(&j,",\"rawQueryString\":"); json_str(&j,qV.s->data,qV.s->len);
    sb_put(&j,",\"cookies\":["); for(size_t i=0;i<carr.list->len;i++){ if(i)sb_put(&j,","); CmmString *cs=carr.list->items[i].s; json_str(&j,cs->data,cs->len);} sb_put(&j,"]");
    sb_put(&j,",\"headers\":{"); { CmmDict *hd=hobj.dict; for(size_t i=0;i<hd->len;i++){ if(i)sb_put(&j,","); json_str(&j,hd->entries[i].key->data,hd->entries[i].key->len); sb_put(&j,":"); CmmString *vv=cmm_to_string(hd->entries[i].val); json_str(&j,vv->data,vv->len);} } sb_put(&j,"}");
    sb_put(&j,",\"requestContext\":{\"http\":{\"method\":"); json_str(&j,method,strlen(method));
    sb_put(&j,",\"path\":"); json_str(&j,pathV.s->data,pathV.s->len);
    sb_put(&j,",\"sourceIp\":\"127.0.0.1\"}}");
    sb_put(&j,",\"body\":"); json_str(&j,b64.s->data,b64.s->len);
    sb_put(&j,",\"isBase64Encoded\":true}");
    return sb_to_str(&j);
}

CmmValue cmm_serve_respond(CmmValue resp){
    if(g_serve_client==SOCK_BAD) return cmm_bool(0);
    CmmValue r=cmm_json_decode(resp);
    long code=200; CmmValue sc=rq_dget(r,"statusCode"); if(sc.tag==CV_INT) code=(long)sc.i; else if(sc.tag==CV_STRING) code=strtol(sc.s->data,NULL,10);
    CmmValue bodyV=rq_dget(r,"body"); CmmString *bs=(bodyV.tag==CV_STRING)?bodyV.s:NULL;
    CmmValue isb=rq_dget(r,"isBase64Encoded"); if(bs && isb.tag==CV_BOOL && isb.b){ bodyV=cmm_base64_decode(bodyV); bs=cmm_to_string(bodyV); }
    const char *body=bs?bs->data:""; size_t blen=bs?bs->len:0;
    SB o; sb_init(&o); char line[256];
    snprintf(line,sizeof line,"HTTP/1.1 %ld OK\r\n",code); sb_put(&o,line);
    CmmValue hs=rq_dget(r,"headers"); int hasCT=0;
    if(hs.tag==CV_DICT){ CmmDict *hd=hs.dict; for(size_t i=0;i<hd->len;i++){ CmmString *k=hd->entries[i].key; CmmString *v=cmm_to_string(hd->entries[i].val);
        if(k->len==12){ size_t j=0; for(;j<12;j++){int a=k->data[j];if(a>='A'&&a<='Z')a+=32; if(a!="content-type"[j])break;} if(j==12)hasCT=1; }
        sb_putn(&o,k->data,k->len); sb_put(&o,": "); sb_putn(&o,v->data,v->len); sb_put(&o,"\r\n"); } }
    if(!hasCT) sb_put(&o,"Content-Type: application/json\r\n");
    snprintf(line,sizeof line,"Content-Length: %zu\r\nConnection: close\r\n\r\n",blen); sb_put(&o,line);
    sb_putn(&o,body,blen);
    size_t sent=0; while(sent<o.len){ int w=send(g_serve_client,o.p+sent,(int)(o.len-sent),0); if(w<=0)break; sent+=(size_t)w; }
    free(o.p);
    CLOSESOCK(g_serve_client); g_serve_client=SOCK_BAD;
    return cmm_bool(1);
}

/* ===== MySQL client: native_password auth + text (COM_QUERY) protocol == */
#define CMM_MYSQL_MAX 64
typedef struct {
    int used;
    sockfd_t fd;
    unsigned char seq;
    char err[512];
    unsigned long long affected;
    unsigned long long insert_id;
    cmm_mutex_t lock;
#ifdef CMM_HAVE_TLS
    int tls_on;                    /* 1 once the link is upgraded to TLS   */
    int have_conf;                 /* 1 if tconf/tca were inited (custom CA)*/
    mbedtls_ssl_context ssl;
    mbedtls_net_context net;
    mbedtls_ssl_config  tconf;     /* per-conn config when a custom CA used */
    mbedtls_x509_crt    tca;       /* caller-supplied CA chain              */
#endif
} MysqlConn;
static MysqlConn g_mysql[CMM_MYSQL_MAX];
static char g_mysql_last_err[512]="";
static cmm_mutex_t g_mysql_tab;
static int g_mysql_tab_ready = 0;
static void my_tab_lock(void){ if(!g_mysql_tab_ready){ cmm_mutex_init(&g_mysql_tab); g_mysql_tab_ready=1; } cmm_mutex_lock(&g_mysql_tab); }
static void my_tab_unlock(void){ cmm_mutex_unlock(&g_mysql_tab); }

static int my_read_full(sockfd_t fd, unsigned char *buf, size_t n){
    size_t got=0;
    while(got<n){ int r=recv(fd,(char*)buf+got,(int)(n-got),0); if(r<=0) return 0; got+=(size_t)r; }
    return 1;
}
/* All packet I/O goes through these so a TLS-upgraded link is transparent. */
static int my_recv_all(MysqlConn *c, unsigned char *buf, size_t n){
#ifdef CMM_HAVE_TLS
    if(c->tls_on){ size_t g=0; while(g<n){ int r=mbedtls_ssl_read(&c->ssl,buf+g,n-g);
        if(r<=0){ if(r==MBEDTLS_ERR_SSL_WANT_READ||r==MBEDTLS_ERR_SSL_WANT_WRITE) continue; return 0; }
        g+=(size_t)r; } return 1; }
#endif
    return my_read_full(c->fd,buf,n);
}
static int my_send_all(MysqlConn *c, const unsigned char *buf, size_t n){
#ifdef CMM_HAVE_TLS
    if(c->tls_on){ size_t s=0; while(s<n){ int w=mbedtls_ssl_write(&c->ssl,buf+s,n-s);
        if(w<=0){ if(w==MBEDTLS_ERR_SSL_WANT_READ||w==MBEDTLS_ERR_SSL_WANT_WRITE) continue; return 0; }
        s+=(size_t)w; } return 1; }
#endif
    size_t sent=0;
    while(sent<n){ int w=send(c->fd,(const char*)buf+sent,(int)(n-sent),0); if(w<=0) return 0; sent+=(size_t)w; }
    return 1;
}
static int my_read_packet(MysqlConn *c, unsigned char **payload, size_t *plen){
    unsigned char hdr[4];
    if(!my_recv_all(c,hdr,4)) return 0;
    size_t len = (size_t)hdr[0] | ((size_t)hdr[1]<<8) | ((size_t)hdr[2]<<16);
    c->seq = hdr[3];
    unsigned char *buf=(unsigned char*)malloc(len?len:1);
    if(len && !my_recv_all(c,buf,len)){ free(buf); return 0; }
    *payload=buf; *plen=len; return 1;
}
static int my_write_packet(MysqlConn *c, const unsigned char *payload, size_t len){
    unsigned char hdr[4];
    hdr[0]=len&0xff; hdr[1]=(len>>8)&0xff; hdr[2]=(len>>16)&0xff; hdr[3]=c->seq;
    if(!my_send_all(c,hdr,4)) return 0;
    if(len && !my_send_all(c,payload,len)) return 0;
    return 1;
}
static unsigned long long my_lenenc(const unsigned char **p, const unsigned char *end){
    if(*p>=end) return 0;
    unsigned char b=**p; (*p)++;
    if(b<0xfb) return b;
    if(b==0xfb) return 0;
    if(b==0xfc){ unsigned long long v=(unsigned long long)(*p)[0]|((unsigned long long)(*p)[1]<<8); *p+=2; return v; }
    if(b==0xfd){ unsigned long long v=(unsigned long long)(*p)[0]|((unsigned long long)(*p)[1]<<8)|((unsigned long long)(*p)[2]<<16); *p+=3; return v; }
    unsigned long long v=0; for(int i=0;i<8;i++) v|=((unsigned long long)(*p)[i])<<(8*i); *p+=8; return v;
}
/* native_password scramble: SHA1(pass) XOR SHA1(salt + SHA1(SHA1(pass))) */
static void my_scramble(const char *pass, const unsigned char *salt, unsigned char out[20]){
    unsigned char s1[20], s2[20], s3[20], cat[40];
    cx_sha1((const unsigned char*)pass, strlen(pass), s1);
    cx_sha1(s1,20,s2);
    memcpy(cat,salt,20); memcpy(cat+20,s2,20);
    cx_sha1(cat,40,s3);
    for(int i=0;i<20;i++) out[i]=s1[i]^s3[i];
}

#ifdef CMM_HAVE_TLS
/* Upgrade an already-connected MySQL socket to TLS (MySQL STARTTLS flow).
   ca_pem: PEM trust anchors to verify the server with; if empty, the built-in
   Mozilla root bundle is used. host is used for SNI + hostname verification. */
static int my_tls_wrap(MysqlConn *c, const char *host, const char *ca_pem){
    cmm_tls_init();
    mbedtls_ssl_init(&c->ssl);
    c->net.fd = (int)c->fd;
    mbedtls_ssl_config *conf = &g_tls_conf;
    if(ca_pem && ca_pem[0]){
        mbedtls_x509_crt_init(&c->tca);
        mbedtls_ssl_config_init(&c->tconf);
        c->have_conf = 1;
        if(mbedtls_ssl_config_defaults(&c->tconf, MBEDTLS_SSL_IS_CLIENT,
               MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)!=0){
            snprintf(c->err,sizeof c->err,"TLS config failed"); goto tls_fail; }
        if(mbedtls_x509_crt_parse(&c->tca,(const unsigned char*)ca_pem, strlen(ca_pem)+1)!=0){
            snprintf(c->err,sizeof c->err,"could not parse CA certificate"); goto tls_fail; }
        int insec = getenv("CMMC_TLS_INSECURE")!=NULL;
        mbedtls_ssl_conf_authmode(&c->tconf, insec?MBEDTLS_SSL_VERIFY_NONE:MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&c->tconf,&c->tca,NULL);
        mbedtls_ssl_conf_rng(&c->tconf, mbedtls_ctr_drbg_random, &g_tls_drbg);
        conf = &c->tconf;
    }
    if(mbedtls_ssl_setup(&c->ssl,conf)!=0){ snprintf(c->err,sizeof c->err,"TLS setup failed"); goto tls_fail; }
    mbedtls_ssl_set_hostname(&c->ssl,host);
    mbedtls_ssl_set_bio(&c->ssl,&c->net,mbedtls_net_send,mbedtls_net_recv,NULL);
    { int hs; while((hs=mbedtls_ssl_handshake(&c->ssl))!=0)
        if(hs!=MBEDTLS_ERR_SSL_WANT_READ && hs!=MBEDTLS_ERR_SSL_WANT_WRITE){
            snprintf(c->err,sizeof c->err,"TLS handshake failed"); goto tls_fail; } }
    if(mbedtls_ssl_get_verify_result(&c->ssl)!=0 && getenv("CMMC_TLS_INSECURE")==NULL){
        snprintf(c->err,sizeof c->err,"TLS certificate verification failed"); goto tls_fail; }
    c->tls_on = 1;
    return 1;
tls_fail:
    mbedtls_ssl_free(&c->ssl);
    if(c->have_conf){ mbedtls_ssl_config_free(&c->tconf); mbedtls_x509_crt_free(&c->tca); c->have_conf=0; }
    return 0;
}
#endif

static CmmValue mysql_connect_impl(CmmValue host, CmmValue port, CmmValue user,
                                   CmmValue pass, CmmValue db,
                                   int use_tls, const char *ca_pem){
    const char *h=cmm_to_string(host)->data;
    int p=(int)as_int(port);
    const char *u=cmm_to_string(user)->data;
    const char *pw=cmm_to_string(pass)->data;
    const char *dbn=cmm_to_string(db)->data;
    sockfd_t fd=tcp_connect(h,p);
    if(fd==SOCK_BAD) return cmm_int(-1);
    my_tab_lock();
    int idx=-1; for(int i=0;i<CMM_MYSQL_MAX;i++){ if(!g_mysql[i].used){ idx=i; break; } }
    if(idx<0){ my_tab_unlock(); CLOSESOCK(fd); return cmm_int(-1); }
    MysqlConn *c=&g_mysql[idx];
    memset(c,0,sizeof *c); c->used=1; c->fd=fd; cmm_mutex_init(&c->lock);
    my_tab_unlock();

    unsigned char *pl=NULL; size_t pn=0;
    if(!my_read_packet(c,&pl,&pn) || pn<2 || pl[0]!=10){ if(pl)free(pl); goto fail; }
    {
        const unsigned char *q=pl+1; const unsigned char *end=pl+pn;
        while(q<end && *q) q++; q++;      /* server version */
        q+=4;                             /* thread id */
        unsigned char salt[20];
        memcpy(salt,q,8); q+=8;           /* salt part 1 */
        q++;                              /* filler */
        unsigned int cap_lo = (unsigned int)q[0] | ((unsigned int)q[1]<<8);
        q+=2;                             /* cap lower */
        q+=1;                             /* charset */
        q+=2;                             /* status */
        q+=2;                             /* cap upper */
        q+=1;                             /* auth plugin data len */
        q+=10;                            /* reserved */
        memcpy(salt+8,q,12);              /* salt part 2 */
        free(pl); pl=NULL;

        unsigned char token[20]; int tlen=0;
        if(pw[0]){ my_scramble(pw,salt,token); tlen=20; }
        uint32_t cap = 0x00000001u|0x00000200u|0x00008000u|0x00080000u|0x00000004u|0x00002000u|0x00020000u;
        int haveDb = dbn && dbn[0];
        if(haveDb) cap |= 0x00000008u;

        if(use_tls){
#ifdef CMM_HAVE_TLS
            if(!(cap_lo & 0x00000800u)){ snprintf(c->err,sizeof c->err,"server does not offer TLS"); goto fail; }
            cap |= 0x00000800u;           /* CLIENT_SSL */
            /* SSLRequest: the 32-byte prefix of the handshake response (no user) */
            SB sr; sb_init(&sr);
            unsigned char sh[4]; sh[0]=cap&0xff; sh[1]=(cap>>8)&0xff; sh[2]=(cap>>16)&0xff; sh[3]=(cap>>24)&0xff; sb_putn(&sr,(char*)sh,4);
            unsigned char mp0[4]={0,0,0,1}; sb_putn(&sr,(char*)mp0,4);
            char cs0=33; sb_putn(&sr,&cs0,1);
            char z0[23]; memset(z0,0,23); sb_putn(&sr,z0,23);
            c->seq=1;
            int sok=my_write_packet(c,(unsigned char*)sr.p,sr.len); free(sr.p);
            if(!sok){ snprintf(c->err,sizeof c->err,"could not send SSLRequest"); goto fail; }
            if(!my_tls_wrap(c,h,ca_pem)) goto fail;   /* err already set */
            c->seq=2;
#else
            snprintf(c->err,sizeof c->err,"TLS not compiled in"); goto fail;
#endif
        } else {
            (void)cap_lo;
            c->seq=1;
        }

        SB rb; sb_init(&rb);
        unsigned char h4[4]; h4[0]=cap&0xff; h4[1]=(cap>>8)&0xff; h4[2]=(cap>>16)&0xff; h4[3]=(cap>>24)&0xff; sb_putn(&rb,(char*)h4,4);
        unsigned char mp[4]={0,0,0,1}; sb_putn(&rb,(char*)mp,4);
        char cs=33; sb_putn(&rb,&cs,1);
        char z23[23]; memset(z23,0,23); sb_putn(&rb,z23,23);
        sb_put(&rb,u); { char z=0; sb_putn(&rb,&z,1); }
        { char l=(char)tlen; sb_putn(&rb,&l,1); if(tlen) sb_putn(&rb,(char*)token,tlen); }
        if(haveDb){ sb_put(&rb,dbn); char z=0; sb_putn(&rb,&z,1); }
        sb_put(&rb,"mysql_native_password"); { char z=0; sb_putn(&rb,&z,1); }
        int wok=my_write_packet(c,(unsigned char*)rb.p,rb.len); free(rb.p);
        if(!wok) goto fail;
    }
    if(!my_read_packet(c,&pl,&pn)) goto fail;
    if(pn>0 && pl[0]==0x00){ free(pl); return cmm_int(idx); }
    if(pn>0 && pl[0]==0xff){
        int code=pl[1]|(pl[2]<<8); const char *msg=(const char*)(pl+3); size_t mlen=pn>3?pn-3:0;
        if(mlen>6 && msg[0]=='#'){ msg+=6; mlen-=6; }
        snprintf(c->err,sizeof c->err,"%d: %.*s",code,(int)mlen,msg); free(pl); goto fail;
    }
    if(pn>0 && (unsigned char)pl[0]==0xfe){ snprintf(c->err,sizeof c->err,"server requested auth-switch (only mysql_native_password supported)"); free(pl); goto fail; }
    if(pl) free(pl);
fail:
    snprintf(g_mysql_last_err,sizeof g_mysql_last_err,"%s",g_mysql[idx].err);
    my_tab_lock();
    if(g_mysql[idx].used){
#ifdef CMM_HAVE_TLS
        if(g_mysql[idx].tls_on){ mbedtls_ssl_close_notify(&g_mysql[idx].ssl); mbedtls_ssl_free(&g_mysql[idx].ssl); }
        if(g_mysql[idx].have_conf){ mbedtls_ssl_config_free(&g_mysql[idx].tconf); mbedtls_x509_crt_free(&g_mysql[idx].tca); }
#endif
        CLOSESOCK(g_mysql[idx].fd); g_mysql[idx].used=0;
    }
    my_tab_unlock();
    return cmm_int(-1);
}

CmmValue cmm_mysql_connect(CmmValue host, CmmValue port, CmmValue user, CmmValue pass, CmmValue db){
    return mysql_connect_impl(host,port,user,pass,db,0,NULL);
}
CmmValue cmm_mysql_connect_tls(CmmValue host, CmmValue port, CmmValue user, CmmValue pass, CmmValue db, CmmValue ca){
    return mysql_connect_impl(host,port,user,pass,db,1,cmm_to_string(ca)->data);
}

static CmmValue my_run_query(MysqlConn *c, const char *sql, size_t sqllen){
    c->err[0]=0; c->affected=0; c->insert_id=0;
    c->seq=0;
    unsigned char *pkt=(unsigned char*)malloc(sqllen+1); pkt[0]=0x03; memcpy(pkt+1,sql,sqllen);
    int ok=my_write_packet(c,pkt,sqllen+1); free(pkt);
    if(!ok){ snprintf(c->err,sizeof c->err,"write failed"); return cmm_new_list(); }
    unsigned char *pl=NULL; size_t pn=0;
    if(!my_read_packet(c,&pl,&pn)){ snprintf(c->err,sizeof c->err,"read failed"); return cmm_new_list(); }
    if(pn>0 && pl[0]==0xff){ int code=pl[1]|(pl[2]<<8); const char*msg=(const char*)(pl+3); size_t mlen=pn>3?pn-3:0; if(mlen>6&&msg[0]=='#'){msg+=6;mlen-=6;} snprintf(c->err,sizeof c->err,"%d: %.*s",code,(int)mlen,msg); free(pl); return cmm_new_list(); }
    if(pn>0 && pl[0]==0x00){ const unsigned char *q=pl+1; const unsigned char *e=pl+pn; c->affected=my_lenenc(&q,e); c->insert_id=my_lenenc(&q,e); free(pl); return cmm_new_list(); }
    if(pn>0 && (unsigned char)pl[0]==0xfb){ snprintf(c->err,sizeof c->err,"LOCAL INFILE unsupported"); free(pl); return cmm_new_list(); }

    const unsigned char *q=pl; const unsigned char *e=pl+pn;
    unsigned long long ncol=my_lenenc(&q,e); free(pl); pl=NULL;
    char **names=(char**)malloc(sizeof(char*)*(ncol?ncol:1));
    for(unsigned long long i=0;i<ncol;i++){
        names[i]=NULL;
        if(!my_read_packet(c,&pl,&pn)){ names[i]=strdup(""); continue; }
        const unsigned char *p=pl; const unsigned char *pe=pl+pn;
        for(int s=0;s<4;s++){ unsigned long long l=my_lenenc(&p,pe); p+=l; }
        unsigned long long nl=my_lenenc(&p,pe);
        names[i]=(char*)malloc((size_t)nl+1); memcpy(names[i],p,(size_t)nl); names[i][nl]=0;
        free(pl); pl=NULL;
    }
    if(my_read_packet(c,&pl,&pn)){ free(pl); pl=NULL; }  /* EOF after column defs */

    CmmValue rows=cmm_new_list();
    while(1){
        if(!my_read_packet(c,&pl,&pn)) break;
        if(pn>0 && (unsigned char)pl[0]==0xfe && pn<9){ free(pl); break; }
        if(pn>0 && (unsigned char)pl[0]==0xff){ int code=pl[1]|(pl[2]<<8); const char*msg=(const char*)(pl+3); size_t mlen=pn>3?pn-3:0; if(mlen>6&&msg[0]=='#'){msg+=6;mlen-=6;} snprintf(c->err,sizeof c->err,"%d: %.*s",code,(int)mlen,msg); free(pl); break; }
        const unsigned char *p=pl; const unsigned char *pe=pl+pn;
        CmmValue row=cmm_new_dict();
        for(unsigned long long i=0;i<ncol;i++){
            if(p<pe && *p==0xfb){ p++; cmm_dict_set(row,cmm_str(names[i]),cmm_empty()); continue; }
            unsigned long long l=my_lenenc(&p,pe);
            CmmValue val=cmm_str_n((const char*)p,(size_t)l); p+=l;
            cmm_dict_set(row,cmm_str(names[i]),val);
        }
        list_push(rows.list,row);
        free(pl); pl=NULL;
    }
    for(unsigned long long i=0;i<ncol;i++) free(names[i]);
    free(names);
    return rows;
}

CmmValue cmm_mysql_query(CmmValue conn, CmmValue sql){
    int idx=(int)as_int(conn);
    if(idx<0||idx>=CMM_MYSQL_MAX||!g_mysql[idx].used) return cmm_new_list();
    MysqlConn *c=&g_mysql[idx];
    cmm_mutex_lock(&c->lock);
    CmmString *s=cmm_to_string(sql);
    CmmValue r=my_run_query(c,s->data,s->len);
    cmm_mutex_unlock(&c->lock);
    return r;
}
CmmValue cmm_mysql_exec(CmmValue conn, CmmValue sql){
    int idx=(int)as_int(conn);
    if(idx<0||idx>=CMM_MYSQL_MAX||!g_mysql[idx].used) return cmm_int(-1);
    MysqlConn *c=&g_mysql[idx];
    cmm_mutex_lock(&c->lock);
    CmmString *s=cmm_to_string(sql);
    my_run_query(c,s->data,s->len);
    long long aff=c->err[0]?-1:(long long)c->affected;
    cmm_mutex_unlock(&c->lock);
    return cmm_int(aff);
}
CmmValue cmm_mysql_insert_id(CmmValue conn){ int idx=(int)as_int(conn); if(idx<0||idx>=CMM_MYSQL_MAX||!g_mysql[idx].used) return cmm_int(0); return cmm_int((int64_t)g_mysql[idx].insert_id); }
CmmValue cmm_mysql_affected(CmmValue conn){ int idx=(int)as_int(conn); if(idx<0||idx>=CMM_MYSQL_MAX||!g_mysql[idx].used) return cmm_int(0); return cmm_int((int64_t)g_mysql[idx].affected); }
CmmValue cmm_mysql_error(CmmValue conn){ int idx=(int)as_int(conn); if(idx<0||idx>=CMM_MYSQL_MAX||!g_mysql[idx].used) return cmm_str(g_mysql_last_err); return cmm_str(g_mysql[idx].err); }
CmmValue cmm_mysql_close(CmmValue conn){
    int idx=(int)as_int(conn);
    if(idx<0||idx>=CMM_MYSQL_MAX||!g_mysql[idx].used) return cmm_bool(0);
    MysqlConn *c=&g_mysql[idx];
    my_tab_lock();
    c->seq=0; unsigned char qb=0x01; my_write_packet(c,&qb,1);   /* COM_QUIT */
#ifdef CMM_HAVE_TLS
    if(c->tls_on){ mbedtls_ssl_close_notify(&c->ssl); mbedtls_ssl_free(&c->ssl); c->tls_on=0; }
    if(c->have_conf){ mbedtls_ssl_config_free(&c->tconf); mbedtls_x509_crt_free(&c->tca); c->have_conf=0; }
#endif
    CLOSESOCK(c->fd); c->used=0;
    my_tab_unlock();
    return cmm_bool(1);
}


/* ===== Regex engine: backtracking bytecode VM + PHP-style preg API ===== */
/* Supports: literals, . , char classes [..] with ranges + \d\w\s\D\W\S,     */
/* anchors ^ $ \b \B, quantifiers * + ? {n} {n,} {n,m} and lazy variants,    */
/* groups ( ) and (?: ), alternation |, backreferences \1..\9, and the       */
/* i / m / s flags. PHP-delimited patterns like  /re/flags .                 */

#define RX_MAXGROUP 32
enum { RXI_CHAR, RXI_ANY, RXI_CLASS, RXI_MATCH, RXI_JMP, RXI_SPLIT,
       RXI_SAVE, RXI_BOL, RXI_EOL, RXI_WORDB, RXI_NWORDB, RXI_BACKREF };
typedef struct { int op; unsigned char c; int x, y; unsigned char set[32]; } RxInst;
typedef struct { RxInst *insts; int n, cap; int ngroup; } RxProg;
typedef struct { int icase, multiline, dotall; } RxFlags;

/* --- AST --- */
enum { RXN_CHAR, RXN_ANY, RXN_CLASS, RXN_CONCAT, RXN_ALT, RXN_STAR, RXN_PLUS,
       RXN_QUEST, RXN_REPEAT, RXN_GROUP, RXN_BOL, RXN_EOL, RXN_WORDB, RXN_NWORDB,
       RXN_BACKREF, RXN_EMPTY };
typedef struct RxNode {
    int type;
    unsigned char c;
    unsigned char set[32];
    struct RxNode **kids; int nkids;
    struct RxNode *kid;
    int greedy, min, max, group, ref;
} RxNode;

typedef struct { const char *p; const char *end; int ngroup; int err; RxFlags fl; } RxParser;

static void rx_set_bit(unsigned char *s, unsigned char c){ s[c>>3] |= (1u<<(c&7)); }
static int  rx_get_bit(const unsigned char *s, unsigned char c){ return (s[c>>3]>>(c&7))&1; }
static int  rx_is_word(unsigned char c){ return isalnum(c) || c=='_'; }

static void rx_class_shorthand(unsigned char *set, char ch){
    int i;
    switch(ch){
        case 'd': for(i='0';i<='9';i++) rx_set_bit(set,(unsigned char)i); break;
        case 'D': for(i=0;i<256;i++) if(!(i>='0'&&i<='9')) rx_set_bit(set,(unsigned char)i); break;
        case 'w': for(i=0;i<256;i++) if(rx_is_word((unsigned char)i)) rx_set_bit(set,(unsigned char)i); break;
        case 'W': for(i=0;i<256;i++) if(!rx_is_word((unsigned char)i)) rx_set_bit(set,(unsigned char)i); break;
        case 's': { const char *ws=" \t\n\r\f\v"; for(const char*w=ws;*w;w++) rx_set_bit(set,(unsigned char)*w); } break;
        case 'S': { unsigned char tmp[32]; memset(tmp,0,32); const char *ws=" \t\n\r\f\v"; for(const char*w=ws;*w;w++) rx_set_bit(tmp,(unsigned char)*w);
                    for(i=0;i<256;i++) if(!rx_get_bit(tmp,(unsigned char)i)) rx_set_bit(set,(unsigned char)i); } break;
    }
}

static RxNode *rx_node(int type){ RxNode *n=(RxNode*)calloc(1,sizeof(RxNode)); n->type=type; n->greedy=1; n->min=0; n->max=-1; return n; }
static void rx_free(RxNode *n){ if(!n) return; if(n->kids){ for(int i=0;i<n->nkids;i++) rx_free(n->kids[i]); free(n->kids); } rx_free(n->kid); free(n); }

static RxNode *rx_parse_alt(RxParser *ps);

/* parse an escaped atom after backslash; returns a node */
static RxNode *rx_parse_escape(RxParser *ps){
    if(ps->p>=ps->end){ ps->err=1; return rx_node(RXN_EMPTY); }
    char c=*ps->p++;
    if(c=='d'||c=='D'||c=='w'||c=='W'||c=='s'||c=='S'){ RxNode*n=rx_node(RXN_CLASS); rx_class_shorthand(n->set,c); return n; }
    if(c=='b'){ return rx_node(RXN_WORDB); }
    if(c=='B'){ return rx_node(RXN_NWORDB); }
    if(c>='1'&&c<='9'){ RxNode*n=rx_node(RXN_BACKREF); n->ref=c-'0'; return n; }
    RxNode*n=rx_node(RXN_CHAR);
    switch(c){ case 'n': n->c='\n'; break; case 't': n->c='\t'; break; case 'r': n->c='\r'; break;
               case 'f': n->c='\f'; break; case 'v': n->c='\v'; break; case '0': n->c='\0'; break;
               default: n->c=(unsigned char)c; }
    return n;
}

static RxNode *rx_parse_class(RxParser *ps){
    RxNode *n=rx_node(RXN_CLASS);
    int neg=0;
    if(ps->p<ps->end && *ps->p=='^'){ neg=1; ps->p++; }
    unsigned char tmp[32]; memset(tmp,0,32);
    /* a leading ] is a literal */
    int first=1;
    while(ps->p<ps->end && (*ps->p!=']' || first)){
        first=0;
        unsigned char lo;
        if(*ps->p=='\\'){
            ps->p++;
            if(ps->p>=ps->end) break;
            char e=*ps->p++;
            if(e=='d'||e=='D'||e=='w'||e=='W'||e=='s'||e=='S'){ rx_class_shorthand(tmp,e); continue; }
            switch(e){ case 'n': lo='\n'; break; case 't': lo='\t'; break; case 'r': lo='\r'; break;
                       case 'f': lo='\f'; break; case 'v': lo='\v'; break; default: lo=(unsigned char)e; }
        } else { lo=(unsigned char)*ps->p++; }
        /* range? */
        if(ps->p+1<ps->end && *ps->p=='-' && ps->p[1]!=']'){
            ps->p++; /* consume '-' */
            unsigned char hi;
            if(*ps->p=='\\'){ ps->p++; char e=*ps->p++; switch(e){ case 'n': hi='\n'; break; case 't': hi='\t'; break; case 'r': hi='\r'; break; default: hi=(unsigned char)e; } }
            else hi=(unsigned char)*ps->p++;
            for(int c=lo;c<=hi;c++) rx_set_bit(tmp,(unsigned char)c);
        } else {
            rx_set_bit(tmp,lo);
        }
    }
    if(ps->p<ps->end && *ps->p==']') ps->p++; else ps->err=1;
    if(neg){ for(int i=0;i<256;i++) if(!rx_get_bit(tmp,(unsigned char)i)) rx_set_bit(n->set,(unsigned char)i); }
    else memcpy(n->set,tmp,32);
    return n;
}

static RxNode *rx_parse_atom(RxParser *ps){
    if(ps->p>=ps->end) return rx_node(RXN_EMPTY);
    char c=*ps->p;
    if(c=='('){
        ps->p++;
        int cap=1, grp=0;
        if(ps->p+1<ps->end && ps->p[0]=='?' && ps->p[1]==':'){ cap=0; ps->p+=2; }
        if(cap){ grp = ++ps->ngroup; }
        RxNode *sub=rx_parse_alt(ps);
        if(ps->p<ps->end && *ps->p==')') ps->p++; else ps->err=1;
        RxNode *g=rx_node(RXN_GROUP); g->group=cap?grp:0; g->kid=sub; return g;
    }
    if(c=='['){ ps->p++; return rx_parse_class(ps); }
    if(c=='.'){ ps->p++; return rx_node(RXN_ANY); }
    if(c=='^'){ ps->p++; return rx_node(RXN_BOL); }
    if(c=='$'){ ps->p++; return rx_node(RXN_EOL); }
    if(c=='\\'){ ps->p++; return rx_parse_escape(ps); }
    ps->p++;
    RxNode *n=rx_node(RXN_CHAR); n->c=(unsigned char)c; return n;
}

static RxNode *rx_parse_repeat(RxParser *ps){
    RxNode *atom=rx_parse_atom(ps);
    while(ps->p<ps->end){
        char c=*ps->p;
        RxNode *q=NULL;
        if(c=='*'){ q=rx_node(RXN_STAR); ps->p++; }
        else if(c=='+'){ q=rx_node(RXN_PLUS); ps->p++; }
        else if(c=='?'){ q=rx_node(RXN_QUEST); ps->p++; }
        else if(c=='{'){
            const char *save=ps->p; ps->p++;
            int mn=0, mx=-1, hasMin=0;
            while(ps->p<ps->end && isdigit((unsigned char)*ps->p)){ mn=mn*10+(*ps->p-'0'); ps->p++; hasMin=1; }
            if(ps->p<ps->end && *ps->p==','){ ps->p++;
                if(ps->p<ps->end && isdigit((unsigned char)*ps->p)){ mx=0; while(ps->p<ps->end && isdigit((unsigned char)*ps->p)){ mx=mx*10+(*ps->p-'0'); ps->p++; } }
                else mx=-1;
            } else { mx=mn; }
            if(ps->p<ps->end && *ps->p=='}' && hasMin){ ps->p++; q=rx_node(RXN_REPEAT); q->min=mn; q->max=mx; }
            else { ps->p=save; break; }  /* not a quantifier, treat '{' literally elsewhere */
        }
        else break;
        /* lazy? */
        if(ps->p<ps->end && *ps->p=='?'){ q->greedy=0; ps->p++; }
        q->kid=atom; atom=q;
    }
    return atom;
}

static RxNode *rx_parse_concat(RxParser *ps){
    RxNode *list=rx_node(RXN_CONCAT);
    list->kids=(RxNode**)malloc(sizeof(RxNode*)*8); int cap=8;
    while(ps->p<ps->end && *ps->p!='|' && *ps->p!=')'){
        RxNode *r=rx_parse_repeat(ps);
        if(list->nkids>=cap){ cap*=2; list->kids=(RxNode**)realloc(list->kids,sizeof(RxNode*)*cap); }
        list->kids[list->nkids++]=r;
        if(ps->err) break;
    }
    return list;
}

static RxNode *rx_parse_alt(RxParser *ps){
    RxNode *left=rx_parse_concat(ps);
    if(ps->p<ps->end && *ps->p=='|'){
        RxNode *alt=rx_node(RXN_ALT);
        alt->kids=(RxNode**)malloc(sizeof(RxNode*)*4); int cap=4;
        alt->kids[alt->nkids++]=left;
        while(ps->p<ps->end && *ps->p=='|'){
            ps->p++;
            RxNode *r=rx_parse_concat(ps);
            if(alt->nkids>=cap){ cap*=2; alt->kids=(RxNode**)realloc(alt->kids,sizeof(RxNode*)*cap); }
            alt->kids[alt->nkids++]=r;
        }
        return alt;
    }
    return left;
}

/* --- compile AST -> program --- */
static int rx_emit(RxProg *pr, RxInst in){ if(pr->n>=pr->cap){ pr->cap=pr->cap?pr->cap*2:64; pr->insts=(RxInst*)realloc(pr->insts,pr->cap*sizeof(RxInst)); } pr->insts[pr->n]=in; return pr->n++; }
static RxInst rx_mk(int op){ RxInst i; memset(&i,0,sizeof i); i.op=op; return i; }

static void rx_compile(RxProg *pr, RxNode *n, RxFlags fl);

static void rx_compile_quant_star(RxProg *pr, RxNode *kid, int greedy, RxFlags fl){
    RxInst sp=rx_mk(RXI_SPLIT); int spi=rx_emit(pr,sp);
    int start=pr->n; rx_compile(pr,kid,fl);
    RxInst jm=rx_mk(RXI_JMP); jm.x=spi; rx_emit(pr,jm);
    int after=pr->n;
    if(greedy){ pr->insts[spi].x=start; pr->insts[spi].y=after; }
    else { pr->insts[spi].x=after; pr->insts[spi].y=start; }
}
static void rx_compile_quant_quest(RxProg *pr, RxNode *kid, int greedy, RxFlags fl){
    RxInst sp=rx_mk(RXI_SPLIT); int spi=rx_emit(pr,sp);
    int start=pr->n; rx_compile(pr,kid,fl); int after=pr->n;
    if(greedy){ pr->insts[spi].x=start; pr->insts[spi].y=after; }
    else { pr->insts[spi].x=after; pr->insts[spi].y=start; }
}

static void rx_compile(RxProg *pr, RxNode *n, RxFlags fl){
    if(!n) return;
    switch(n->type){
        case RXN_EMPTY: break;
        case RXN_CHAR: { RxInst i=rx_mk(RXI_CHAR); i.c=n->c; rx_emit(pr,i); } break;
        case RXN_ANY:  rx_emit(pr,rx_mk(RXI_ANY)); break;
        case RXN_CLASS: { RxInst i=rx_mk(RXI_CLASS); memcpy(i.set,n->set,32);
            if(fl.icase){ for(int c='a';c<='z';c++){ if(rx_get_bit(i.set,(unsigned char)c)) rx_set_bit(i.set,(unsigned char)(c-32)); }
                          for(int c='A';c<='Z';c++){ if(rx_get_bit(i.set,(unsigned char)c)) rx_set_bit(i.set,(unsigned char)(c+32)); } }
            rx_emit(pr,i); } break;
        case RXN_BOL: rx_emit(pr,rx_mk(RXI_BOL)); break;
        case RXN_EOL: rx_emit(pr,rx_mk(RXI_EOL)); break;
        case RXN_WORDB: rx_emit(pr,rx_mk(RXI_WORDB)); break;
        case RXN_NWORDB: rx_emit(pr,rx_mk(RXI_NWORDB)); break;
        case RXN_BACKREF: { RxInst i=rx_mk(RXI_BACKREF); i.x=n->ref; rx_emit(pr,i); } break;
        case RXN_CONCAT: for(int i=0;i<n->nkids;i++) rx_compile(pr,n->kids[i],fl); break;
        case RXN_ALT: {
            int *jmps=(int*)malloc(sizeof(int)*n->nkids); int nj=0;
            for(int i=0;i<n->nkids-1;i++){
                RxInst sp=rx_mk(RXI_SPLIT); int spi=rx_emit(pr,sp);
                int start=pr->n; rx_compile(pr,n->kids[i],fl);
                RxInst jm=rx_mk(RXI_JMP); int ji=rx_emit(pr,jm); jmps[nj++]=ji;
                int next=pr->n; pr->insts[spi].x=start; pr->insts[spi].y=next;
            }
            rx_compile(pr,n->kids[n->nkids-1],fl);
            int end=pr->n; for(int i=0;i<nj;i++) pr->insts[jmps[i]].x=end;
            free(jmps);
        } break;
        case RXN_STAR:  rx_compile_quant_star(pr,n->kid,n->greedy,fl); break;
        case RXN_QUEST: rx_compile_quant_quest(pr,n->kid,n->greedy,fl); break;
        case RXN_PLUS: {
            int start=pr->n; rx_compile(pr,n->kid,fl);
            RxInst sp=rx_mk(RXI_SPLIT); int spi=rx_emit(pr,sp);
            if(n->greedy){ pr->insts[spi].x=start; pr->insts[spi].y=pr->n; }
            else { pr->insts[spi].x=pr->n; pr->insts[spi].y=start; }
        } break;
        case RXN_REPEAT: {
            int mn=n->min, mx=n->max;
            for(int i=0;i<mn;i++) rx_compile(pr,n->kid,fl);
            if(mx<0){ rx_compile_quant_star(pr,n->kid,n->greedy,fl); }
            else { for(int i=0;i<mx-mn;i++) rx_compile_quant_quest(pr,n->kid,n->greedy,fl); }
        } break;
        case RXN_GROUP: {
            if(n->group>0 && n->group<RX_MAXGROUP){ RxInst a=rx_mk(RXI_SAVE); a.x=2*n->group; rx_emit(pr,a); rx_compile(pr,n->kid,fl); RxInst b=rx_mk(RXI_SAVE); b.x=2*n->group+1; rx_emit(pr,b); }
            else rx_compile(pr,n->kid,fl);
        } break;
    }
}

static int rx_chr_eq(char a, unsigned char b, int icase){ return icase ? (tolower((unsigned char)a)==tolower(b)) : ((unsigned char)a==b); }

/* recursive backtracking VM; returns 1 on match, 0 otherwise */
static int rx_run(RxProg *pr, int pc, const char *s, int sp, int slen, int *saves, RxFlags fl, long *budget){
    for(;;){
        if(--(*budget) < 0) return 0;
        RxInst *in=&pr->insts[pc];
        switch(in->op){
            case RXI_CHAR: if(sp<slen && rx_chr_eq(s[sp],in->c,fl.icase)){ sp++; pc++; break; } return 0;
            case RXI_ANY:  if(sp<slen && (fl.dotall || s[sp]!='\n')){ sp++; pc++; break; } return 0;
            case RXI_CLASS: if(sp<slen && rx_get_bit(in->set,(unsigned char)s[sp])){ sp++; pc++; break; } return 0;
            case RXI_BOL: if(sp==0 || (fl.multiline && s[sp-1]=='\n')){ pc++; break; } return 0;
            case RXI_EOL: if(sp==slen || (fl.multiline && s[sp]=='\n')){ pc++; break; } return 0;
            case RXI_WORDB: { int a=(sp>0)&&rx_is_word((unsigned char)s[sp-1]); int b=(sp<slen)&&rx_is_word((unsigned char)s[sp]); if(a!=b){ pc++; break; } return 0; }
            case RXI_NWORDB:{ int a=(sp>0)&&rx_is_word((unsigned char)s[sp-1]); int b=(sp<slen)&&rx_is_word((unsigned char)s[sp]); if(a==b){ pc++; break; } return 0; }
            case RXI_BACKREF: { int g=in->x; int st=saves[2*g], en=saves[2*g+1]; if(st<0||en<0){ pc++; break; } int len=en-st; if(sp+len<=slen){ int ok=1; for(int k=0;k<len;k++){ if(!rx_chr_eq(s[sp+k],(unsigned char)s[st+k],fl.icase)){ ok=0; break; } } if(ok){ sp+=len; pc++; break; } } return 0; }
            case RXI_SAVE: { int slot=in->x; int old=saves[slot]; saves[slot]=sp; if(rx_run(pr,pc+1,s,sp,slen,saves,fl,budget)) return 1; saves[slot]=old; return 0; }
            case RXI_JMP: pc=in->x; break;
            case RXI_SPLIT: if(rx_run(pr,in->x,s,sp,slen,saves,fl,budget)) return 1; pc=in->y; break;
            case RXI_MATCH: return 1;
        }
    }
}

/* Parse a PHP-delimited pattern "/re/flags"; returns malloc'd regex body, fills flags. */
static char *rx_parse_delim(const char *pat, size_t plen, RxFlags *fl){
    memset(fl,0,sizeof *fl);
    if(plen<2) { char *r=(char*)malloc(plen+1); memcpy(r,pat,plen); r[plen]=0; return r; }
    char open=pat[0], close=open;
    if(open=='(') close=')'; else if(open=='[') close=']';
    else if(open=='{') close='}'; else if(open=='<') close='>';
    /* find the last close delimiter */
    int endpos=-1;
    for(int i=(int)plen-1;i>=1;i--){ if(pat[i]==close){ endpos=i; break; } }
    if(endpos<1){ char *r=(char*)malloc(plen+1); memcpy(r,pat,plen); r[plen]=0; return r; }
    for(int i=endpos+1;i<(int)plen;i++){ char f=pat[i]; if(f=='i') fl->icase=1; else if(f=='m') fl->multiline=1; else if(f=='s') fl->dotall=1; }
    int blen=endpos-1;
    char *r=(char*)malloc(blen+1); memcpy(r,pat+1,blen); r[blen]=0; return r;
}

/* Compile a delimited pattern; returns 1 ok (fills *prog, *fl), 0 on error. */
static int rx_compile_pattern(const char *pat, size_t plen, RxProg *prog, RxFlags *fl){
    char *body=rx_parse_delim(pat,plen,fl);
    RxParser ps; ps.p=body; ps.end=body+strlen(body); ps.ngroup=0; ps.err=0; ps.fl=*fl;
    RxNode *ast=rx_parse_alt(&ps);
    memset(prog,0,sizeof *prog);
    RxInst s0=rx_mk(RXI_SAVE); s0.x=0; rx_emit(prog,s0);
    rx_compile(prog,ast,*fl);
    RxInst s1=rx_mk(RXI_SAVE); s1.x=1; rx_emit(prog,s1);
    rx_emit(prog,rx_mk(RXI_MATCH));
    prog->ngroup=ps.ngroup;
    int err=ps.err;
    rx_free(ast); free(body);
    return !err;
}

/* Search from startmin; on match returns start pos and fills saves (size 2*(ngroup+1)). */
static int rx_search(RxProg *prog, const char *s, int slen, int startmin, int *saves, RxFlags fl){
    int nslot=2*(prog->ngroup+1);
    for(int sp0=startmin; sp0<=slen; sp0++){
        for(int i=0;i<nslot;i++) saves[i]=-1;
        long budget=2000000;
        if(rx_run(prog,0,s,sp0,slen,saves,fl,&budget)) return sp0;
    }
    return -1;
}

/* ---- PHP-style preg wrappers ---- */
CmmValue cmm_preg_match(CmmValue pattern, CmmValue subject){
    CmmString *pat=cmm_to_string(pattern), *sub=cmm_to_string(subject);
    RxProg prog; RxFlags fl;
    CmmValue out=cmm_new_list();
    if(!rx_compile_pattern(pat->data,pat->len,&prog,&fl)){ free(prog.insts); return out; }
    int saves[2*(RX_MAXGROUP+1)];
    int st=rx_search(&prog,sub->data,(int)sub->len,0,saves,fl);
    if(st>=0){
        for(int g=0;g<=prog.ngroup;g++){
            int a=saves[2*g], b=saves[2*g+1];
            if(a>=0 && b>=a) list_push(out.list, cmm_str_n(sub->data+a, (size_t)(b-a)));
            else list_push(out.list, cmm_str(""));
        }
    }
    free(prog.insts);
    return out;
}
CmmValue cmm_preg_test(CmmValue pattern, CmmValue subject){
    CmmString *pat=cmm_to_string(pattern), *sub=cmm_to_string(subject);
    RxProg prog; RxFlags fl;
    if(!rx_compile_pattern(pat->data,pat->len,&prog,&fl)){ free(prog.insts); return cmm_bool(0); }
    int saves[2*(RX_MAXGROUP+1)];
    int st=rx_search(&prog,sub->data,(int)sub->len,0,saves,fl);
    free(prog.insts);
    return cmm_bool(st>=0);
}
CmmValue cmm_preg_match_all(CmmValue pattern, CmmValue subject){
    CmmString *pat=cmm_to_string(pattern), *sub=cmm_to_string(subject);
    RxProg prog; RxFlags fl;
    CmmValue out=cmm_new_list();
    if(!rx_compile_pattern(pat->data,pat->len,&prog,&fl)){ free(prog.insts); return out; }
    int saves[2*(RX_MAXGROUP+1)];
    int from=0, slen=(int)sub->len;
    while(from<=slen){
        int st=rx_search(&prog,sub->data,slen,from,saves,fl);
        if(st<0) break;
        int a=saves[0], b=saves[1];
        list_push(out.list, cmm_str_n(sub->data+a,(size_t)(b-a)));
        from = (b>a) ? b : b+1;   /* avoid infinite loop on empty match */
    }
    free(prog.insts);
    return out;
}
CmmValue cmm_preg_replace(CmmValue pattern, CmmValue replacement, CmmValue subject){
    CmmString *pat=cmm_to_string(pattern), *rep=cmm_to_string(replacement), *sub=cmm_to_string(subject);
    RxProg prog; RxFlags fl;
    if(!rx_compile_pattern(pat->data,pat->len,&prog,&fl)){ free(prog.insts); return cmm_str_n(sub->data,sub->len); }
    int saves[2*(RX_MAXGROUP+1)];
    SB b; sb_init(&b);
    int from=0, slen=(int)sub->len;
    while(from<=slen){
        int st=rx_search(&prog,sub->data,slen,from,saves,fl);
        if(st<0) break;
        int ms=saves[0], me=saves[1];
        sb_putn(&b, sub->data+from, (size_t)(ms-from));   /* text before match */
        /* expand replacement with $n / ${n} / \n backrefs */
        for(size_t i=0;i<rep->len;i++){
            char rc=rep->data[i];
            int grp=-1; size_t adv=0;
            if((rc=='$'||rc=='\\') && i+1<rep->len){
                if(rc=='$' && rep->data[i+1]=='{'){
                    size_t j=i+2; int num=0, has=0; while(j<rep->len && isdigit((unsigned char)rep->data[j])){ num=num*10+(rep->data[j]-'0'); j++; has=1; }
                    if(has && j<rep->len && rep->data[j]=='}'){ grp=num; adv=j-i; }
                } else if(isdigit((unsigned char)rep->data[i+1])){
                    grp=rep->data[i+1]-'0'; adv=1;
                }
            }
            if(grp>=0){
                if(grp<=prog.ngroup){ int a=saves[2*grp], e=saves[2*grp+1]; if(a>=0&&e>=a) sb_putn(&b, sub->data+a,(size_t)(e-a)); }
                i+=adv;
            } else {
                sb_putn(&b,&rc,1);
            }
        }
        from = (me>ms) ? me : me+1;
        if(me==ms && ms<slen) sb_putn(&b, sub->data+ms, 1);  /* emit the skipped char on empty match */
    }
    if(from<slen) sb_putn(&b, sub->data+from, (size_t)(slen-from));
    free(prog.insts);
    return sb_to_str(&b);
}
CmmValue cmm_preg_split(CmmValue pattern, CmmValue subject){
    CmmString *pat=cmm_to_string(pattern), *sub=cmm_to_string(subject);
    RxProg prog; RxFlags fl;
    CmmValue out=cmm_new_list();
    if(!rx_compile_pattern(pat->data,pat->len,&prog,&fl)){ list_push(out.list,cmm_str_n(sub->data,sub->len)); free(prog.insts); return out; }
    int saves[2*(RX_MAXGROUP+1)];
    int from=0, last=0, slen=(int)sub->len;
    while(from<=slen){
        int st=rx_search(&prog,sub->data,slen,from,saves,fl);
        if(st<0) break;
        int ms=saves[0], me=saves[1];
        if(me==ms){ from=ms+1; continue; }   /* skip empty separators */
        list_push(out.list, cmm_str_n(sub->data+last,(size_t)(ms-last)));
        last=me; from=me;
    }
    list_push(out.list, cmm_str_n(sub->data+last,(size_t)(slen-last)));
    free(prog.insts);
    return out;
}
CmmValue cmm_preg_quote(CmmValue str){
    CmmString *s=cmm_to_string(str);
    const char *meta=".\\+*?[^]$(){}=!<>|:-#";
    SB b; sb_init(&b);
    for(size_t i=0;i<s->len;i++){ char c=s->data[i]; if(strchr(meta,c)){ char bs='\\'; sb_putn(&b,&bs,1); } sb_putn(&b,&c,1); }
    return sb_to_str(&b);
}

/* ---- Lambda deploy: upload a zip to the Lambda control-plane API ---- */
/* Uses AWS SigV4 over HTTPS, so it requires a TLS build (--tls). Region and  */
/* credentials come from the standard AWS_* environment variables.            */
#ifdef CMM_HAVE_TLS
static void b64_encode(SB *out, const unsigned char *p, size_t n){
    static const char B[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i=0;
    for(; i+3<=n; i+=3){ unsigned v=((unsigned)p[i]<<16)|((unsigned)p[i+1]<<8)|p[i+2];
        char q[4]={B[(v>>18)&63],B[(v>>12)&63],B[(v>>6)&63],B[v&63]}; sb_putn(out,q,4); }
    if(n-i==1){ unsigned v=(unsigned)p[i]<<16; char q[4]={B[(v>>18)&63],B[(v>>12)&63],'=','='}; sb_putn(out,q,4); }
    else if(n-i==2){ unsigned v=((unsigned)p[i]<<16)|((unsigned)p[i+1]<<8);
        char q[4]={B[(v>>18)&63],B[(v>>12)&63],B[(v>>6)&63],'='}; sb_putn(out,q,4); }
}
static void sha256_hex(const unsigned char *d,size_t n,char out[65]){
    unsigned char h[32]; cx_sha256(d,n,h); cx_hex(h,32,out);
}
static void hex32(const unsigned char *h,char out[65]){
    static const char hx[]="0123456789abcdef";
    for(int i=0;i<32;i++){ out[i*2]=hx[(h[i]>>4)&0xF]; out[i*2+1]=hx[h[i]&0xF]; } out[64]=0;
}
static void hmac256(const unsigned char *k,int kl,const unsigned char *d,size_t n,unsigned char out[32]){
    cx_hmac_sha256(k,(size_t)kl,d,n,out);
}
/* pure: compute the SigV4 Authorization header value (deterministic) */
static void aws_sigv4_auth(const char *method,const char *host,const char *path,const char *qs,
                           const char *body,size_t bodylen,const char *region,const char *service,
                           const char *akey,const char *skey,const char *token,
                           const char *amzdate,const char *datestamp,SB *out_auth){
    char payhash[65]; sha256_hex((const unsigned char*)body,bodylen,payhash);
    SB ch; sb_init(&ch); SB sh; sb_init(&sh);
    sb_put(&ch,"content-type:application/json\n");
    sb_put(&ch,"host:"); sb_put(&ch,host); sb_put(&ch,"\n");
    sb_put(&ch,"x-amz-date:"); sb_put(&ch,amzdate); sb_put(&ch,"\n");
    sb_put(&sh,"content-type;host;x-amz-date");
    if(token && *token){ sb_put(&ch,"x-amz-security-token:"); sb_put(&ch,token); sb_put(&ch,"\n");
                         sb_put(&sh,";x-amz-security-token"); }
    SB creq; sb_init(&creq);
    sb_put(&creq,method);    sb_put(&creq,"\n");
    sb_put(&creq,path);      sb_put(&creq,"\n");
    sb_put(&creq,qs?qs:"");  sb_put(&creq,"\n");
    sb_putn(&creq,ch.p,ch.len); sb_put(&creq,"\n");
    sb_putn(&creq,sh.p,sh.len); sb_put(&creq,"\n");
    sb_put(&creq,payhash);
    char creqhash[65]; sha256_hex((const unsigned char*)creq.p,creq.len,creqhash);
    char scope[160]; snprintf(scope,sizeof scope,"%s/%s/%s/aws4_request",datestamp,region,service);
    SB sts; sb_init(&sts);
    sb_put(&sts,"AWS4-HMAC-SHA256\n"); sb_put(&sts,amzdate); sb_put(&sts,"\n");
    sb_put(&sts,scope); sb_put(&sts,"\n"); sb_put(&sts,creqhash);
    char k0[300]; int k0n=snprintf(k0,sizeof k0,"AWS4%s",skey);
    unsigned char kd[32],kr[32],ks[32],ksig[32],sig[32];
    hmac256((unsigned char*)k0,k0n,(const unsigned char*)datestamp,strlen(datestamp),kd);
    hmac256(kd,32,(const unsigned char*)region,strlen(region),kr);
    hmac256(kr,32,(const unsigned char*)service,strlen(service),ks);
    hmac256(ks,32,(const unsigned char*)"aws4_request",12,ksig);
    hmac256(ksig,32,(const unsigned char*)sts.p,sts.len,sig);
    char sighex[65]; hex32(sig,sighex);
    sb_put(out_auth,"AWS4-HMAC-SHA256 Credential="); sb_put(out_auth,akey);
    sb_put(out_auth,"/"); sb_put(out_auth,scope);
    sb_put(out_auth,", SignedHeaders="); sb_putn(out_auth,sh.p,sh.len);
    sb_put(out_auth,", Signature="); sb_put(out_auth,sighex);
    free(ch.p); free(sh.p); free(creq.p); free(sts.p);
}
static CmmValue lambda_sigv4_send(const char *method,const char *path,const char *body,size_t bodylen){
    const char *region=getenv("AWS_REGION"); if(!region||!*region) region=getenv("AWS_DEFAULT_REGION");
    const char *akey=getenv("AWS_ACCESS_KEY_ID");
    const char *skey=getenv("AWS_SECRET_ACCESS_KEY");
    const char *token=getenv("AWS_SESSION_TOKEN");
    if(!region||!*region||!akey||!*akey||!skey||!*skey)
        return cmm_str("{\"error\":\"missing AWS_REGION or AWS credentials in environment\"}");
    char host[300]; snprintf(host,sizeof host,"lambda.%s.amazonaws.com",region);
    time_t now=time(NULL); struct tm g;
#ifdef _WIN32
    gmtime_s(&g,&now);
#else
    gmtime_r(&now,&g);
#endif
    char amzdate[20], datestamp[10];
    strftime(amzdate,sizeof amzdate,"%Y%m%dT%H%M%SZ",&g);
    strftime(datestamp,sizeof datestamp,"%Y%m%d",&g);
    SB auth; sb_init(&auth);
    aws_sigv4_auth(method,host,path,"",body,bodylen,region,"lambda",akey,skey,token,amzdate,datestamp,&auth);
    SB req; sb_init(&req); char line[400];
    snprintf(line,sizeof line,"%s %s HTTP/1.1\r\n",method,path); sb_put(&req,line);
    snprintf(line,sizeof line,"Host: %s\r\n",host); sb_put(&req,line);
    sb_put(&req,"Content-Type: application/json\r\n");
    snprintf(line,sizeof line,"X-Amz-Date: %s\r\n",amzdate); sb_put(&req,line);
    if(token&&*token){ snprintf(line,sizeof line,"X-Amz-Security-Token: %s\r\n",token); sb_put(&req,line); }
    sb_put(&req,"Authorization: "); sb_putn(&req,auth.p,auth.len); sb_put(&req,"\r\n");
    snprintf(line,sizeof line,"Content-Length: %zu\r\nConnection: close\r\n\r\n",bodylen); sb_put(&req,line);
    sb_putn(&req,body,bodylen);
    free(auth.p);
    size_t reqlen=req.len; char *buf=(char*)malloc(reqlen?reqlen:1);
    memcpy(buf,req.p,reqlen); free(req.p);
    CmmValue resp=http_exchange_tls(host,443,buf,reqlen); free(buf);
    return http_body(resp);
}
CmmValue cmm_lambda_create(CmmValue name, CmmValue role, CmmValue zip){
    json_tbl();
    CmmString *n=cmm_to_string(name), *r=cmm_to_string(role), *z=cmm_to_string(zip);
    SB b; sb_init(&b);
    sb_put(&b,"{\"FunctionName\":"); json_str(&b,n->data,n->len);
    sb_put(&b,",\"Runtime\":\"provided.al2023\",\"Role\":"); json_str(&b,r->data,r->len);
    sb_put(&b,",\"Handler\":\"bootstrap\",\"PackageType\":\"Zip\",\"Code\":{\"ZipFile\":\"");
    b64_encode(&b,(const unsigned char*)z->data,z->len);
    sb_put(&b,"\"}}");
    CmmValue resp=lambda_sigv4_send("POST","/2015-03-31/functions/",b.p,b.len);
    free(b.p);
    return resp;
}
CmmValue cmm_lambda_update_code(CmmValue name, CmmValue zip){
    CmmString *n=cmm_to_string(name), *z=cmm_to_string(zip);
    SB b; sb_init(&b);
    sb_put(&b,"{\"ZipFile\":\""); b64_encode(&b,(const unsigned char*)z->data,z->len); sb_put(&b,"\"}");
    char path[320]; snprintf(path,sizeof path,"/2015-03-31/functions/%s/code",n->data);
    CmmValue resp=lambda_sigv4_send("PUT",path,b.p,b.len);
    free(b.p);
    return resp;
}
#else
CmmValue cmm_lambda_create(CmmValue name, CmmValue role, CmmValue zip){
    (void)name;(void)role;(void)zip;
    return cmm_str("{\"error\":\"Lambda deploy requires a TLS build (compile with --tls)\"}");
}
CmmValue cmm_lambda_update_code(CmmValue name, CmmValue zip){
    (void)name;(void)zip;
    return cmm_str("{\"error\":\"Lambda deploy requires a TLS build (compile with --tls)\"}");
}
#endif

/* ======================================================================== */
/* Data: first-class accessors over the flexible value tree that JSON       */
/* decodes into. The decoded value IS an ordinary cmm dict/list/value, so   */
/* these make navigating and reshaping it ergonomic, and you hand the same  */
/* structure straight back to Json.encode.                                  */
/* ======================================================================== */
static double cmm_as_double(CmmValue v){
    if(v.tag==CV_FLOAT) return v.f;
    if(v.tag==CV_INT)   return (double)v.i;
    if(v.tag==CV_BOOL)  return v.b?1.0:0.0;
    if(v.tag==CV_STRING && v.s) return strtod(v.s->data,NULL);
    return 0.0;
}
CmmValue cmm_data_type(CmmValue d){
    const char *t;
    switch(d.tag){
        case CV_DICT:   t="object"; break;
        case CV_LIST:   t="array";  break;
        case CV_STRING: t="string"; break;
        case CV_INT:    t="int";    break;
        case CV_FLOAT:  t="float";  break;
        case CV_BOOL:   t="bool";   break;
        default:        t="null";
    }
    return cmm_str(t);
}
CmmValue cmm_data_is_object(CmmValue d){ return cmm_bool(d.tag==CV_DICT); }
CmmValue cmm_data_is_array(CmmValue d){ return cmm_bool(d.tag==CV_LIST); }
CmmValue cmm_data_is_null(CmmValue d){ return cmm_bool(d.tag==CV_EMPTY); }
CmmValue cmm_data_at(CmmValue d, CmmValue idx){
    if(d.tag==CV_LIST) return cmm_index_get(d, idx);
    return cmm_empty();
}
CmmValue cmm_data_get_str(CmmValue d, CmmValue key){
    CmmValue v=cmm_index_get(d,key); CmmString *s=cmm_to_string(v);
    CmmValue r; r.tag=CV_STRING; r.s=s; return r;
}
CmmValue cmm_data_get_int(CmmValue d, CmmValue key){ return cmm_int(as_int(cmm_index_get(d,key))); }
CmmValue cmm_data_get_float(CmmValue d, CmmValue key){ return cmm_float(cmm_as_double(cmm_index_get(d,key))); }
CmmValue cmm_data_get_bool(CmmValue d, CmmValue key){ return cmm_bool(cmm_truthy(cmm_index_get(d,key))); }
/* dotted path: "a.b.0.c" (numeric segments index arrays). Missing -> empty. */
CmmValue cmm_data_path(CmmValue d, CmmValue path){
    CmmString *ps=cmm_to_string(path);
    const char *p=ps->data, *e=p+ps->len;
    CmmValue cur=d;
    while(p<e){
        const char *seg=p; while(p<e && *p!='.') p++;
        size_t n=(size_t)(p-seg);
        if(p<e) p++;                       /* skip '.' */
        if(n==0) continue;
        if(cur.tag==CV_LIST){
            int num=1; for(size_t k=0;k<n;k++) if(seg[k]<'0'||seg[k]>'9'){num=0;break;}
            if(!num) return cmm_empty();
            int64_t ix=0; for(size_t k=0;k<n;k++) ix=ix*10+(int64_t)(seg[k]-'0');
            cur=cmm_index_get(cur, cmm_int(ix));
        } else if(cur.tag==CV_DICT){
            cur=cmm_index_get(cur, cmm_str_n(seg,n));
        } else {
            return cmm_empty();
        }
    }
    return cur;
}
