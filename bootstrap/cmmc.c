#if !defined(_WIN32)
#  define _XOPEN_SOURCE 700
#  define _DEFAULT_SOURCE 1
#endif
/* cmmc — a self-contained native compiler for the cmm language.
 *
 * This is a C port of the original Python frontend (lexer, parser, analyzer,
 * code generator and driver). It depends only on a C compiler at runtime
 * (which the toolchain already requires to link programs), so the toolchain
 * ships as a single native binary with no interpreter dependency.
 *
 * Pipeline:  .cmm source -> tokens -> AST -> analyzed AST -> C99 -> native.
 *
 * The cmm runtime (cmm_runtime.c / .h) is embedded in this binary (see
 * embedded_runtime.h) and written to a temporary directory at build time, so
 * no separate runtime files need to be distributed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <ctype.h>

#include "embedded_runtime.h"

#ifdef _WIN32
  #define PATHSEP '\\'
  #define NULLDEV "NUL"
  #define EXESUF  ".exe"
#else
  #define PATHSEP '/'
  #define NULLDEV "/dev/null"
  #define EXESUF  ""
#endif

/* ===================================================================== */
/* Allocation + small utilities                                          */
/* ===================================================================== */
static void *xmalloc(size_t n){ void *p=malloc(n?n:1); if(!p){fprintf(stderr,"cmmc: out of memory\n");exit(1);} return p; }
static void *xrealloc(void *q,size_t n){ void *p=realloc(q,n?n:1); if(!p){fprintf(stderr,"cmmc: out of memory\n");exit(1);} return p; }
static char *xstrdup(const char *s){ size_t n=strlen(s)+1; char *p=(char*)xmalloc(n); memcpy(p,s,n); return p; }
static char *xstrndup(const char *s,size_t n){ char *p=(char*)xmalloc(n+1); memcpy(p,s,n); p[n]=0; return p; }

/* printf into a freshly allocated string */
static char *sfmt(const char *fmt, ...){
    va_list ap; va_start(ap,fmt);
    va_list ap2; va_copy(ap2,ap);
    int n=vsnprintf(NULL,0,fmt,ap); va_end(ap);
    char *buf=(char*)xmalloc((size_t)n+1);
    vsnprintf(buf,(size_t)n+1,fmt,ap2); va_end(ap2);
    return buf;
}

/* growable pointer vector */
typedef struct { void **data; int len, cap; } Vec;
static void vec_push(Vec *v, void *p){
    if(v->len==v->cap){ v->cap=v->cap?v->cap*2:8; v->data=(void**)xrealloc(v->data,(size_t)v->cap*sizeof(void*)); }
    v->data[v->len++]=p;
}

/* growable text buffer */
typedef struct { char *p; size_t len, cap; } Buf;
static void buf_addn(Buf *b, const char *s, size_t n){
    if(b->len+n+1>b->cap){ while(b->len+n+1>b->cap) b->cap=b->cap?b->cap*2:256; b->p=(char*)xrealloc(b->p,b->cap); }
    memcpy(b->p+b->len,s,n); b->len+=n; b->p[b->len]=0;
}
static void buf_add(Buf *b, const char *s){ buf_addn(b,s,strlen(s)); }
static void buf_addf(Buf *b, const char *fmt, ...){
    va_list ap; va_start(ap,fmt); va_list ap2; va_copy(ap2,ap);
    int n=vsnprintf(NULL,0,fmt,ap); va_end(ap);
    char *t=(char*)xmalloc((size_t)n+1); vsnprintf(t,(size_t)n+1,fmt,ap2); va_end(ap2);
    buf_addn(b,t,(size_t)n); free(t);
}

static int set_has(const char *const *arr, int n, const char *s){
    for(int i=0;i<n;i++) if(strcmp(arr[i],s)==0) return 1;
    return 0;
}

/* indentation string of 4*ind spaces (freshly allocated) */
static char *pads(int ind){
    int n=ind*4; char *s=(char*)xmalloc((size_t)n+1);
    memset(s,' ',(size_t)n); s[n]=0; return s;
}

/* ===================================================================== */
/* Diagnostics                                                           */
/* ===================================================================== */
static void die_at(const char *file,int line,int col,const char *fmt,...){
    va_list ap; va_start(ap,fmt);
    fprintf(stderr,"%s:%d:%d: error: ",file?file:"<input>",line,col);
    vfprintf(stderr,fmt,ap); fputc('\n',stderr); va_end(ap);
    exit(1);
}
static void die(const char *fmt,...){
    va_list ap; va_start(ap,fmt);
    fprintf(stderr,"error: "); vfprintf(stderr,fmt,ap); fputc('\n',stderr); va_end(ap);
    exit(1);
}

/* ===================================================================== */
/* Type model                                                            */
/* ===================================================================== */
typedef struct Type { const char *name; struct Type **args; int nargs; } Type;

static Type *type_new(const char *name, Type **args, int nargs){
    Type *t=(Type*)xmalloc(sizeof(Type));
    t->name=name; t->nargs=nargs;
    if(nargs){ t->args=(Type**)xmalloc(sizeof(Type*)*(size_t)nargs); for(int i=0;i<nargs;i++) t->args[i]=args[i]; }
    else t->args=NULL;
    return t;
}
static Type *type0(const char *name){ return type_new(name,NULL,0); }

static Type *T_INT, *T_FLOAT, *T_BOOL, *T_STRING, *T_DATA, *T_VOID, *T_SOCKET;
static Type *T_ELEM, *T_VAL;   /* sentinels for generic value-method returns */

static Type *list_of(Type *t){ Type *a[1]={t}; return type_new("List",a,1); }
static Type *dict_of(Type *k, Type *v){ Type *a[2]={k,v}; return type_new("Dict",a,2); }
static Type *job_of(Type *t){ Type *a[1]={t}; return type_new("Job",a,1); }
static Type *namespace_of(const char *n){ Type *a[1]={type0(n)}; return type_new("Namespace",a,1); }

static int type_eq(Type *a, Type *b){
    if(a==b) return 1;
    if(!a||!b) return 0;
    if(strcmp(a->name,b->name)!=0) return 0;
    if(a->nargs!=b->nargs) return 0;
    for(int i=0;i<a->nargs;i++) if(!type_eq(a->args[i],b->args[i])) return 0;
    return 1;
}
static int type_is_numeric(Type *t){ return strcmp(t->name,"Int")==0 || strcmp(t->name,"Float")==0; }

static const char *PRIMS[] = {"Int","Float","Bool","String","Data"};
static const char *NATIVE_NS[] = {"Console","Math","File","Date","Json","Socket","Http","Sys","Lambda","Zip","Crypto","Base64","Mysql","Preg"};
static const char *NATIVE_CLS[] = {"String","Int","Float","Bool","List","Dict","Data","Json",
                                   "File","Socket","Console","Date","Math","Job","Http","Sys","Lambda","Zip","Crypto","Base64","Mysql","Preg"};
static int is_native_ns(const char *s){ return set_has(NATIVE_NS,(int)(sizeof NATIVE_NS/sizeof*NATIVE_NS),s); }
static int is_native_cls(const char *s){ return set_has(NATIVE_CLS,(int)(sizeof NATIVE_CLS/sizeof*NATIVE_CLS),s); }

/* ---- namespace method table: (ns, method) -> cfn, ret, arity(-1=any) ---- */
typedef struct { const char *ns, *method, *cfn; Type **ret; int arity; } NSMethod;
/* ret stored as Type** (pointer to the global slot) so the singletons are
 * initialised before use at runtime. */
static NSMethod *NSM;     /* filled by init_tables */
static int NSM_N;
typedef struct { const char *base, *method, *cfn; Type **ret; int arity; } VMethod;
static VMethod *VM;
static int VM_N;

/* storage for ret slots */
static Type *RT_list_str;   /* List[String] */
static Type *RT_list_data;  /* List[Data] */

static void init_tables(void){
    T_INT=type0("Int"); T_FLOAT=type0("Float"); T_BOOL=type0("Bool");
    T_STRING=type0("String"); T_DATA=type0("Data"); T_VOID=type0("Void");
    T_SOCKET=type0("Socket");
    T_ELEM=type0("_elem"); T_VAL=type0("_val");
    RT_list_str=list_of(T_STRING);
    RT_list_data=list_of(T_DATA);

    static NSMethod nsm[] = {
        {"Console","print","cmm_console_print",&T_VOID,-1},
        {"Console","println","cmm_console_println",&T_VOID,-1},
        {"Console","read","cmm_console_read",&T_STRING,0},
        {"Math","sqrt","cmm_math_sqrt",&T_FLOAT,1},
        {"Math","abs","cmm_math_abs",&T_FLOAT,1},
        {"Math","pow","cmm_math_pow",&T_FLOAT,2},
        {"Math","floor","cmm_math_floor",&T_INT,1},
        {"Math","ceil","cmm_math_ceil",&T_INT,1},
        {"Math","min","cmm_math_min",&T_FLOAT,2},
        {"Math","max","cmm_math_max",&T_FLOAT,2},
        {"Math","random","cmm_math_random",&T_FLOAT,0},
        {"Math","pi","cmm_math_pi",&T_FLOAT,0},
        {"File","read","cmm_file_read",&T_STRING,1},
        {"File","write","cmm_file_write",&T_BOOL,2},
        {"File","append","cmm_file_append",&T_BOOL,2},
        {"File","exists","cmm_file_exists",&T_BOOL,1},
        {"File","delete","cmm_file_delete",&T_BOOL,1},
        {"Date","now","cmm_date_now",&T_INT,0},
        {"Date","format","cmm_date_format",&T_STRING,2},
        {"Date","amzDate","cmm_date_amz",&T_STRING,0},
        {"Date","date","cmm_date_date",&T_STRING,2},
        {"Date","gmdate","cmm_date_gmdate",&T_STRING,2},
        {"Json","encode","cmm_json_encode",&T_STRING,1},
        {"Json","decode","cmm_json_decode",&T_DATA,1},
        {"Json","pretty","cmm_json_pretty",&T_STRING,1},
        {"Json","parse","cmm_json_decode",&T_DATA,1},
        {"Json","stringify","cmm_json_encode",&T_STRING,1},
        {"Socket","connect","cmm_socket_connect",&T_SOCKET,2},
        {"Http","get","cmm_http_get",&T_STRING,1},
        {"Http","post","cmm_http_post",&T_STRING,2},
        {"Http","request","cmm_http_request",&T_STRING,4},
        {"Sys","exit","cmm_sys_exit",&T_VOID,1},
        {"Sys","exec","cmm_sys_run",&T_STRING,1},
        {"Sys","shell","cmm_sys_shell",&T_INT,1},
        {"Sys","env","cmm_sys_env",&T_STRING,1},
        {"Sys","args","cmm_sys_args",&RT_list_str,0},
        {"Sys","cwd","cmm_sys_cwd",&T_STRING,0},
        {"Sys","chdir","cmm_sys_chdir",&T_BOOL,1},
        {"Sys","peakRss","cmm_sys_peak_rss",&T_INT,0},
        {"Lambda","next","cmm_lambda_next",&T_STRING,0},
        {"Lambda","success","cmm_lambda_success",&T_BOOL,1},
        {"Lambda","failure","cmm_lambda_failure",&T_BOOL,2},
        {"Lambda","initError","cmm_lambda_init_error",&T_BOOL,2},
        {"Lambda","requestId","cmm_lambda_request_id",&T_STRING,0},
        {"Lambda","deadlineMs","cmm_lambda_deadline",&T_INT,0},
        {"Lambda","invokedArn","cmm_lambda_arn",&T_STRING,0},
        {"Lambda","traceId","cmm_lambda_trace",&T_STRING,0},
        {"Lambda","log","cmm_lambda_log",&T_VOID,1},
        {"Zip","build","cmm_zip_build",&T_STRING,1},
        {"Zip","unzip","cmm_zip_unzip",&T_INT,2},
        {"Http","send","cmm_http_send",&T_DATA,4},
        {"Crypto","sha256Hex","cmm_crypto_sha256hex",&T_STRING,1},
        {"Crypto","sha1Hex","cmm_crypto_sha1hex",&T_STRING,1},
        {"Crypto","hmacSha256","cmm_crypto_hmac_sha256",&T_STRING,2},
        {"Crypto","hmacSha256Hex","cmm_crypto_hmac_sha256_hex",&T_STRING,2},
        {"Crypto","hex","cmm_crypto_hex",&T_STRING,1},
        {"Crypto","randomHex","cmm_crypto_random_hex",&T_STRING,1},
        {"Base64","encode","cmm_base64_encode",&T_STRING,1},
        {"Base64","decode","cmm_base64_decode",&T_STRING,1},
        {"Mysql","connect","cmm_mysql_connect",&T_INT,5},
        {"Mysql","query","cmm_mysql_query",&RT_list_data,2},
        {"Mysql","exec","cmm_mysql_exec",&T_INT,2},
        {"Mysql","insertId","cmm_mysql_insert_id",&T_INT,1},
        {"Mysql","affected","cmm_mysql_affected",&T_INT,1},
        {"Mysql","error","cmm_mysql_error",&T_STRING,1},
        {"Mysql","close","cmm_mysql_close",&T_BOOL,1},
        {"Preg","match","cmm_preg_match",&RT_list_str,2},
        {"Preg","test","cmm_preg_test",&T_BOOL,2},
        {"Preg","matchAll","cmm_preg_match_all",&RT_list_str,2},
        {"Preg","replace","cmm_preg_replace",&T_STRING,3},
        {"Preg","split","cmm_preg_split",&RT_list_str,2},
        {"Preg","quote","cmm_preg_quote",&T_STRING,1},
        {"Sys","os","cmm_sys_os",&T_STRING,0},
        {"Sys","arch","cmm_sys_arch",&T_STRING,0},
        {"Sys","platform","cmm_sys_platform",&T_STRING,0},
        {"Lambda","create","cmm_lambda_create",&T_STRING,3},
        {"Lambda","updateCode","cmm_lambda_update_code",&T_STRING,2},
    };
    NSM=nsm; NSM_N=(int)(sizeof nsm/sizeof*nsm);

    static VMethod vm[] = {
        {"String","length","cmm_string_length",&T_INT,0},
        {"String","substring","cmm_string_substring",&T_STRING,2},
        {"String","indexOf","cmm_string_indexof",&T_INT,1},
        {"String","contains","cmm_string_contains",&T_BOOL,1},
        {"String","startsWith","cmm_string_startswith",&T_BOOL,1},
        {"String","endsWith","cmm_string_endswith",&T_BOOL,1},
        {"String","upper","cmm_string_upper",&T_STRING,0},
        {"String","lower","cmm_string_lower",&T_STRING,0},
        {"String","trim","cmm_string_trim",&T_STRING,0},
        {"String","split","cmm_string_split",&RT_list_str,1},
        {"String","replace","cmm_string_replace",&T_STRING,2},
        {"String","toInt","cmm_string_toint",&T_INT,0},
        {"String","toFloat","cmm_string_tofloat",&T_FLOAT,0},
        {"String","toStr","cmm_identity",&T_STRING,0},
        {"List","add","cmm_list_add",&T_VOID,1},
        {"List","remove","cmm_list_remove",&T_VOID,1},
        {"List","get","cmm_list_get",&T_ELEM,1},
        {"List","set","cmm_list_set",&T_VOID,2},
        {"List","length","cmm_list_length",&T_INT,0},
        {"List","contains","cmm_list_contains",&T_BOOL,1},
        {"List","clear","cmm_list_clear",&T_VOID,0},
        {"Dict","get","cmm_dict_get",&T_VAL,1},
        {"Dict","set","cmm_dict_set",&T_VOID,2},
        {"Dict","has","cmm_dict_has",&T_BOOL,1},
        {"Dict","remove","cmm_dict_remove",&T_VOID,1},
        {"Dict","keys","cmm_dict_keys",&RT_list_str,0},
        {"Dict","length","cmm_dict_length",&T_INT,0},
        {"Data","get","cmm_data_get",&T_DATA,1},
        {"Data","set","cmm_data_set",&T_VOID,2},
        {"Data","has","cmm_data_has",&T_BOOL,1},
        {"Data","length","cmm_data_length",&T_INT,0},
        {"Data","keys","cmm_data_keys",&RT_list_str,0},
        {"Data","type","cmm_data_type",&T_STRING,0},
        {"Data","at","cmm_data_at",&T_DATA,1},
        {"Data","path","cmm_data_path",&T_DATA,1},
        {"Data","getStr","cmm_data_get_str",&T_STRING,1},
        {"Data","getInt","cmm_data_get_int",&T_INT,1},
        {"Data","getFloat","cmm_data_get_float",&T_FLOAT,1},
        {"Data","getBool","cmm_data_get_bool",&T_BOOL,1},
        {"Data","isObject","cmm_data_is_object",&T_BOOL,0},
        {"Data","isArray","cmm_data_is_array",&T_BOOL,0},
        {"Data","isNull","cmm_data_is_null",&T_BOOL,0},
        {"Data","add","cmm_list_add",&T_VOID,1},
        {"Int","toStr","cmm_int_tostr",&T_STRING,0},
        {"Int","toFloat","cmm_int_tofloat",&T_FLOAT,0},
        {"Float","toStr","cmm_float_tostr",&T_STRING,0},
        {"Float","toInt","cmm_float_toint",&T_INT,0},
        {"Bool","toStr","cmm_bool_tostr",&T_STRING,0},
        {"Socket","write","cmm_socket_write",&T_BOOL,1},
        {"Socket","read","cmm_socket_read",&T_STRING,1},
        {"Socket","readAll","cmm_socket_readall",&T_STRING,0},
        {"Socket","close","cmm_socket_close",&T_BOOL,0},
    };
    VM=vm; VM_N=(int)(sizeof vm/sizeof*vm);
}

static NSMethod *ns_lookup(const char *ns, const char *m){
    for(int i=0;i<NSM_N;i++) if(strcmp(NSM[i].ns,ns)==0 && strcmp(NSM[i].method,m)==0) return &NSM[i];
    return NULL;
}
static VMethod *vm_lookup(const char *base, const char *m){
    for(int i=0;i<VM_N;i++) if(strcmp(VM[i].base,base)==0 && strcmp(VM[i].method,m)==0) return &VM[i];
    return NULL;
}
static int vm_base_known(const char *base){
    for(int i=0;i<VM_N;i++) if(strcmp(VM[i].base,base)==0) return 1;
    return 0;
}

/* ===================================================================== */
/* AST                                                                   */
/* ===================================================================== */
typedef struct TypeRef { const char *name; struct TypeRef **args; int nargs; int line, col; } TypeRef;

enum {
    /* statements */
    N_ASSIGN, N_EMPTYDECL, N_INCDEC, N_EXPRSTMT, N_RETURN, N_IF, N_WHILE,
    N_FOR, N_USELOCK,
    /* expressions */
    N_INT, N_FLOAT, N_STRING, N_BOOL, N_LISTLIT, N_DICTLIT, N_NAME,
    N_CLASSVARREF, N_BINARY, N_UNARY, N_EMPTYCHECK, N_INDEX, N_CALL,
    N_MEMBER, N_RUN, N_WAIT
};

typedef struct Node {
    int kind, line, col;
    Type *type;                 /* inferred by analyzer */
    /* scalars / literals */
    long long ival; double fval; const char *sval; int bval;
    const char *name;           /* Name/ClassVarRef/Member/For-var/EmptyDecl */
    const char *op;             /* Binary/Unary/IncDec operator */
    TypeRef *tref;              /* EmptyDecl type */
    /* generic children */
    struct Node *a, *b;         /* see per-kind notes */
    /* sequences */
    struct Node **items; int nitems;    /* ListLit items / Call args / DictLit keys */
    struct Node **items2; int nitems2;  /* DictLit values */
    struct Node **body;  int nbody;     /* While/For/UseLock body, If-then */
    struct Node **els;   int nels; int has_else;  /* If else */
    const char **svars;  int nsvars;    /* UseLock class-var names */
    int declares;                       /* Assign first-assignment */
    Type *elem_type;                    /* For */
    /* call resolution */
    struct Node *callee;
    const char *call_kind, *target_class, *method;
} Node;

typedef struct { const char *name; TypeRef *type; int line, col; } Param;
typedef struct { const char *name; TypeRef *type; int line, col; } ClassVar;
typedef struct { const char *name; const char *url; int line, col; } Use;
typedef struct {
    const char *name; Param **params; int nparams;
    TypeRef *ret; Node **body; int nbody; int is_native; int line, col;
} Function;
typedef struct {
    const char *classname; int is_native;
    Use **uses; int nuses;
    ClassVar **vars; int nvars;
    Function **funcs; int nfuncs;
    int line, col;
} Module;

static Node *node_new(int kind,int line,int col){
    Node *n=(Node*)xmalloc(sizeof(Node)); memset(n,0,sizeof(Node));
    n->kind=kind; n->line=line; n->col=col; return n;
}

static Type *type_from_ref(TypeRef *r){
    Type **args=NULL;
    if(r->nargs){ args=(Type**)xmalloc(sizeof(Type*)*(size_t)r->nargs);
        for(int i=0;i<r->nargs;i++) args[i]=type_from_ref(r->args[i]); }
    return type_new(r->name,args,r->nargs);
}

/* module helpers */
static int mod_var_index(Module *m, const char *name){
    for(int i=0;i<m->nvars;i++) if(strcmp(m->vars[i]->name,name)==0) return i;
    return -1;
}
static ClassVar *mod_var(Module *m, const char *name){
    for(int i=0;i<m->nvars;i++) if(strcmp(m->vars[i]->name,name)==0) return m->vars[i];
    return NULL;
}
static Function *mod_func(Module *m, const char *name){
    for(int i=0;i<m->nfuncs;i++) if(strcmp(m->funcs[i]->name,name)==0) return m->funcs[i];
    return NULL;
}
static Type *func_ret_type(Function *f){ return f->ret ? type_from_ref(f->ret) : T_VOID; }

/* ===================================================================== */
/* Lexer                                                                 */
/* ===================================================================== */
static const char *KEYWORDS[] = {
    "class","use","fn","if","else","for","in","while","return","run","wait",
    "empty","native","and","or","not","true","false"
};
/* multi-char operators, longest first */
static const char *OPERATORS[] = {
    "->","==","!=","<=",">=","++","--",
    "+","-","*","/","%","=","<",">",
    "{","}","(",")","[","]",
    ";",":",",",".","@"
};

typedef struct { const char *kind; const char *value; int line, col; } Token;

typedef struct {
    const char *src; size_t n; const char *file;
    size_t i; int line, col;
    Vec toks;
} Lexer;

static char lx_peek(Lexer *L, size_t off){ size_t j=L->i+off; return j<L->n ? L->src[j] : 0; }
static char lx_adv(Lexer *L){ char c=L->src[L->i++]; if(c=='\n'){L->line++;L->col=1;} else L->col++; return c; }
static void lx_add(Lexer *L,const char *kind,const char *val,int line,int col){
    Token *t=(Token*)xmalloc(sizeof(Token)); t->kind=kind; t->value=val; t->line=line; t->col=col;
    vec_push(&L->toks,t);
}

static void lx_string(Lexer *L){
    int line=L->line, col=L->col;
    lx_adv(L); /* opening quote */
    Buf out={0};
    for(;;){
        if(L->i>=L->n) die_at(L->file,line,col,"unterminated string literal");
        char c=lx_adv(L);
        if(c=='"') break;
        if(c=='\\'){
            char e=lx_adv(L); char r;
            switch(e){ case 'n':r='\n';break; case 't':r='\t';break; case 'r':r='\r';break;
                case '\\':r='\\';break; case '"':r='"';break; case '0':r='\0';break; default:r=e; }
            buf_addn(&out,&r,1);
        } else buf_addn(&out,&c,1);
    }
    /* out.p may be NULL for empty string */
    lx_add(L,"string", out.p?out.p:xstrdup(""), line, col);
}
static void lx_number(Lexer *L){
    int line=L->line, col=L->col; size_t start=L->i; int is_float=0;
    while(L->i<L->n && isdigit((unsigned char)lx_peek(L,0))) lx_adv(L);
    if(lx_peek(L,0)=='.' && isdigit((unsigned char)lx_peek(L,1))){
        is_float=1; lx_adv(L);
        while(L->i<L->n && isdigit((unsigned char)lx_peek(L,0))) lx_adv(L);
    }
    char *text=xstrndup(L->src+start, L->i-start);
    lx_add(L, is_float?"float":"int", text, line, col);
}
static void lx_word(Lexer *L){
    int line=L->line, col=L->col; size_t start=L->i;
    while(L->i<L->n){ char c=lx_peek(L,0); if(isalnum((unsigned char)c)||c=='_') lx_adv(L); else break; }
    char *text=xstrndup(L->src+start, L->i-start);
    int kw=set_has(KEYWORDS,(int)(sizeof KEYWORDS/sizeof*KEYWORDS),text);
    lx_add(L, kw?"kw":"id", text, line, col);
}
static int lx_operator(Lexer *L){
    int line=L->line, col=L->col;
    for(size_t k=0;k<sizeof OPERATORS/sizeof*OPERATORS;k++){
        const char *op=OPERATORS[k]; size_t ol=strlen(op);
        if(L->i+ol<=L->n && memcmp(L->src+L->i,op,ol)==0){
            for(size_t q=0;q<ol;q++) lx_adv(L);
            lx_add(L,"op",op,line,col);
            return 1;
        }
    }
    return 0;
}
static Token **tokenize(const char *src,const char *file,int *count){
    Lexer L; memset(&L,0,sizeof L);
    L.src=src; L.n=strlen(src); L.file=file; L.i=0; L.line=1; L.col=1;
    while(L.i<L.n){
        char c=lx_peek(&L,0);
        if(c==' '||c=='\t'||c=='\r'||c=='\n'){ lx_adv(&L); continue; }
        if(c=='/'&&lx_peek(&L,1)=='/'){ while(L.i<L.n && lx_peek(&L,0)!='\n') lx_adv(&L); continue; }
        if(c=='/'&&lx_peek(&L,1)=='*'){
            lx_adv(&L); lx_adv(&L);
            while(L.i<L.n && !(lx_peek(&L,0)=='*'&&lx_peek(&L,1)=='/')) lx_adv(&L);
            if(L.i>=L.n) die_at(L.file,L.line,L.col,"unterminated block comment");
            lx_adv(&L); lx_adv(&L); continue;
        }
        if(c=='"'){ lx_string(&L); continue; }
        if(isdigit((unsigned char)c)){ lx_number(&L); continue; }
        if(isalpha((unsigned char)c)||c=='_'){ lx_word(&L); continue; }
        if(!lx_operator(&L)) die_at(L.file,L.line,L.col,"unexpected character '%c'",c);
    }
    lx_add(&L,"eof","",L.line,L.col);
    *count=L.toks.len;
    return (Token**)L.toks.data;
}

/* ===================================================================== */
/* Parser                                                                */
/* ===================================================================== */
static const char *BINOPS[] = {"+","-","*","/","%","==","!=","<","<=",">",">=","and","or"};
static int is_binop_tok(Token *t){
    if(!(strcmp(t->kind,"op")==0 || strcmp(t->kind,"kw")==0)) return 0;
    return set_has(BINOPS,(int)(sizeof BINOPS/sizeof*BINOPS),t->value);
}

typedef struct { Token **toks; int n, pos; const char *file; } Parser;

static Token *P_cur(Parser *P){ return P->toks[P->pos]; }
static int P_at_end(Parser *P){ return strcmp(P_cur(P)->kind,"eof")==0; }
static void P_err(Parser *P, Token *t, const char *fmt, ...){
    if(!t) t=P_cur(P);
    va_list ap; va_start(ap,fmt);
    char *msg; { va_list a2; va_copy(a2,ap); int n=vsnprintf(NULL,0,fmt,ap); va_end(ap);
        msg=(char*)xmalloc((size_t)n+1); vsnprintf(msg,(size_t)n+1,fmt,a2); va_end(a2); }
    die_at(P->file,t->line,t->col,"%s",msg);
}
static int P_check(Parser *P,const char *kind,const char *val){
    Token *t=P_cur(P);
    if(strcmp(t->kind,kind)!=0) return 0;
    return val==NULL || strcmp(t->value,val)==0;
}
static int P_check_kw(Parser *P,const char *v){ return P_check(P,"kw",v); }
static int P_check_op(Parser *P,const char *v){ return P_check(P,"op",v); }
static Token *P_advance(Parser *P){ Token *t=P_cur(P); if(!P_at_end(P)) P->pos++; return t; }
static Token *P_match_op(Parser *P,const char *v){ return P_check_op(P,v)?P_advance(P):NULL; }
static Token *P_match_kw(Parser *P,const char *v){ return P_check_kw(P,v)?P_advance(P):NULL; }
static Token *P_expect_op(Parser *P,const char *v){
    if(!P_check_op(P,v)) P_err(P,NULL,"expected '%s' but found '%s'",v,P_cur(P)->value);
    return P_advance(P);
}
static Token *P_expect_kw(Parser *P,const char *v){
    if(!P_check_kw(P,v)) P_err(P,NULL,"expected '%s' but found '%s'",v,P_cur(P)->value);
    return P_advance(P);
}
static Token *P_expect_id(Parser *P){
    if(strcmp(P_cur(P)->kind,"id")!=0) P_err(P,NULL,"expected identifier but found '%s'",P_cur(P)->value);
    return P_advance(P);
}

static TypeRef *parse_type(Parser *P);
static Node *parse_expression(Parser *P);
static Node *parse_unary(Parser *P);
static Node *parse_postfix(Parser *P);
static Node *parse_primary(Parser *P);
/* lexical dirname matching the Python port: last '/' or '\\' separator */
static const char *src_dirname(const char *p){
    const char *s1=strrchr(p,'/'), *s2=strrchr(p,'\\');
    const char *s = s1>s2 ? s1 : s2;
    if(!s) return xstrdup(".");
    if(s==p){ char *r=(char*)xmalloc(2); r[0]=*p; r[1]=0; return r; }
    return xstrndup(p,(size_t)(s-p));
}
static Node **parse_block(Parser *P,int *count);
static Node *parse_statement(Parser *P);

static TypeRef *parse_type(Parser *P){
    Token *tok=P_cur(P);
    if(strcmp(tok->kind,"id")!=0) P_err(P,tok,"expected a type name, found '%s'",tok->value);
    const char *name=P_advance(P)->value;
    Vec args={0};
    if(P_match_op(P,"[")){
        vec_push(&args,parse_type(P));
        while(P_match_op(P,",")) vec_push(&args,parse_type(P));
        P_expect_op(P,"]");
    }
    TypeRef *r=(TypeRef*)xmalloc(sizeof(TypeRef));
    r->name=name; r->args=(TypeRef**)args.data; r->nargs=args.len; r->line=tok->line; r->col=tok->col;
    return r;
}

static Param *parse_param(Parser *P){
    Token *nm=P_expect_id(P);
    P_expect_op(P,":");
    TypeRef *t=parse_type(P);
    Param *p=(Param*)xmalloc(sizeof(Param)); p->name=nm->value; p->type=t; p->line=nm->line; p->col=nm->col;
    return p;
}

static Use *parse_use(Parser *P){
    Token *kw=P_expect_kw(P,"use");
    Use *u=(Use*)xmalloc(sizeof(Use)); u->name=NULL; u->url=NULL; u->line=kw->line; u->col=kw->col;
    if(strcmp(P_cur(P)->kind,"string")==0){
        u->url=P_advance(P)->value; P_expect_op(P,";"); return u;
    }
    Token *nm=P_expect_id(P);
    if(P_check_kw(P,"as")||P_check_op(P,"*")||P_check_op(P,"{"))
        P_err(P,NULL,"only whole classes may be imported; aliases and wildcards are not allowed");
    P_expect_op(P,";");
    u->name=nm->value; return u;
}

static ClassVar *parse_classvar(Parser *P){
    Token *at=P_expect_op(P,"@");
    Token *nm=P_expect_id(P);
    P_expect_op(P,":");
    TypeRef *t=parse_type(P);
    P_expect_op(P,";");
    ClassVar *v=(ClassVar*)xmalloc(sizeof(ClassVar)); v->name=nm->value; v->type=t; v->line=at->line; v->col=at->col;
    return v;
}

static Function *parse_function(Parser *P,int in_native_class){
    Token *kw=P_expect_kw(P,"fn");
    Token *nm=P_expect_id(P);
    P_expect_op(P,"(");
    Vec params={0};
    if(!P_check_op(P,")")){
        vec_push(&params,parse_param(P));
        while(P_match_op(P,",")){ if(P_check_op(P,")")) break; vec_push(&params,parse_param(P)); }
    }
    P_expect_op(P,")");
    TypeRef *ret=NULL;
    if(P_match_op(P,"->")) ret=parse_type(P);
    Function *f=(Function*)xmalloc(sizeof(Function)); memset(f,0,sizeof *f);
    f->name=nm->value; f->params=(Param**)params.data; f->nparams=params.len; f->ret=ret;
    f->line=kw->line; f->col=kw->col;
    if(P_check_op(P,";")){ P_advance(P); f->is_native=1; f->body=NULL; f->nbody=0; return f; }
    int nb; Node **body=parse_block(P,&nb);
    P_match_op(P,";");
    f->body=body; f->nbody=nb; f->is_native=in_native_class;
    return f;
}

static Module *parse_module(Parser *P){
    int is_native = P_match_kw(P,"native")!=NULL;
    Token *ckw=P_expect_kw(P,"class");
    Token *nm=P_expect_id(P);
    P_expect_op(P,";");
    Vec uses={0}, vars={0}, funcs={0};
    while(!P_at_end(P)){
        if(P_check_kw(P,"use")) vec_push(&uses,parse_use(P));
        else if(P_check_op(P,"@")) vec_push(&vars,parse_classvar(P));
        else if(P_check_kw(P,"fn")) vec_push(&funcs,parse_function(P,is_native));
        else P_err(P,NULL,"expected 'use', a '@variable', or 'fn' at class scope, found '%s'",P_cur(P)->value);
    }
    Module *m=(Module*)xmalloc(sizeof(Module)); memset(m,0,sizeof *m);
    m->classname=nm->value; m->is_native=is_native;
    m->uses=(Use**)uses.data; m->nuses=uses.len;
    m->vars=(ClassVar**)vars.data; m->nvars=vars.len;
    m->funcs=(Function**)funcs.data; m->nfuncs=funcs.len;
    m->line=ckw->line; m->col=ckw->col;
    return m;
}

static Node **parse_block(Parser *P,int *count){
    P_expect_op(P,"{");
    Vec v={0};
    while(!P_check_op(P,"}") && !P_at_end(P)) vec_push(&v,parse_statement(P));
    P_expect_op(P,"}");
    *count=v.len; return (Node**)v.data;
}

static Node *parse_if(Parser *P){
    Token *kw=P_expect_kw(P,"if");
    Node *cond=parse_expression(P);
    int nthen; Node **then=parse_block(P,&nthen);
    Node *n=node_new(N_IF,kw->line,kw->col);
    n->a=cond; n->body=then; n->nbody=nthen; n->has_else=0;
    P_match_op(P,";");
    if(P_match_kw(P,"else")){
        n->has_else=1;
        if(P_check_kw(P,"if")){ Node *inner=parse_if(P); Node **e=(Node**)xmalloc(sizeof(Node*)); e[0]=inner; n->els=e; n->nels=1; }
        else { int ne; Node **e=parse_block(P,&ne); n->els=e; n->nels=ne; P_match_op(P,";"); }
    }
    return n;
}
static Node *parse_while(Parser *P){
    Token *kw=P_expect_kw(P,"while");
    Node *cond=parse_expression(P);
    int nb; Node **body=parse_block(P,&nb);
    P_match_op(P,";");
    Node *n=node_new(N_WHILE,kw->line,kw->col); n->a=cond; n->body=body; n->nbody=nb; return n;
}
static Node *parse_for(Parser *P){
    Token *kw=P_expect_kw(P,"for");
    Token *var=P_expect_id(P);
    P_expect_kw(P,"in");
    Node *it=parse_expression(P);
    int nb; Node **body=parse_block(P,&nb);
    P_match_op(P,";");
    Node *n=node_new(N_FOR,kw->line,kw->col); n->name=var->value; n->a=it; n->body=body; n->nbody=nb; return n;
}
static Node *parse_return(Parser *P){
    Token *kw=P_expect_kw(P,"return");
    Node *val=NULL;
    if(!P_check_op(P,";")) val=parse_expression(P);
    P_expect_op(P,";");
    Node *n=node_new(N_RETURN,kw->line,kw->col); n->a=val; return n;
}
static Node *parse_uselock(Parser *P){
    Token *kw=P_expect_kw(P,"use");
    Vec names={0};
    P_expect_op(P,"@"); vec_push(&names,(void*)P_expect_id(P)->value);
    while(P_match_op(P,",")){ P_expect_op(P,"@"); vec_push(&names,(void*)P_expect_id(P)->value); }
    int nb; Node **body=parse_block(P,&nb);
    P_match_op(P,";");
    Node *n=node_new(N_USELOCK,kw->line,kw->col);
    n->svars=(const char**)names.data; n->nsvars=names.len; n->body=body; n->nbody=nb; return n;
}

static Node *parse_statement(Parser *P){
    if(P_check_kw(P,"if")) return parse_if(P);
    if(P_check_kw(P,"while")) return parse_while(P);
    if(P_check_kw(P,"for")) return parse_for(P);
    if(P_check_kw(P,"return")) return parse_return(P);
    if(P_check_kw(P,"use")) return parse_uselock(P);

    Token *start=P_cur(P);
    Node *lhs=parse_expression(P);

    if(P_match_op(P,":")){
        if(lhs->kind!=N_NAME) P_err(P,start,"empty declaration must name a simple variable");
        TypeRef *t=parse_type(P); P_expect_op(P,";");
        Node *n=node_new(N_EMPTYDECL,start->line,start->col); n->name=lhs->name; n->tref=t; return n;
    }
    if(P_match_op(P,"=")){
        Node *val=parse_expression(P); P_expect_op(P,";");
        if(!(lhs->kind==N_NAME||lhs->kind==N_CLASSVARREF||lhs->kind==N_INDEX))
            P_err(P,start,"invalid assignment target");
        Node *n=node_new(N_ASSIGN,start->line,start->col); n->a=lhs; n->b=val; n->declares=0; return n;
    }
    if(P_check_op(P,"++")||P_check_op(P,"--")){
        const char *op=P_advance(P)->value; P_expect_op(P,";");
        if(!(lhs->kind==N_NAME||lhs->kind==N_CLASSVARREF)) P_err(P,start,"'++'/'--' require a variable");
        Node *n=node_new(N_INCDEC,start->line,start->col); n->a=lhs; n->op=op; return n;
    }
    P_expect_op(P,";");
    Node *n=node_new(N_EXPRSTMT,start->line,start->col); n->a=lhs; return n;
}

static Node *parse_expression(Parser *P){
    Node *left=parse_unary(P);
    Token *t=P_cur(P);
    if(is_binop_tok(t)){
        const char *op=P_advance(P)->value;
        Node *right=parse_unary(P);
        if(is_binop_tok(P_cur(P)))
            P_err(P,NULL,"compound expression must be grouped with parentheses (e.g. 'a + (b * c)')");
        Node *n=node_new(N_BINARY,left->line,left->col); n->op=op; n->a=left; n->b=right; return n;
    }
    return left;
}
static Node *parse_unary(Parser *P){
    if(P_check_kw(P,"not")){ Token *t=P_advance(P); Node *n=node_new(N_UNARY,t->line,t->col); n->op="not"; n->a=parse_unary(P); return n; }
    if(P_check_op(P,"-")){ Token *t=P_advance(P); Node *n=node_new(N_UNARY,t->line,t->col); n->op="-"; n->a=parse_unary(P); return n; }
    if(P_check_kw(P,"empty")){ Token *t=P_advance(P); P_expect_op(P,"("); Node *e=parse_expression(P); P_expect_op(P,")");
        Node *n=node_new(N_EMPTYCHECK,t->line,t->col); n->a=e; return n; }
    return parse_postfix(P);
}
static Node **parse_args(Parser *P,int *count){
    P_expect_op(P,"(");
    Vec v={0};
    if(!P_check_op(P,")")){
        vec_push(&v,parse_expression(P));
        while(P_match_op(P,",")){ if(P_check_op(P,")")) break; vec_push(&v,parse_expression(P)); }
    }
    P_expect_op(P,")");
    *count=v.len; return (Node**)v.data;
}
static Node *parse_postfix(Parser *P){
    Node *e=parse_primary(P);
    for(;;){
        if(P_match_op(P,".")){
            Token *nm=P_expect_id(P);
            Node *m=node_new(N_MEMBER,e->line,e->col); m->a=e; m->name=nm->value; e=m;
        } else if(P_check_op(P,"[")){
            P_advance(P); Node *idx=parse_expression(P); P_expect_op(P,"]");
            Node *ix=node_new(N_INDEX,e->line,e->col); ix->a=e; ix->b=idx; e=ix;
        } else if(P_check_op(P,"(")){
            int na; Node **args=parse_args(P,&na);
            Node *c=node_new(N_CALL,e->line,e->col); c->callee=e; c->items=args; c->nitems=na;
            c->call_kind=""; c->target_class=""; c->method=""; e=c;
        } else break;
    }
    return e;
}
static Node *parse_list_literal(Parser *P){
    Token *t=P_expect_op(P,"[");
    Vec v={0};
    if(!P_check_op(P,"]")){
        vec_push(&v,parse_expression(P));
        while(P_match_op(P,",")){ if(P_check_op(P,"]")) break; vec_push(&v,parse_expression(P)); }
    }
    P_expect_op(P,"]");
    Node *n=node_new(N_LISTLIT,t->line,t->col); n->items=(Node**)v.data; n->nitems=v.len; return n;
}
static Node *parse_dict_literal(Parser *P){
    Token *t=P_expect_op(P,"{");
    Vec keys={0}, vals={0};
    if(!P_check_op(P,"}")){
        Node *k=parse_expression(P); P_expect_op(P,":"); Node *vv=parse_expression(P);
        vec_push(&keys,k); vec_push(&vals,vv);
        while(P_match_op(P,",")){ if(P_check_op(P,"}")) break;
            k=parse_expression(P); P_expect_op(P,":"); vv=parse_expression(P);
            vec_push(&keys,k); vec_push(&vals,vv); }
    }
    P_expect_op(P,"}");
    Node *n=node_new(N_DICTLIT,t->line,t->col);
    n->items=(Node**)keys.data; n->nitems=keys.len; n->items2=(Node**)vals.data; n->nitems2=vals.len; return n;
}
static Node *parse_primary(Parser *P){
    Token *t=P_cur(P);
    if(strcmp(t->kind,"int")==0){ P_advance(P); Node *n=node_new(N_INT,t->line,t->col); n->sval=t->value; n->ival=strtoll(t->value,NULL,10); return n; }
    if(strcmp(t->kind,"float")==0){ P_advance(P); Node *n=node_new(N_FLOAT,t->line,t->col); n->sval=t->value; n->fval=strtod(t->value,NULL); return n; }
    if(strcmp(t->kind,"string")==0){ P_advance(P); Node *n=node_new(N_STRING,t->line,t->col); n->sval=t->value; return n; }
    if(P_check_kw(P,"true")){ P_advance(P); Node *n=node_new(N_BOOL,t->line,t->col); n->bval=1; return n; }
    if(P_check_kw(P,"false")){ P_advance(P); Node *n=node_new(N_BOOL,t->line,t->col); n->bval=0; return n; }
    if(P_check_kw(P,"run")){ P_advance(P); Node *call=parse_postfix(P);
        if(call->kind!=N_CALL) P_err(P,t,"'run' must be followed by a call expression");
        Node *n=node_new(N_RUN,t->line,t->col); n->a=call; return n; }
    if(P_check_kw(P,"wait")){ P_advance(P); Node *job=parse_postfix(P);
        Node *n=node_new(N_WAIT,t->line,t->col); n->a=job; return n; }
    if(P_match_op(P,"(")){ Node *e=parse_expression(P); P_expect_op(P,")"); return e; }
    if(P_check_op(P,"[")) return parse_list_literal(P);
    if(P_check_op(P,"{")) return parse_dict_literal(P);
    if(P_check_op(P,"@")){ P_advance(P); Token *nm=P_expect_id(P); Node *n=node_new(N_CLASSVARREF,t->line,t->col); n->name=nm->value; return n; }
    if(strcmp(t->kind,"id")==0){ P_advance(P);
        if(strcmp(t->value,"__FILE__")==0){ Node *n=node_new(N_STRING,t->line,t->col); n->sval=P->file; return n; }
        if(strcmp(t->value,"__DIR__")==0){ Node *n=node_new(N_STRING,t->line,t->col); n->sval=src_dirname(P->file); return n; }
        Node *n=node_new(N_NAME,t->line,t->col); n->name=t->value; return n; }
    P_err(P,NULL,"unexpected '%s' in expression",t->value);
    return NULL;
}

static Module *parse_source(const char *src,const char *file){
    int nt; Token **toks=tokenize(src,file,&nt);
    Parser P; P.toks=toks; P.n=nt; P.pos=0; P.file=file;
    return parse_module(&P);
}

/* ===================================================================== */
/* Analyzer                                                              */
/* ===================================================================== */
static Vec g_program;            /* Module* */
static Module *A_cur_class;
static const char *A_file;
/* lint mode: collect all diagnostics instead of dying on the first */
static int g_collect=0;
static jmp_buf g_astmt_jmp;
static int g_astmt_active=0;
typedef struct { const char *file; int line, col; char *msg; } ADiag;
static ADiag g_adiags[1024];
static int g_nadiags=0;

static Module *find_class(const char *name){
    for(int i=0;i<g_program.len;i++){ Module *m=(Module*)g_program.data[i]; if(strcmp(m->classname,name)==0) return m; }
    return NULL;
}

typedef struct { const char *name; Type *type; } Binding;
typedef struct { Vec b; } Scope;
static Type *scope_get(Scope *s,const char *n){
    for(int i=0;i<s->b.len;i++){ Binding *bd=(Binding*)s->b.data[i]; if(strcmp(bd->name,n)==0) return bd->type; }
    return NULL;
}
static int scope_has(Scope *s,const char *n){ return scope_get(s,n)!=NULL; }
static void scope_declare(Scope *s,const char *n,Type *t){
    for(int i=0;i<s->b.len;i++){ Binding *bd=(Binding*)s->b.data[i]; if(strcmp(bd->name,n)==0){ bd->type=t; return; } }
    Binding *bd=(Binding*)xmalloc(sizeof(Binding)); bd->name=n; bd->type=t; vec_push(&s->b,bd);
}

static void A_err(Node *n,const char *fmt,...){
    va_list ap; va_start(ap,fmt);
    char *m; { va_list a2; va_copy(a2,ap); int k=vsnprintf(NULL,0,fmt,ap); va_end(ap);
        m=(char*)xmalloc((size_t)k+1); vsnprintf(m,(size_t)k+1,fmt,a2); va_end(a2); }
    if(g_collect && g_astmt_active){
        if(g_nadiags < (int)(sizeof g_adiags/sizeof g_adiags[0])){
            g_adiags[g_nadiags].file=A_file; g_adiags[g_nadiags].line=n?n->line:0;
            g_adiags[g_nadiags].col=n?n->col:0; g_adiags[g_nadiags].msg=m; g_nadiags++;
        }
        longjmp(g_astmt_jmp, 1);
    }
    die_at(A_file, n?n->line:0, n?n->col:0, "%s", m);
}

static Type *classvar_type(const char *name, Node *node){
    ClassVar *v=mod_var(A_cur_class,name);
    if(!v) A_err(node,"@%s is not a declared class variable",name);
    return type_from_ref(v->type);
}
static int assignable(Type *target, Type *value){
    if(type_eq(target,value)) return 1;
    if(type_eq(target,T_DATA)||type_eq(value,T_DATA)) return 1;
    if(type_eq(target,T_FLOAT)&&type_eq(value,T_INT)) return 1;
    if(strcmp(target->name,value->name)==0 &&
       (strcmp(target->name,"List")==0||strcmp(target->name,"Dict")==0||strcmp(target->name,"Job")==0)) return 1;
    return 0;
}

static Type *infer(Node *e, Scope *sc);

static int is_cmp_or_logic(const char *op){
    return strcmp(op,"==")==0||strcmp(op,"!=")==0||strcmp(op,"<")==0||strcmp(op,"<=")==0||
           strcmp(op,">")==0||strcmp(op,">=")==0||strcmp(op,"and")==0||strcmp(op,"or")==0;
}

static Type *infer_call(Node *e, Scope *sc){
    Node *callee=e->callee;
    if(callee->kind==N_NAME && !scope_has(sc,callee->name)){
        const char *name=callee->name;
        if(find_class(name) && !is_native_ns(name)){
            e->call_kind="ctor"; e->target_class=name;
            for(int i=0;i<e->nitems;i++) infer(e->items[i],sc);
            return type0(name);
        }
        Function *f=mod_func(A_cur_class,name);
        if(f){
            e->call_kind="self_method"; e->target_class=A_cur_class->classname; e->method=name;
            for(int i=0;i<e->nitems;i++) infer(e->items[i],sc);
            return func_ret_type(f);
        }
        A_err(e,"unknown function or class '%s'",name);
    }
    if(callee->kind==N_MEMBER){
        Node *target=callee->a; const char *method=callee->name;
        if(target->kind==N_NAME && !scope_has(sc,target->name) && is_native_ns(target->name)){
            NSMethod *nm=ns_lookup(target->name,method);
            if(!nm) A_err(e,"%s has no method '%s'",target->name,method);
            if(nm->arity>=0 && e->nitems!=nm->arity)
                A_err(e,"%s.%s expects %d argument(s), got %d",target->name,method,nm->arity,e->nitems);
            for(int i=0;i<e->nitems;i++) infer(e->items[i],sc);
            e->call_kind="native"; e->target_class=target->name; e->method=method;
            return *nm->ret;
        }
        if(target->kind==N_NAME && !scope_has(sc,target->name) && find_class(target->name) && !is_native_ns(target->name)){
            Module *cm=find_class(target->name);
            Function *f=mod_func(cm,method);
            if(!f) A_err(e,"class %s has no static method '%s'",target->name,method);
            for(int i=0;i<e->nitems;i++) infer(e->items[i],sc);
            e->call_kind="static"; e->target_class=target->name; e->method=method;
            return func_ret_type(f);
        }
        Type *recv=infer(target,sc);
        for(int i=0;i<e->nitems;i++) infer(e->items[i],sc);
        const char *base=recv->name;
        VMethod *vmth=vm_lookup(base,method);
        if(vmth){
            Type *ret=*vmth->ret;
            if(ret==T_ELEM) ret = recv->nargs>0 ? recv->args[0] : T_DATA;
            else if(ret==T_VAL) ret = recv->nargs>1 ? recv->args[1] : T_DATA;
            if(vmth->arity>=0 && e->nitems!=vmth->arity)
                A_err(e,"%s.%s expects %d argument(s), got %d",base,method,vmth->arity,e->nitems);
            e->call_kind="value_method"; e->target_class=base; e->method=method;
            return ret;
        }
        Module *cm=find_class(base);
        if(cm){
            Function *f=mod_func(cm,method);
            if(!f) A_err(e,"class %s has no method '%s'",base,method);
            e->call_kind="method"; e->target_class=base; e->method=method;
            return func_ret_type(f);
        }
        A_err(e,"type %s has no method '%s'",recv->name,method);
    }
    A_err(e,"invalid call");
    return T_VOID;
}

static Type *infer(Node *e, Scope *sc){
    Type *t=NULL;
    switch(e->kind){
    case N_INT:    t=T_INT; break;
    case N_FLOAT:  t=T_FLOAT; break;
    case N_STRING: t=T_STRING; break;
    case N_BOOL:   t=T_BOOL; break;
    case N_LISTLIT: {
        Type *elem=NULL; int uniform=1;
        for(int i=0;i<e->nitems;i++){ Type *it=infer(e->items[i],sc); if(i==0) elem=it; else if(!type_eq(it,elem)) uniform=0; }
        if(e->nitems==0||!uniform) elem=T_DATA;
        t=list_of(elem); break;
    }
    case N_DICTLIT: {
        for(int i=0;i<e->nitems;i++) infer(e->items[i],sc);
        Type *val=NULL; int uniform=1;
        for(int i=0;i<e->nitems2;i++){ Type *vt=infer(e->items2[i],sc); if(i==0) val=vt; else if(!type_eq(vt,val)) uniform=0; }
        if(e->nitems2==0||!uniform) val=T_DATA;
        t=dict_of(T_STRING,val); break;
    }
    case N_NAME: {
        Type *st=scope_get(sc,e->name);
        if(st) { t=st; break; }
        if(is_native_ns(e->name)||is_native_cls(e->name)){ t=namespace_of(e->name); break; }
        if(find_class(e->name)){ t=namespace_of(e->name); break; }
        A_err(e,"undefined name '%s'",e->name); break;
    }
    case N_CLASSVARREF: t=classvar_type(e->name,e); break;
    case N_UNARY: { Type *ot=infer(e->a,sc); t = strcmp(e->op,"not")==0 ? T_BOOL : ot; break; }
    case N_EMPTYCHECK: infer(e->a,sc); t=T_BOOL; break;
    case N_BINARY: {
        Type *lt=infer(e->a,sc), *rt=infer(e->b,sc);
        if(is_cmp_or_logic(e->op)){ t=T_BOOL; break; }
        if(strcmp(e->op,"+")==0 && (type_eq(lt,T_STRING)||type_eq(rt,T_STRING))){ t=T_STRING; break; }
        if(type_eq(lt,T_FLOAT)||type_eq(rt,T_FLOAT)){ t=T_FLOAT; break; }
        if(type_eq(lt,T_INT)&&type_eq(rt,T_INT)){ t=T_INT; break; }
        if(type_eq(lt,T_DATA)||type_eq(rt,T_DATA)){ t=T_DATA; break; }
        t=lt; break;
    }
    case N_INDEX: {
        Type *tt=infer(e->a,sc); infer(e->b,sc);
        if(strcmp(tt->name,"List")==0) t = tt->nargs>0 ? tt->args[0] : T_DATA;
        else if(strcmp(tt->name,"Dict")==0) t = tt->nargs>1 ? tt->args[1] : T_DATA;
        else if(strcmp(tt->name,"String")==0) t=T_STRING;
        else if(strcmp(tt->name,"Data")==0) t=T_DATA;
        else A_err(e,"cannot index a value of type %s",tt->name);
        break;
    }
    case N_MEMBER: {
        Type *tt=infer(e->a,sc);
        if(strcmp(tt->name,"Data")==0) t=T_DATA;
        else A_err(e,"'%s' must be called as a method",e->name);
        break;
    }
    case N_RUN: { Type *ct=infer(e->a,sc); t=job_of(type_eq(ct,T_VOID)?T_DATA:ct); break; }
    case N_WAIT: {
        Type *jt=infer(e->a,sc);
        if(strcmp(jt->name,"Job")==0) t = jt->nargs>0 ? jt->args[0] : T_DATA;
        else A_err(e,"'wait' expects a Job, got %s",jt->name);
        break;
    }
    case N_CALL: t=infer_call(e,sc); break;
    default: A_err(e,"cannot analyze expression kind %d",e->kind);
    }
    e->type=t;
    return t;
}

static void analyze_block(Node **stmts,int n,Scope *sc);

static void analyze_stmt(Node *s, Scope *sc){
    switch(s->kind){
    case N_ASSIGN: {
        Type *vt=T_DATA;
        if(g_collect){
            /* analyze the RHS under a nested setjmp; if it errors, still declare
               a new simple target as permissive so later uses don't cascade. */
            jmp_buf save; int prev=g_astmt_active;
            memcpy(save, g_astmt_jmp, sizeof(jmp_buf));
            g_astmt_active=1;
            if(setjmp(g_astmt_jmp)==0){
                vt=infer(s->b,sc);
                g_astmt_active=prev; memcpy(g_astmt_jmp, save, sizeof(jmp_buf));
            } else {
                g_astmt_active=prev; memcpy(g_astmt_jmp, save, sizeof(jmp_buf));
                if(s->a->kind==N_NAME && !scope_has(sc,s->a->name))
                    scope_declare(sc, s->a->name, T_DATA);
                longjmp(g_astmt_jmp, 1);   /* abort this statement, keep going */
            }
        } else {
            vt=infer(s->b,sc);
        }
        Node *tgt=s->a;
        if(tgt->kind==N_NAME){
            Type *ex=scope_get(sc,tgt->name);
            if(!ex){
                if(type_eq(vt,T_VOID)) A_err(s,"cannot assign a value-less expression");
                scope_declare(sc,tgt->name,vt); s->declares=1; tgt->type=vt;
            } else {
                if(!type_eq(ex,vt) && !assignable(ex,vt))
                    A_err(s,"variable '%s' has type %s; cannot reassign it to %s (a local's type is fixed at first assignment)",tgt->name,ex->name,vt->name);
                tgt->type=ex;
            }
        } else if(tgt->kind==N_CLASSVARREF){
            Type *t=classvar_type(tgt->name,s); tgt->type=t;
            if(!assignable(t,vt)) A_err(s,"cannot assign %s to @%s of type %s",vt->name,tgt->name,t->name);
        } else if(tgt->kind==N_INDEX){
            infer(tgt,sc);
        } else A_err(s,"invalid assignment target");
        break;
    }
    case N_EMPTYDECL:
        if(scope_has(sc,s->name)) A_err(s,"'%s' is already declared",s->name);
        scope_declare(sc,s->name,type_from_ref(s->tref));
        break;
    case N_INCDEC: { Type *t=infer(s->a,sc); if(!type_is_numeric(t)) A_err(s,"'%s' requires a numeric variable, got %s",s->op,t->name); break; }
    case N_EXPRSTMT: infer(s->a,sc); break;
    case N_RETURN:
        if(s->a==NULL) break;
        /* a return may carry any expression; codegen captures it into a
           __ret temporary before cmm_frame_leave, so no restriction here. */
        infer(s->a,sc);
        break;
    case N_IF:
        infer(s->a,sc);
        analyze_block(s->body,s->nbody,sc);
        if(s->has_else) analyze_block(s->els,s->nels,sc);
        break;
    case N_WHILE:
        infer(s->a,sc); analyze_block(s->body,s->nbody,sc); break;
    case N_FOR: {
        Type *it=infer(s->a,sc); Type *elem;
        if(strcmp(it->name,"List")==0) elem = it->nargs>0 ? it->args[0] : T_DATA;
        else if(strcmp(it->name,"String")==0) elem=T_STRING;
        else if(strcmp(it->name,"Dict")==0) elem=T_STRING;
        else if(strcmp(it->name,"Data")==0) elem=T_DATA;
        else { A_err(s,"cannot iterate over a value of type %s",it->name); elem=T_DATA; }
        scope_declare(sc,s->name,elem); s->elem_type=elem;
        analyze_block(s->body,s->nbody,sc);
        break;
    }
    case N_USELOCK:
        for(int i=0;i<s->nsvars;i++)
            if(!mod_var(A_cur_class,s->svars[i])) A_err(s,"'use' lock target @%s is not a class variable",s->svars[i]);
        analyze_block(s->body,s->nbody,sc);
        break;
    default: A_err(s,"cannot analyze statement kind %d",s->kind);
    }
}
static void analyze_block(Node **stmts,int n,Scope *sc){
    for(int i=0;i<n;i++){
        if(g_collect){
            jmp_buf save; int prev=g_astmt_active;
            memcpy(save, g_astmt_jmp, sizeof(jmp_buf));
            g_astmt_active=1;
            if(setjmp(g_astmt_jmp)==0) analyze_stmt(stmts[i],sc);
            g_astmt_active=prev;
            memcpy(g_astmt_jmp, save, sizeof(jmp_buf));
        } else {
            analyze_stmt(stmts[i],sc);
        }
    }
}

static void analyze_program(void){
    for(int i=0;i<g_program.len;i++){
        Module *m=(Module*)g_program.data[i];
        A_cur_class=m; A_file=sfmt("%s.cmm",m->classname);
        for(int j=0;j<m->nfuncs;j++){
            Function *fn=m->funcs[j];
            if(!fn->is_native && fn->body!=NULL && !m->is_native){
                Scope sc; memset(&sc,0,sizeof sc);
                for(int p=0;p<fn->nparams;p++) scope_declare(&sc,fn->params[p]->name,type_from_ref(fn->params[p]->type));
                analyze_block(fn->body,fn->nbody,&sc);
            }
        }
    }
}

/* ===================================================================== */
/* Code generation                                                       */
/* ===================================================================== */
static const char *C_KEYWORDS[] = {
    "auto","break","case","char","const","continue","default","do","double",
    "else","enum","extern","float","for","goto","if","int","long","register",
    "return","short","signed","sizeof","static","struct","switch","typedef",
    "union","unsigned","void","volatile","while","self","restrict","inline","_Bool"
};
static char *cident(const char *name){
    if(set_has(C_KEYWORDS,(int)(sizeof C_KEYWORDS/sizeof*C_KEYWORDS),name)) return sfmt("lv_%s",name);
    return xstrdup(name);
}
static char *cstring(const char *s){
    Buf b={0}; buf_add(&b,"\"");
    for(const unsigned char *p=(const unsigned char*)s; *p; p++){
        unsigned char c=*p;
        if(c=='\\') buf_add(&b,"\\\\");
        else if(c=='"') buf_add(&b,"\\\"");
        else if(c=='\n') buf_add(&b,"\\n");
        else if(c=='\r') buf_add(&b,"\\r");
        else if(c=='\t') buf_add(&b,"\\t");
        else if(c>=32 && c<127){ char ch=(char)c; buf_addn(&b,&ch,1); }
        else buf_addf(&b,"\\%03o",c);
    }
    buf_add(&b,"\""); return b.p;
}

/* codegen state */
static Module **G_classes; static int G_nclasses;
static int G_classid(const char *name){ for(int i=0;i<G_nclasses;i++) if(strcmp(G_classes[i]->classname,name)==0) return i; return -1; }
static Vec G_thunks;          /* char* snippets */
static int G_tmp;
static Module *G_cur_class;
static int g_debug=0;   /* --debug / -g : #line maps + -O0 -g -DCMM_DEBUG */
typedef struct { char *lk; int count; } Lock;
static Vec G_lockstack;       /* Lock* */

static char *new_tmp(const char *prefix){ return sfmt("%s%d",prefix,++G_tmp); }

static void emit(Buf *out,const char *s){ buf_add(out,s); buf_add(out,"\n"); }

static char *proto(Module *m, Function *fn){
    Buf b={0};
    buf_addf(&b,"static CmmValue %s_%s(CmmValue self",cident(m->classname),cident(fn->name));
    for(int i=0;i<fn->nparams;i++) buf_addf(&b,", CmmValue %s",cident(fn->params[i]->name));
    buf_add(&b,")");
    return b.p;
}

static char *typed_empty(TypeRef *r){
    const char *n=r->name;
    if(strcmp(n,"Int")==0) return xstrdup("cmm_int(0)");
    if(strcmp(n,"Float")==0) return xstrdup("cmm_float(0)");
    if(strcmp(n,"Bool")==0) return xstrdup("cmm_bool(0)");
    if(strcmp(n,"String")==0) return xstrdup("cmm_str(\"\")");
    if(strcmp(n,"List")==0) return xstrdup("cmm_new_list()");
    if(strcmp(n,"Dict")==0) return xstrdup("cmm_new_dict()");
    return xstrdup("cmm_empty()");
}

static char *gen_constructor(Module *m){
    Buf out={0}; char *cls=cident(m->classname); int nf=m->nvars;
    buf_addf(&out,"static CmmValue %s_new(void) {\n",cls);
    buf_addf(&out,"    CmmValue self = cmm_object_value(cmm_new_object(CLASS_%s, %d, 0));\n",cls,nf);
    for(int i=0;i<nf;i++) buf_addf(&out,"    self.obj->fields[%d] = %s;\n",i,typed_empty(m->vars[i]->type));
    buf_add(&out,"    return self;\n}");
    return out.p;
}

/* collect every local assigned/declared anywhere in the body */
static int name_in(Vec *v,const char *n){ for(int i=0;i<v->len;i++) if(strcmp((char*)v->data[i],n)==0) return 1; return 0; }
static void collect_walk(Node **stmts,int n,Vec *names,Vec *seen,Vec *params){
    for(int i=0;i<n;i++){
        Node *s=stmts[i];
        if(s->kind==N_ASSIGN && s->a->kind==N_NAME){
            const char *nm=s->a->name;
            if(!name_in(seen,nm)&&!name_in(params,nm)){ vec_push(seen,(void*)nm); vec_push(names,(void*)nm); }
        } else if(s->kind==N_EMPTYDECL){
            if(!name_in(seen,s->name)&&!name_in(params,s->name)){ vec_push(seen,(void*)s->name); vec_push(names,(void*)s->name); }
        } else if(s->kind==N_FOR){
            if(!name_in(seen,s->name)&&!name_in(params,s->name)){ vec_push(seen,(void*)s->name); vec_push(names,(void*)s->name); }
            collect_walk(s->body,s->nbody,names,seen,params);
        } else if(s->kind==N_IF){
            collect_walk(s->body,s->nbody,names,seen,params);
            if(s->has_else) collect_walk(s->els,s->nels,names,seen,params);
        } else if(s->kind==N_WHILE){
            collect_walk(s->body,s->nbody,names,seen,params);
        } else if(s->kind==N_USELOCK){
            collect_walk(s->body,s->nbody,names,seen,params);
        }
    }
}

static char *op_cfn(const char *op){
    if(strcmp(op,"+")==0) return "cmm_add"; if(strcmp(op,"-")==0) return "cmm_sub";
    if(strcmp(op,"*")==0) return "cmm_mul"; if(strcmp(op,"/")==0) return "cmm_div";
    if(strcmp(op,"%")==0) return "cmm_mod"; if(strcmp(op,"==")==0) return "cmm_eq";
    if(strcmp(op,"!=")==0) return "cmm_ne"; if(strcmp(op,"<")==0) return "cmm_lt";
    if(strcmp(op,"<=")==0) return "cmm_le"; if(strcmp(op,">")==0) return "cmm_gt";
    if(strcmp(op,">=")==0) return "cmm_ge"; if(strcmp(op,"and")==0) return "cmm_and";
    if(strcmp(op,"or")==0) return "cmm_or"; return "cmm_add";
}
static char *join_list(char **parts,int n){
    Buf b={0}; for(int i=0;i<n;i++){ if(i) buf_add(&b,", "); buf_add(&b,parts[i]); }
    return b.p?b.p:xstrdup("");
}

static char *gen_expr(Node *e, Vec *setup);

static char *call_expr(Node *call, const char *recv_str, char **arg_strs, int narg, const char *self_tok){
    const char *kind=call->call_kind;
    if(strcmp(kind,"ctor")==0) return sfmt("%s_new()",cident(call->target_class));
    if(strcmp(kind,"self_method")==0){
        char **parts=(char**)xmalloc(sizeof(char*)*(size_t)(narg+1));
        parts[0]=(char*)self_tok; for(int i=0;i<narg;i++) parts[i+1]=arg_strs[i];
        return sfmt("%s_%s(%s)",cident(call->target_class),cident(call->method),join_list(parts,narg+1));
    }
    if(strcmp(kind,"method")==0){
        char **parts=(char**)xmalloc(sizeof(char*)*(size_t)(narg+1));
        parts[0]=(char*)recv_str; for(int i=0;i<narg;i++) parts[i+1]=arg_strs[i];
        return sfmt("%s_%s(%s)",cident(call->target_class),cident(call->method),join_list(parts,narg+1));
    }
    if(strcmp(kind,"static")==0){
        char **parts=(char**)xmalloc(sizeof(char*)*(size_t)(narg+1));
        parts[0]=sfmt("%s_new()",cident(call->target_class));
        for(int i=0;i<narg;i++) parts[i+1]=arg_strs[i];
        return sfmt("%s_%s(%s)",cident(call->target_class),cident(call->method),join_list(parts,narg+1));
    }
    if(strcmp(kind,"native")==0){
        NSMethod *nm=ns_lookup(call->target_class,call->method);
        return sfmt("%s(%s)",nm->cfn,join_list(arg_strs,narg));
    }
    if(strcmp(kind,"value_method")==0){
        VMethod *vmth=vm_lookup(call->target_class,call->method);
        char **parts=(char**)xmalloc(sizeof(char*)*(size_t)(narg+1));
        parts[0]=(char*)recv_str; for(int i=0;i<narg;i++) parts[i+1]=arg_strs[i];
        return sfmt("%s(%s)",vmth->cfn,join_list(parts,narg+1));
    }
    die("internal: unknown call kind '%s'",kind); return NULL;
}

static char *render_call(Node *call, Vec *setup){
    const char *recv_str=NULL;
    if(strcmp(call->call_kind,"method")==0||strcmp(call->call_kind,"value_method")==0)
        recv_str=gen_expr(call->callee->a,setup);
    char **args=(char**)xmalloc(sizeof(char*)*(size_t)(call->nitems>0?call->nitems:1));
    for(int i=0;i<call->nitems;i++) args[i]=gen_expr(call->items[i],setup);
    return call_expr(call,recv_str,args,call->nitems,"self");
}

static char *gen_run(Node *e, Vec *setup){
    Node *call=e->a;
    const char *recv_str=NULL;
    if(strcmp(call->call_kind,"method")==0||strcmp(call->call_kind,"value_method")==0)
        recv_str=gen_expr(call->callee->a,setup);
    char **args=(char**)xmalloc(sizeof(char*)*(size_t)(call->nitems>0?call->nitems:1));
    for(int i=0;i<call->nitems;i++) args[i]=gen_expr(call->items[i],setup);

    int n=G_thunks.len;
    char *strct=sfmt("__Thunk%d",n);
    int is_self = strcmp(call->call_kind,"self_method")==0;
    const char *thunk_recv = recv_str?"th->recv":NULL;
    char **thunk_args=(char**)xmalloc(sizeof(char*)*(size_t)(call->nitems>0?call->nitems:1));
    for(int i=0;i<call->nitems;i++) thunk_args[i]=sfmt("th->a%d",i);

    char *body_call=call_expr(call,thunk_recv,thunk_args,call->nitems,"th->self");

    Buf sn={0};
    buf_add(&sn,"typedef struct {\n");
    if(recv_str) buf_add(&sn,"    CmmValue recv;\n");
    if(is_self)  buf_add(&sn,"    CmmValue self;\n");
    for(int i=0;i<call->nitems;i++) buf_addf(&sn,"    CmmValue a%d;\n",i);
    buf_add(&sn,"    CmmJob *job;\n");
    buf_addf(&sn,"} %s;\n",strct);
    buf_addf(&sn,"static void *__run%d(void *p) {\n",n);
    buf_add(&sn,"    cmm_thread_attach();\n");
    buf_addf(&sn,"    %s *th = (%s *)p;\n",strct,strct);
    buf_addf(&sn,"    th->job->result = %s;\n",body_call);
    buf_add(&sn,"    return (void *)0;\n");
    buf_add(&sn,"}");
    vec_push(&G_thunks,sn.p);

    char *thv=new_tmp("__th"); char *jbv=new_tmp("__jb");
    vec_push(setup,sfmt("%s *%s = (%s *)cmm_alloc(sizeof(%s));",strct,thv,strct,strct));
    if(recv_str) vec_push(setup,sfmt("%s->recv = %s;",thv,recv_str));
    if(is_self)  vec_push(setup,sfmt("%s->self = self;",thv));
    for(int i=0;i<call->nitems;i++) vec_push(setup,sfmt("%s->a%d = %s;",thv,i,args[i]));
    vec_push(setup,sfmt("CmmJob *%s = cmm_job_new();",jbv));
    vec_push(setup,sfmt("%s->job = %s;",thv,jbv));
    vec_push(setup,sfmt("cmm_thread_start(%s, __run%d, %s);",jbv,n,thv));
    return sfmt("cmm_job_value(%s)",jbv);
}

static char *gen_expr(Node *e, Vec *setup){
    switch(e->kind){
    case N_INT:    return sfmt("cmm_int(%s)",e->sval);
    case N_FLOAT:  return sfmt("cmm_float(%s)",e->sval);
    case N_STRING: return sfmt("cmm_str(%s)",cstring(e->sval));
    case N_BOOL:   return sfmt("cmm_bool(%d)",e->bval?1:0);
    case N_NAME:   return cident(e->name);
    case N_CLASSVARREF: return sfmt("self.obj->fields[%d]",mod_var_index(G_cur_class,e->name));
    case N_LISTLIT: {
        char *tmp=new_tmp("__list");
        vec_push(setup,sfmt("CmmValue %s = cmm_new_list();",tmp));
        for(int i=0;i<e->nitems;i++){ char *v=gen_expr(e->items[i],setup); vec_push(setup,sfmt("cmm_list_add(%s, %s);",tmp,v)); }
        return tmp;
    }
    case N_DICTLIT: {
        char *tmp=new_tmp("__dict");
        vec_push(setup,sfmt("CmmValue %s = cmm_new_dict();",tmp));
        for(int i=0;i<e->nitems;i++){ char *k=gen_expr(e->items[i],setup); char *v=gen_expr(e->items2[i],setup);
            vec_push(setup,sfmt("cmm_dict_set(%s, %s, %s);",tmp,k,v)); }
        return tmp;
    }
    case N_BINARY: { char *l=gen_expr(e->a,setup); char *r=gen_expr(e->b,setup); return sfmt("%s(%s, %s)",op_cfn(e->op),l,r); }
    case N_UNARY: { char *v=gen_expr(e->a,setup); return strcmp(e->op,"not")==0?sfmt("cmm_not(%s)",v):sfmt("cmm_neg(%s)",v); }
    case N_EMPTYCHECK: { char *v=gen_expr(e->a,setup); return sfmt("cmm_bool(cmm_is_empty(%s))",v); }
    case N_INDEX: { char *t=gen_expr(e->a,setup); char *i=gen_expr(e->b,setup); return sfmt("cmm_index_get(%s, %s)",t,i); }
    case N_MEMBER: { char *t=gen_expr(e->a,setup); return sfmt("cmm_data_get(%s, cmm_str(%s))",t,cstring(e->name)); }
    case N_CALL: return render_call(e,setup);
    case N_RUN:  return gen_run(e,setup);
    case N_WAIT: { char *j=gen_expr(e->a,setup); return sfmt("cmm_job_wait(%s)",j); }
    default: die("internal: codegen expr kind %d",e->kind); return NULL;
    }
}

static void emit_setup(Buf *out,const char *pad,Vec *setup){
    for(int i=0;i<setup->len;i++) buf_addf(out,"%s%s\n",pad,(char*)setup->data[i]);
}
static void gen_stmt(Node *s, Buf *out, int ind);

static void emit_unlocks(Buf *out,const char *pad){
    for(int i=G_lockstack.len-1;i>=0;i--){ Lock *lk=(Lock*)G_lockstack.data[i];
        buf_addf(out,"%scmm_object_unlock(self.obj, %s, %d);\n",pad,lk->lk,lk->count); }
}

static void gen_block(Node **stmts,int n,Buf *out,int ind){ for(int i=0;i<n;i++) gen_stmt(stmts[i],out,ind); }

static void gen_stmt(Node *s, Buf *out, int ind){
    char *pad=pads(ind);
    if(g_debug && G_cur_class && s->line>0)
        buf_addf(out,"#line %d \"%s.cmm\"\n", s->line, G_cur_class->classname);
    switch(s->kind){
    case N_ASSIGN: {
        Vec setup={0}; char *val=gen_expr(s->b,&setup); emit_setup(out,pad,&setup);
        Node *tgt=s->a;
        if(tgt->kind==N_NAME) buf_addf(out,"%s%s = %s;\n",pad,cident(tgt->name),val);
        else if(tgt->kind==N_CLASSVARREF) buf_addf(out,"%scmm_field_set(self, %d, %s);\n",pad,mod_var_index(G_cur_class,tgt->name),val);
        else if(tgt->kind==N_INDEX){
            Vec is={0}; char *cont=gen_expr(tgt->a,&is); char *idx=gen_expr(tgt->b,&is); emit_setup(out,pad,&is);
            buf_addf(out,"%scmm_index_set(%s, %s, %s);\n",pad,cont,idx,val);
        }
        break;
    }
    case N_EMPTYDECL: buf_addf(out,"%s%s = %s;\n",pad,cident(s->name),typed_empty(s->tref)); break;
    case N_INCDEC: {
        Vec setup={0}; char *ref=gen_expr(s->a,&setup); emit_setup(out,pad,&setup);
        const char *op = strcmp(s->op,"++")==0?"cmm_add":"cmm_sub";
        if(s->a->kind==N_CLASSVARREF){ int idx=mod_var_index(G_cur_class,s->a->name);
            buf_addf(out,"%scmm_field_set(self, %d, %s(self.obj->fields[%d], cmm_int(1)));\n",pad,idx,op,idx);
        } else buf_addf(out,"%s%s = %s(%s, cmm_int(1));\n",pad,ref,op,ref);
        break;
    }
    case N_EXPRSTMT: { Vec setup={0}; char *ex=gen_expr(s->a,&setup); emit_setup(out,pad,&setup); buf_addf(out,"%s(void)(%s);\n",pad,ex); break; }
    case N_RETURN: {
        if(s->a==NULL){ emit_unlocks(out,pad); buf_addf(out,"%sreturn cmm_frame_leave(cmm_empty());\n",pad); break; }
        Vec setup={0}; char *v=gen_expr(s->a,&setup); emit_setup(out,pad,&setup);
        char *rv=new_tmp("__ret");
        buf_addf(out,"%sCmmValue %s = %s;\n",pad,rv,v);
        emit_unlocks(out,pad);
        buf_addf(out,"%sreturn cmm_frame_leave(%s);\n",pad,rv);
        break;
    }
    case N_IF: {
        Vec setup={0}; char *cond=gen_expr(s->a,&setup); emit_setup(out,pad,&setup);
        buf_addf(out,"%sif (cmm_truthy(%s)) {\n",pad,cond);
        gen_block(s->body,s->nbody,out,ind+1);
        if(s->has_else){ buf_addf(out,"%s} else {\n",pad); gen_block(s->els,s->nels,out,ind+1); }
        buf_addf(out,"%s}\n",pad);
        break;
    }
    case N_WHILE: {
        buf_addf(out,"%sfor (;;) {\n",pad);
        Vec setup={0}; char *cond=gen_expr(s->a,&setup);
        char *inner=pads(ind+1);
        emit_setup(out,inner,&setup);
        buf_addf(out,"%sif (!cmm_truthy(%s)) break;\n",inner,cond);
        gen_block(s->body,s->nbody,out,ind+1);
        buf_addf(out,"%s}\n",pad);
        break;
    }
    case N_FOR: {
        Vec setup={0}; char *it=gen_expr(s->a,&setup); emit_setup(out,pad,&setup);
        char *itv=new_tmp("__it"); char *nv=new_tmp("__n"); char *iv=new_tmp("__i");
        buf_addf(out,"%sCmmValue %s = %s;\n",pad,itv,it);
        buf_addf(out,"%ssize_t %s = cmm_iter_count(%s);\n",pad,nv,itv);
        buf_addf(out,"%sfor (size_t %s = 0; %s < %s; %s++) {\n",pad,iv,iv,nv,iv);
        char *inner=pads(ind+1);
        buf_addf(out,"%s%s = cmm_iter_at(%s, %s);\n",inner,cident(s->name),itv,iv);
        gen_block(s->body,s->nbody,out,ind+1);
        buf_addf(out,"%s}\n",pad);
        break;
    }
    case N_USELOCK: {
        /* sort field indices ascending */
        int k=s->nsvars; int *idxs=(int*)xmalloc(sizeof(int)*(size_t)(k>0?k:1));
        for(int i=0;i<k;i++) idxs[i]=mod_var_index(G_cur_class,s->svars[i]);
        for(int a=0;a<k;a++) for(int b=a+1;b<k;b++) if(idxs[b]<idxs[a]){ int t=idxs[a];idxs[a]=idxs[b];idxs[b]=t; }
        char *lk=new_tmp("__lk");
        buf_addf(out,"%s{\n",pad);
        char *inner=pads(ind+1);
        Buf arr={0}; for(int i=0;i<k;i++){ if(i) buf_add(&arr,", "); buf_addf(&arr,"%d",idxs[i]); }
        buf_addf(out,"%sint %s[] = { %s };\n",inner,lk,arr.p?arr.p:"");
        buf_addf(out,"%scmm_object_lock(self.obj, %s, %d);\n",inner,lk,k);
        Lock *L=(Lock*)xmalloc(sizeof(Lock)); L->lk=lk; L->count=k; vec_push(&G_lockstack,L);
        gen_block(s->body,s->nbody,out,ind+1);
        G_lockstack.len--;
        buf_addf(out,"%scmm_object_unlock(self.obj, %s, %d);\n",inner,lk,k);
        buf_addf(out,"%s}\n",pad);
        break;
    }
    default: die("internal: codegen stmt kind %d",s->kind);
    }
}

static char *gen_function(Module *m, Function *fn){
    G_cur_class=m; G_lockstack.len=0;
    Buf out={0};
    buf_addf(&out,"%s {\n",proto(m,fn));
    buf_add(&out,"    cmm_frame_enter();\n");
    Vec names={0}, seen={0}, params={0};
    for(int i=0;i<fn->nparams;i++) vec_push(&params,(void*)fn->params[i]->name);
    collect_walk(fn->body,fn->nbody,&names,&seen,&params);
    for(int i=0;i<names.len;i++) buf_addf(&out,"    CmmValue %s = cmm_empty();\n",cident((char*)names.data[i]));
    gen_block(fn->body,fn->nbody,&out,1);
    buf_add(&out,"    return cmm_frame_leave(cmm_empty());\n}");
    return out.p;
}

static char *codegen_generate(const char *entry){
    Buf out={0};
    G_classes=(Module**)g_program.data; G_nclasses=g_program.len;
    G_thunks.len=0; G_tmp=0; G_lockstack.len=0;

    emit(&out,"/* Generated by cmmc. Do not edit. */");
    emit(&out,"#include \"cmm_runtime.h\"");
    emit(&out,"");
    for(int i=0;i<G_nclasses;i++) buf_addf(&out,"#define CLASS_%s %d\n",cident(G_classes[i]->classname),G_classid(G_classes[i]->classname));
    emit(&out,"");
    for(int i=0;i<G_nclasses;i++){
        Module *m=G_classes[i];
        buf_addf(&out,"static CmmValue %s_new(void);\n",cident(m->classname));
        for(int j=0;j<m->nfuncs;j++) buf_addf(&out,"%s;\n",proto(m,m->funcs[j]));
    }
    emit(&out,"");

    Vec bodies={0};
    for(int i=0;i<G_nclasses;i++){
        Module *m=G_classes[i];
        vec_push(&bodies,gen_constructor(m));
        for(int j=0;j<m->nfuncs;j++) vec_push(&bodies,gen_function(m,m->funcs[j]));
    }
    for(int i=0;i<G_thunks.len;i++) emit(&out,(char*)G_thunks.data[i]);
    if(G_thunks.len) emit(&out,"");
    for(int i=0;i<bodies.len;i++){ emit(&out,(char*)bodies.data[i]); emit(&out,""); }

    /* main */
    char *cls=cident(entry);
    emit(&out,"int main(int argc, char **argv) {");
    emit(&out,"    cmm_set_args(argc, argv);");
    emit(&out,"    cmm_init();");
    buf_addf(&out,"    CmmValue __entry = %s_new();\n",cls);
    Module *em=find_class(entry); int has_main=0;
    for(int j=0;j<em->nfuncs;j++) if(strcmp(em->funcs[j]->name,"main")==0) has_main=1;
    if(has_main) buf_addf(&out,"    %s_main(__entry);\n",cls);
    emit(&out,"    return 0;");
    emit(&out,"}");
    return out.p;
}

/* ===================================================================== */
/* Driver: imports, backend C compiler, build                           */
/* ===================================================================== */
#include <sys/stat.h>
#ifdef _WIN32
  #include <windows.h>
  #include <direct.h>
#else
  #include <unistd.h>
#endif

static char *read_file(const char *path){
    FILE *f=fopen(path,"rb"); if(!f) return NULL;
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    if(n<0){ fclose(f); return NULL; }
    char *buf=(char*)xmalloc((size_t)n+1);
    size_t rd=fread(buf,1,(size_t)n,f); fclose(f);
    buf[rd]=0; return buf;
}
static int file_exists(const char *path){ FILE *f=fopen(path,"rb"); if(f){fclose(f);return 1;} return 0; }
static void write_file(const char *path,const char *data,size_t n){
    FILE *f=fopen(path,"wb"); if(!f) die("cannot write %s",path);
    fwrite(data,1,n,f); fclose(f);
}

static int is_sep(char c){ return c=='/'||c=='\\'; }
static char *path_dirname(const char *p){
    int last=-1; for(int i=0;p[i];i++) if(is_sep(p[i])) last=i;
    if(last<0) return xstrdup(".");
    if(last==0) return xstrdup("/");
    return xstrndup(p,(size_t)last);
}
static const char *path_basename(const char *p){
    const char *b=p; for(const char *q=p;*q;q++) if(is_sep(*q)) b=q+1; return b;
}
static char *base_noext(const char *p){
    const char *b=path_basename(p);
    const char *dot=NULL; for(const char *q=b;*q;q++) if(*q=='.') dot=q;
    if(!dot) return xstrdup(b);
    return xstrndup(b,(size_t)(dot-b));
}
static char *strip_ext(const char *p){
    const char *b=path_basename(p); const char *dot=NULL;
    for(const char *q=b;*q;q++) if(*q=='.') dot=q;
    if(!dot) return xstrdup(p);
    return xstrndup(p,(size_t)(dot-p));
}
static char *path_join(const char *a,const char *b){
    size_t n=strlen(a); if(n&&is_sep(a[n-1])) return sfmt("%s%s",a,b);
    return sfmt("%s%c%s",a,PATHSEP,b);
}
static char *abspath(const char *p){
#ifdef _WIN32
    char buf[4096]; if(_fullpath(buf,p,sizeof buf)) return xstrdup(buf); return xstrdup(p);
#else
    char buf[4096]; if(realpath(p,buf)) return xstrdup(buf);
    /* file may not exist yet: fall back to cwd join */
    if(p[0]=='/') return xstrdup(p);
    char cwd[4096]; if(getcwd(cwd,sizeof cwd)) return path_join(cwd,p);
    return xstrdup(p);
#endif
}

/* ---- import resolution ---- */
static char *g_self_dir=NULL;    /* set in main(); dir of the cmmc executable */

/* Find Name.cmm across: entry dir, $CMMC_PATH, bundled stdlib. NULL if none. */
static char *resolve_use(const char *name, const char *entry_dir){
    char *fname=sfmt("%s.cmm",name);
    char *cand;
    cand=path_join(entry_dir,fname); if(file_exists(cand)) return cand;
    const char *envp=getenv("CMMC_PATH");
    if(envp && *envp){
        char *dup=xstrdup(envp);
#ifdef _WIN32
        const char *seps=";";
#else
        const char *seps=":";
#endif
        for(char *tok=strtok(dup,seps); tok; tok=strtok(NULL,seps)){
            cand=path_join(tok,fname); if(file_exists(cand)) return cand;
        }
    }
    if(g_self_dir){
        cand=path_join(path_join(g_self_dir,"stdlib"),fname); if(file_exists(cand)) return cand;
        cand=path_join(path_join(path_dirname(g_self_dir),"stdlib"),fname); if(file_exists(cand)) return cand;
    }
    return NULL;
}

static char *load_program(const char *entry_path, Vec *remote){
    char *entry_abs=abspath(entry_path);
    char *search_dir=path_dirname(entry_abs);
    Vec pending={0}, seen={0};
    vec_push(&pending,entry_abs);
    while(pending.len){
        char *path=(char*)pending.data[--pending.len];
        int dup=0; for(int i=0;i<seen.len;i++) if(strcmp((char*)seen.data[i],path)==0){dup=1;break;}
        if(dup) continue; vec_push(&seen,path);
        if(!file_exists(path)) die("cannot find source file: %s",path);
        char *src=read_file(path); if(!src) die("cannot read %s",path);
        Module *m=parse_source(src,path);
        char *expected=base_noext(path);
        if(strcmp(m->classname,expected)!=0)
            die("%s: class '%s' must live in a file named '%s.cmm'",path,m->classname,m->classname);
        if(!find_class(m->classname)) vec_push(&g_program,m);
        for(int i=0;i<m->nuses;i++){
            Use *u=m->uses[i];
            if(u->url){ vec_push(remote,(void*)u->url); continue; }
            const char *name=u->name;
            if(is_native_cls(name)) continue;
            if(find_class(name)) continue;
            char *cand=resolve_use(name, search_dir);
            if(!cand)
                die("%s: cannot resolve `use %s;` — no %s.cmm found next to the "
                    "entry file, in $CMMC_PATH, or in the bundled stdlib",path,name,name);
            vec_push(&pending,cand);
        }
    }
    return base_noext(entry_abs);
}

/* ---- backend C compiler ---- */
static int run_cmd(const char *cmd,int quiet){
    char *base = quiet ? sfmt("%s >%s 2>&1",cmd,NULLDEV) : xstrdup(cmd);
#ifdef _WIN32
    /* cmd.exe strips the outer quote pair when a command both starts and ends
       with '"'. Our compile commands do exactly that, which corrupts the
       executable name ("gcc" "...out.exe" -> gcc" ... "out.exe). Wrap the whole
       command in an extra quote pair so cmd strips ours, not the real ones. */
    char *full = sfmt("\"%s\"", base);
#else
    char *full = base;
#endif
    int rc=system(full);
    return rc;
}

/* Directory containing the running cmmc executable (for bundled toolchains). */
static char *self_exe_dir(const char *argv0){
#ifdef _WIN32
    char buf[4096]; DWORD n=GetModuleFileNameA(NULL,buf,(DWORD)sizeof buf);
    if(n>0 && n<sizeof buf){ buf[n]=0; return path_dirname(buf); }
#else
    char buf[4096]; ssize_t n=readlink("/proc/self/exe",buf,sizeof buf-1);
    if(n>0){ buf[n]=0; return path_dirname(buf); }
#endif
    if(argv0 && strpbrk(argv0,"/\\")) return path_dirname(abspath(argv0));
    return xstrdup(".");   /* last resort: only bundle if cwd has one */
}

/* Discovered toolchain. kind: 0 gcc-like, 1 msvc, 2 tcc. */
static int   g_kind=0;
static char *g_cc=NULL;          /* command / path to invoke               */
static char *g_extra_inc=NULL;   /* bundled include dir, or NULL           */
static char *g_extra_lib=NULL;   /* bundled lib dir, or NULL               */
static int   g_bundled=0;
static const char *g_cc_override=NULL; /* from --cc or $CMMC_CC           */
static int   g_static=0;               /* --static: link statically       */
static int   g_no_verify=0;            /* --no-verify: drop CA bundle, no cert check */
static const char *g_target_os=NULL;   /* --target-os linux|windows|macos */

/* render the compiler command: a multi-word --cc (e.g. "zig cc -target ...")
   is emitted verbatim; a bare path is quoted. */
static void cc_prefix(char *buf, size_t n){
    if(g_cc && strchr(g_cc,' ')) snprintf(buf,n,"%s",g_cc);
    else snprintf(buf,n,"\"%s\"",g_cc?g_cc:"cc");
}
/* is the *target* Windows? defaults to the host unless --target-os overrides */
static int target_is_windows(void){
    if(g_target_os) return strcmp(g_target_os,"windows")==0;
#ifdef _WIN32
    return 1;
#else
    return 0;
#endif
}

static char *tool_in(const char *dir,const char *name);
/* Find a zig toolchain: $CMMC_ZIG, bundled bin/zig, zig on PATH, or the
   `python -m ziglang` pip package. Returns the command prefix or NULL. */
static char *find_zig(void){
    const char *e=getenv("CMMC_ZIG"); if(e&&*e) return xstrdup(e);
    if(g_self_dir){ char *p=tool_in(path_join(g_self_dir,"bin"),"zig"); if(p) return p; }
    if(run_cmd("zig version",1)==0) return xstrdup("zig");
    if(run_cmd("python3 -m ziglang version",1)==0) return xstrdup("python3 -m ziglang");
    if(run_cmd("python -m ziglang version",1)==0)  return xstrdup("python -m ziglang");
    return NULL;
}
static const char *g_target_name=NULL;   /* --target / --lambda */

/* Configure a cross-target preset (Amazon Linux). Uses zig as the backend. */
static void setup_target(void){
    if(!g_target_name) return;
    const char *zt; int stat=0;
    if(!strcmp(g_target_name,"al2023")||!strcmp(g_target_name,"lambda")||
       !strcmp(g_target_name,"amazonlinux")) zt="x86_64-linux-gnu.2.34";
    else if(!strcmp(g_target_name,"al2023-arm64")) zt="aarch64-linux-gnu.2.34";
    else if(!strcmp(g_target_name,"al2023-static")){ zt="x86_64-linux-musl"; stat=1; }
    else { die("unknown --target '%s' (use: al2023, al2023-arm64, al2023-static)",g_target_name); return; }
    g_target_os="linux"; if(stat) g_static=1;
    if(!g_cc && !g_cc_override && !getenv("CMMC_CC")){
        char *zig=find_zig();
        if(!zig) die("building for Amazon Linux needs zig (one toolchain for all hosts).\n"
                     "  install once:  pip install ziglang     (Windows and Linux)\n"
                     "  or put zig from ziglang.org on PATH, or set CMMC_ZIG=<path>.");
        g_cc=sfmt("%s cc -target %s",zig,zt); g_kind=0;
    }
}

static int infer_kind(const char *p){
    const char *b=path_basename(p);
    if(strstr(b,"tcc")) return 2;
    if(strcmp(b,"cl")==0 || strcmp(b,"cl.exe")==0) return 1;
    return 0;
}
static char *tool_in(const char *dir,const char *name){
    char *p=path_join(dir, sfmt("%s%s",name,EXESUF));
    if(file_exists(p)) return p;
    return NULL;
}

/* Resolve once: explicit override -> zig (preferred) -> bundled -> PATH. */
static void resolve_toolchain(void){
    if(g_cc) return;
    const char *ov = g_cc_override ? g_cc_override : getenv("CMMC_CC");
    if(ov && *ov){ g_cc=xstrdup(ov); g_kind=infer_kind(ov); return; }

    /* one toolchain everywhere: prefer zig when present */
    { char *zig=find_zig(); if(zig){ g_cc=sfmt("%s cc",zig); g_kind=0; return; } }

    if(g_self_dir){
        char *bin=path_join(g_self_dir,"bin");
        static const char *names[]={"gcc","clang","cc","tcc"};
        for(int i=0;i<4;i++){
            char *cand=tool_in(bin,names[i]);
            if(cand){ g_cc=cand; g_kind=infer_kind(cand); g_bundled=1; break; }
        }
        if(g_bundled){
            char *inc=path_join(g_self_dir,"include");
            char *lib=path_join(g_self_dir,"lib");
            if(file_exists(inc) || 1) g_extra_inc=inc;  /* pass even if empty */
            if(file_exists(lib) || 1) g_extra_lib=lib;
            return;
        }
    }
    /* PATH search */
    static const char *cands[]={"clang","gcc","cc"};
    for(int i=0;i<3;i++){
        char *probe=sfmt("%s --version",cands[i]);
        if(run_cmd(probe,1)==0){ g_cc=xstrdup(cands[i]); g_kind=0; return; }
    }
    if(run_cmd("cl",1)==0){ g_cc=xstrdup("cl"); g_kind=1; }   /* MSVC dev prompt */
}


/* ---- temp dir + embedded runtime ---- */
static char *make_temp_dir(void){
#ifdef _WIN32
    char base[4096]; DWORD n=GetTempPathA(sizeof base,base); (void)n;
    char *dir=sfmt("%scmmc_%lu",base,(unsigned long)GetCurrentProcessId());
    _mkdir(dir); return dir;
#else
    char tmpl[]="/tmp/cmmcXXXXXX";
    char *d=mkdtemp(tmpl);
    if(!d) die("cannot create temp dir");
    return xstrdup(d);
#endif
}
static char *write_runtime(const char *tmpdir){
    char *h=path_join(tmpdir,"cmm_runtime.h");
    char *c=path_join(tmpdir,"cmm_runtime.c");
    write_file(h,(const char*)RUNTIME_H,(size_t)RUNTIME_H_len);
    write_file(c,(const char*)RUNTIME_C,(size_t)RUNTIME_C_len);
    return c;
}

/* ---- vendored mbedTLS: out-of-the-box, cross-platform TLS -------------- *
 * TLS is provided by a vendored mbedTLS tree (third_party/) that the active
 * C backend (zig) compiles for whatever target we're building — host, AL2023,
 * arm64, Windows. The combined object is built once per target and cached, so
 * only the first TLS build pays the ~15s; later builds just link it.         */
static char *g_tls_dir = NULL;        /* dir holding mbedtls/ + cmm_ca_certs.h */
static int   g_tls_dir_known = 0;
static char *g_tls_obj = NULL;        /* cached combined object for this target */

static int has_file(const char *dir,const char *rel){
    char *p=sfmt("%s/%s",dir,rel); int ok=file_exists(p); return ok;
}
static char *find_tls_dir(void){
    if(g_tls_dir_known) return g_tls_dir;
    g_tls_dir_known=1;
    const char *env=getenv("CMMC_TLS_DIR");
    if(env && *env && has_file(env,"mbedtls/include/mbedtls/ssl.h")){ g_tls_dir=xstrdup(env); return g_tls_dir; }
    if(g_self_dir){
        static const char *rels[]={"third_party","../third_party","../../third_party","../lib/third_party"};
        for(int i=0;i<4;i++){
            char *d=path_join(g_self_dir,(char*)rels[i]);
            if(has_file(d,"mbedtls/include/mbedtls/ssl.h") && has_file(d,"cmm_ca_certs.h")){ g_tls_dir=d; return d; }
        }
    }
    return NULL;
}
static char *cache_root(void){
    const char *c=getenv("CMMC_CACHE"); if(c&&*c) return xstrdup(c);
#ifdef _WIN32
    const char *la=getenv("LOCALAPPDATA"); if(la&&*la) return sfmt("%s\\cmm\\cache",la);
    return xstrdup("cmm-cache");
#else
    const char *h=getenv("HOME"); if(h&&*h) return sfmt("%s/.cache/cmm",h);
    return xstrdup("/tmp/cmm-cache");
#endif
}
static void mkdir_one(const char *p){
#ifdef _WIN32
    _mkdir(p);
#else
    mkdir(p,0755);
#endif
}
static void mkdir_p(const char *path){
    char *t=xstrdup(path); size_t n=strlen(t);
    for(size_t i=1;i<n;i++){ if(t[i]=='/'||t[i]=='\\'){ char c=t[i]; t[i]=0; mkdir_one(t); t[i]=c; } }
    mkdir_one(t); free(t);
}
/* Build (once) and cache the combined mbedTLS object for the current target.
   Returns the object path, or NULL if TLS can't be provided. */
static char *ensure_tls_lib(int verbose){
    if(g_tls_obj) return g_tls_obj;
    if(g_kind!=0) return NULL;                 /* mbedTLS path is for gcc/clang/zig */
    char *tdir=find_tls_dir();
    if(!tdir) return NULL;
    const char *key=g_target_name?g_target_name:(g_static?"host-static":"host");
    char *cdir=sfmt("%s%ctls%c%s",cache_root(),PATHSEP,PATHSEP,key);
    mkdir_p(cdir);
    char *obj=path_join(cdir, target_is_windows()?"cmmtls_min.a":"cmmtls_min.o");
    if(file_exists(obj)){ g_tls_obj=obj; return obj; }
    char *manifest=sfmt("%s/mbedtls/sources.list",tdir);
    char *list=read_file(manifest);
    if(!list){ return NULL; }
    char ccp[1024]; cc_prefix(ccp,sizeof ccp);
    if(verbose) fprintf(stderr,"  building TLS support for '%s' (one-time)...\n",key);

    if(target_is_windows()){
        /* COFF can't merge objects into one; build a cached static archive. */
        char *objdir=sfmt("%s%cobj",cdir,PATHSEP); mkdir_p(objdir);
        Buf objs={0};
        char *p=list;
        while(*p){
            char *nl=strchr(p,'\n'); if(nl)*nl=0;
            size_t L=strlen(p); if(L&&p[L-1]=='\r')p[L-1]=0;
            if(*p){
                char base[256]; snprintf(base,sizeof base,"%s",p);
                char *dot=strrchr(base,'.'); if(dot)*dot=0;
                char *o=sfmt("%s%c%s.o",objdir,PATHSEP,base);
                char *cc1=sfmt("%s -w -O2 -I\"%s/mbedtls/include\" -I\"%s\" \"-DMBEDTLS_CONFIG_FILE=<cmm_mbedtls_config.h>\" -c \"%s/mbedtls/library/%s\" -o \"%s\"",
                               ccp,tdir,tdir,tdir,p,o);
                if(run_cmd(cc1,verbose?0:1)!=0) return NULL;
                buf_addf(&objs," \"%s\"",o);
            }
            if(!nl) break; p=nl+1;
        }
        char *arc=path_join(cdir,"cmmtls_min.a");
        /* archiver mirrors the backend: '<zig> ar', else system ar */
        char *zig=find_zig();
        char *arp = zig ? sfmt("%s ar",zig) : (char*)"ar";
        char *arcmd=sfmt("%s rcs \"%s\"%s",arp,arc,objs.p);
        if(run_cmd(arcmd,verbose?0:1)!=0) return NULL;
        g_tls_obj=arc; return arc;
    }

    /* ELF/Mach: partial-link everything into one cached relocatable object. */
    Buf rsp={0};
    buf_addf(&rsp,"-w\n-O2\n-ffunction-sections\n-fdata-sections\n-I\"%s/mbedtls/include\"\n-I\"%s\"\n-DMBEDTLS_CONFIG_FILE=<cmm_mbedtls_config.h>\n-r\n",tdir,tdir);
    char *p=list;
    while(*p){
        char *nl=strchr(p,'\n'); if(nl)*nl=0;
        size_t L=strlen(p); if(L&&p[L-1]=='\r')p[L-1]=0;
        if(*p) buf_addf(&rsp,"\"%s/mbedtls/library/%s\"\n",tdir,p);
        if(!nl) break; p=nl+1;
    }
    buf_addf(&rsp,"-o\n\"%s\"\n",obj);
    char *rspf=path_join(cdir,"build.rsp");
    write_file(rspf,rsp.p,rsp.len);
    char *cmd=sfmt("%s @\"%s\"",ccp,rspf);
    if(run_cmd(cmd,verbose?0:1)!=0){ remove(obj); return NULL; }
    g_tls_obj=obj; return obj;
}

/* tls: 0=off, 1=auto, 2=on */
static void compile_to_exe(const char *c_path,const char *out_path,int verbose,int tls,int uses_http){
    setup_target();
    resolve_toolchain();
    if(!g_cc) die("no C compiler found.\n"
                  "  cmm compiles through C, so a C compiler must be available. Options:\n"
                  "    - drop gcc.exe (and its DLLs) into <cmmc-dir>\\bin  (Windows: e.g. w64devkit/winlibs MinGW)\n"
                  "    - add a compiler to PATH (gcc, clang, or MSVC cl in a Developer Prompt)\n"
                  "    - set CMMC_CC=<path-to-compiler>, or pass --cc <path>");
    char *tmpdir=make_temp_dir();
    char *runtime_c=write_runtime(tmpdir);

    int use_tls=0;
    /* auto (tls==1): only link TLS if the program actually calls Http.* */
    int want_tls = (tls==2) || (tls==1 && uses_http);
    if(want_tls){
        if(ensure_tls_lib(verbose)) use_tls=1;
        else if(tls==2)
            die("TLS requested (--tls) but the bundled TLS library was not found.\n"
                "  cmm ships TLS as a vendored mbedTLS tree compiled by zig. Expected\n"
                "  a 'third_party/mbedtls' directory next to cmmc (or set CMMC_TLS_DIR),\n"
                "  and a zig backend (pip install ziglang). See docs/networking.");
    }

    Buf cmd={0};
    if(g_kind==1){                         /* MSVC */
        buf_addf(&cmd,"\"%s\" /nologo %s /I\"%s\"",g_cc, g_debug?"/Od /Zi /DCMM_DEBUG":"/O2 /Gy /Gw", tmpdir);
        if(g_extra_inc) buf_addf(&cmd," /I\"%s\"",g_extra_inc);
        if(use_tls) buf_add(&cmd," /DCMM_HAVE_TLS");
        buf_addf(&cmd," \"%s\" \"%s\" ws2_32.lib advapi32.lib",c_path,runtime_c);
        if(use_tls) buf_add(&cmd," libssl.lib libcrypto.lib");
        buf_addf(&cmd," /Fe:\"%s\"",out_path);
        if(!g_debug) buf_add(&cmd," /link /OPT:REF /OPT:ICF");
        if(g_extra_lib) buf_addf(&cmd," /link /LIBPATH:\"%s\"",g_extra_lib);
    } else if(g_kind==2){                  /* TinyCC */
        char ccp[1024]; cc_prefix(ccp,sizeof ccp);
        buf_addf(&cmd,"%s -I\"%s\"",ccp,tmpdir);
        if(g_debug) buf_add(&cmd," -g -DCMM_DEBUG");
        if(g_static) buf_add(&cmd," -static");
        if(g_extra_inc) buf_addf(&cmd," -I\"%s\"",g_extra_inc);
        if(use_tls) buf_add(&cmd," -DCMM_HAVE_TLS");
        buf_addf(&cmd," \"%s\" \"%s\" -o \"%s\"",c_path,runtime_c,out_path);
        if(g_extra_lib) buf_addf(&cmd," -L\"%s\"",g_extra_lib);
        if(target_is_windows()) buf_add(&cmd," -lws2_32 -ladvapi32"); else buf_add(&cmd," -lpthread -lm");
        if(use_tls) buf_add(&cmd," -lssl -lcrypto");
    } else {                               /* gcc / clang / zig cc */
        char ccp[1024]; cc_prefix(ccp,sizeof ccp);
        buf_addf(&cmd,"%s -std=c99 %s -I\"%s\"",ccp, g_debug?"-O0 -g -DCMM_DEBUG":"-O2 -ffunction-sections -fdata-sections", tmpdir);
        if(g_static) buf_add(&cmd," -static");
        if(g_extra_inc) buf_addf(&cmd," -I\"%s\"",g_extra_inc);
        if(g_extra_lib) buf_addf(&cmd," -L\"%s\"",g_extra_lib);
        if(use_tls) buf_addf(&cmd," -DCMM_HAVE_TLS -I\"%s/mbedtls/include\" -I\"%s\" \"-DMBEDTLS_CONFIG_FILE=<cmm_mbedtls_config.h>\"",g_tls_dir,g_tls_dir);
        if(use_tls && g_no_verify) buf_add(&cmd," -DCMM_TLS_NO_VERIFY");
        buf_addf(&cmd," \"%s\" \"%s\" -o \"%s\" -lm",c_path,runtime_c,out_path);
        if(target_is_windows()) buf_add(&cmd," -lws2_32 -ladvapi32"); else buf_add(&cmd," -lpthread");
        if(use_tls && g_tls_obj){ buf_addf(&cmd," \"%s\"",g_tls_obj);
            if(target_is_windows()) buf_add(&cmd," -lbcrypt"); }
        if(!g_debug) buf_add(&cmd," -Wl,--gc-sections -s");
    }
    if(verbose||g_debug){
        const char *kn = g_kind==1?"msvc":(g_kind==2?"tcc":"gcc-like");
        printf("  cc: %s  [%s%s]\n",g_cc,kn,g_bundled?", bundled":"");
        printf("  %s\n",cmd.p);
        printf("  TLS: %s\n", use_tls?"enabled (OpenSSL)":"disabled (https:// will return empty)");
    }
    int rc=run_cmd(cmd.p,0);
    remove(runtime_c);
    { char *h=path_join(tmpdir,"cmm_runtime.h"); remove(h); }
#ifndef _WIN32
    rmdir(tmpdir);
#endif
    if(rc!=0) die("C compiler failed");
}

/* ---- top-level build ---- */
static char *do_build(const char *entry_path,const char *out_opt,int emit_c,int keep_c,
                      int verbose,int tls,Vec *remote){
    char *entry=load_program(entry_path,remote);
    analyze_program();
    char *c_src=codegen_generate(entry);

    char *base=strip_ext(abspath(entry_path));
    char *c_path=sfmt("%s.c",base);
    write_file(c_path,c_src,strlen(c_src));

    const char *suf = g_target_os ? (strcmp(g_target_os,"windows")==0?".exe":"") : EXESUF;
    char *out_path = out_opt ? xstrdup(out_opt) : sfmt("%s%s",base,suf);
    if(emit_c){ if(verbose) printf("  wrote %s\n",c_path); return c_path; }
    int uses_http = strstr(c_src,"cmm_http_")!=NULL;
    compile_to_exe(c_path,out_path,verbose,tls,uses_http);
    if(!keep_c) remove(c_path);
    return out_path;
}

/* ===================================================================== */
/* CLI                                                                   */
/* ===================================================================== */
#define CMMC_VERSION "0.8.0"
static const char *USAGE =
    "cmmc - the C-- (cmm) compiler\n"
    "Usage:\n"
    "    cmmc build <file.cmm> [-o OUT] [--emit-c] [--keep-c] [--tls|--no-tls] [-v]\n"
    "    cmmc run   <file.cmm> [-v] [-- ARGS...]\n"
    "    cmmc emit  <file.cmm>            # write the generated C and stop\n"
    "    cmmc check <file.cmm>            # parse + type-check only; reports all errors\n"
    "    --debug/-g on build/run: -O0 -g, #line source maps, CMM_DEBUG runtime\n"
    "    cmmc version\n";

static void print_remote(Vec *remote){
    if(remote->len==0) return;
    fprintf(stderr,"note: remote `use \"<url>\";` imports are not fetched in this build; "
                   "referenced classes must be available locally:\n");
    for(int i=0;i<remote->len;i++) fprintf(stderr,"      %s\n",(char*)remote->data[i]);
}

static int cmd_build(int argc,char **argv,int do_emit){
    const char *out=NULL,*file=NULL; int emit_c=do_emit,keep_c=0,verbose=0,tls=1;
    for(int i=0;i<argc;i++){
        char *a=argv[i];
        if((strcmp(a,"-o")==0||strcmp(a,"--out")==0)&&i+1<argc) out=argv[++i];
        else if(strcmp(a,"--emit-c")==0) emit_c=1;
        else if(strcmp(a,"--keep-c")==0) keep_c=1;
        else if(strcmp(a,"--no-tls")==0) tls=0;
        else if(strcmp(a,"--tls")==0) tls=2;
        else if(strcmp(a,"--no-verify")==0) g_no_verify=1;
        else if(strcmp(a,"--cc")==0 && i+1<argc) g_cc_override=argv[++i];
        else if(strcmp(a,"--static")==0) g_static=1;
        else if(strcmp(a,"--target")==0 && i+1<argc) g_target_name=argv[++i];
        else if(strcmp(a,"--target-os")==0 && i+1<argc) g_target_os=argv[++i];
        else if(strcmp(a,"--lambda")==0) g_target_name="al2023";
        else if(strcmp(a,"-v")==0||strcmp(a,"--verbose")==0) verbose=1;
        else if(strcmp(a,"--debug")==0||strcmp(a,"-g")==0){ g_debug=1; keep_c=1; }
        else file=a;
    }
    if(!file){ fprintf(stderr,"cmmc build: expected exactly one .cmm file\n"); return 2; }
    Vec remote={0};
    char *res=do_build(file,out,emit_c,keep_c,verbose,tls,&remote);
    if(emit_c){ printf("%s\n",res); return 0; }
    printf("built %s\n",res);
    print_remote(&remote);
    return 0;
}

static int cmd_run(int argc,char **argv){
    int verbose=0; const char *file=NULL; int passthru_at=-1;
    for(int i=0;i<argc;i++){
        if(strcmp(argv[i],"--")==0){ passthru_at=i; break; }
        if(strcmp(argv[i],"-v")==0||strcmp(argv[i],"--verbose")==0) verbose=1;
        else if(strcmp(argv[i],"--cc")==0 && i+1<argc) g_cc_override=argv[++i];
        else if(strcmp(argv[i],"--static")==0) g_static=1;
        else if(strcmp(argv[i],"--target")==0 && i+1<argc) g_target_name=argv[++i];
        else if(strcmp(argv[i],"--target-os")==0 && i+1<argc) g_target_os=argv[++i];
        else if(strcmp(argv[i],"--lambda")==0) g_target_name="al2023";
        else if(strcmp(argv[i],"--no-tls")==0) { /* run always tries tls; ignore */ }
        else if(strcmp(argv[i],"--debug")==0||strcmp(argv[i],"-g")==0) g_debug=1;
        else file=argv[i];
    }
    if(!file){ fprintf(stderr,"cmmc run: expected exactly one .cmm file\n"); return 2; }
    Vec remote={0};
    char *exe=do_build(file,NULL,0,0,verbose,1,&remote);
    print_remote(&remote);
    Buf cmd={0}; buf_addf(&cmd,"\"%s\"",exe);
    if(passthru_at>=0) for(int i=passthru_at+1;i<argc;i++) buf_addf(&cmd," \"%s\"",argv[i]);
    int rc=system(cmd.p);
#ifdef _WIN32
    return rc;
#else
    if(rc==-1) return 1;
    return (rc&0x7f)? 128+(rc&0x7f) : ((rc>>8)&0xff);
#endif
}

static int cmd_check(int argc,char **argv){
    const char *file=NULL;
    for(int i=0;i<argc;i++){ if(argv[i][0]!='-') file=argv[i]; }
    if(!file){ fprintf(stderr,"cmmc check: expected exactly one .cmm file\n"); return 2; }
    Vec remote={0};
    g_collect=1;
    load_program(file,&remote);
    analyze_program();
    for(int i=0;i<g_nadiags;i++)
        fprintf(stderr,"%s:%d:%d: error: %s\n",
            g_adiags[i].file?g_adiags[i].file:"<input>",
            g_adiags[i].line, g_adiags[i].col, g_adiags[i].msg);
    return g_nadiags?1:0;
}

int main(int argc,char **argv){
    init_tables();
    g_self_dir = self_exe_dir(argc>0?argv[0]:NULL);
    if(argc<2){ fputs(USAGE,stderr); return 2; }
    const char *cmd=argv[1];
    if(strcmp(cmd,"build")==0) return cmd_build(argc-2,argv+2,0);
    if(strcmp(cmd,"run")==0)   return cmd_run(argc-2,argv+2);
    if(strcmp(cmd,"emit")==0)  return cmd_build(argc-2,argv+2,1);
    if(strcmp(cmd,"check")==0) return cmd_check(argc-2,argv+2);
    if(strcmp(cmd,"version")==0||strcmp(cmd,"--version")==0||strcmp(cmd,"-V")==0){ printf("cmmc %s\n",CMMC_VERSION); return 0; }
    if(strcmp(cmd,"help")==0||strcmp(cmd,"--help")==0||strcmp(cmd,"-h")==0){ fputs(USAGE,stdout); return 0; }
    fprintf(stderr,"cmmc: unknown command '%s'\n",cmd); fputs(USAGE,stderr); return 2;
}
