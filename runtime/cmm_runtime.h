/* cmm_runtime.h - runtime support for the cmm language.
 *
 * Cross-platform (Windows first, then POSIX). A single tagged value type
 * (CmmValue) plus an arena allocator, native classes, OS threads for
 * run/wait and mutexes for `use` locks.
 */
#ifndef CMM_RUNTIME_H
#define CMM_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  typedef CRITICAL_SECTION cmm_mutex_t;
  typedef HANDLE           cmm_thread_t;
#else
  #include <pthread.h>
  typedef pthread_mutex_t  cmm_mutex_t;
  typedef pthread_t        cmm_thread_t;
#endif

typedef enum {
    CV_EMPTY = 0,
    CV_INT,
    CV_FLOAT,
    CV_BOOL,
    CV_STRING,
    CV_LIST,
    CV_DICT,
    CV_OBJECT,
    CV_SOCKET,
    CV_JOB
} CmmTag;

typedef struct CmmValue  CmmValue;
typedef struct CmmString CmmString;
typedef struct CmmList   CmmList;
typedef struct CmmDict   CmmDict;
typedef struct CmmObject CmmObject;
typedef struct CmmJob    CmmJob;
typedef struct CmmSocket CmmSocket;
typedef struct CmmArena  CmmArena;   /* opaque region; see cmm_runtime.c */

struct CmmValue {
    CmmTag tag;
    union {
        int64_t     i;
        double      f;
        int         b;
        CmmString *s;
        CmmList   *list;
        CmmDict   *dict;
        CmmObject *obj;
        CmmJob    *job;
        CmmSocket *sock;
    };
};

/* Every heap value records the arena that owns it. This lets the runtime
 * relocate a function's result into the caller's region on return and free
 * the function's own region (per-function reclaim), while keeping references
 * to longer-lived (e.g. @class-variable) data shared rather than copied. */
struct CmmString { char *data; size_t len;  CmmArena *arena; };
struct CmmList   { CmmValue *items; size_t len; size_t cap; CmmArena *arena; };
typedef struct { CmmString *key; CmmValue val; } CmmDictEntry;
struct CmmDict   { CmmDictEntry *entries; size_t len; size_t cap; CmmArena *arena; };

struct CmmObject {
    int          class_id;
    int          is_empty;
    size_t       nfields;
    CmmValue   *fields;
    cmm_mutex_t *locks;   /* nfields mutexes */
    CmmArena   *arena;
};

struct CmmJob {
    cmm_thread_t thread;
    CmmValue     result;
    int           started;
    CmmArena    *arena;
};

struct CmmSocket {
#ifdef _WIN32
    uintptr_t fd;
#else
    int fd;
#endif
    int open;
    CmmArena *arena;
};

/* ---- lifecycle ---- */
void  cmm_init(void);
void *cmm_alloc(size_t n);

/* ---- per-function regions (memory reclaim) ---- */
void      cmm_frame_enter(void);            /* push a fresh region          */
CmmValue cmm_frame_leave(CmmValue ret);   /* relocate ret to parent, free */
void      cmm_thread_attach(void);          /* root a worker thread's region */
void      cmm_field_set(CmmValue self, int idx, CmmValue v); /* @field = v */

/* ---- constructors ---- */
CmmValue cmm_empty(void);
CmmValue cmm_int(int64_t v);
CmmValue cmm_float(double v);
CmmValue cmm_bool(int v);
CmmValue cmm_str(const char *cstr);
CmmValue cmm_str_n(const char *p, size_t n);
CmmValue cmm_new_list(void);
CmmValue cmm_new_dict(void);
CmmObject *cmm_new_object(int class_id, size_t nfields, int is_empty);
CmmValue   cmm_object_value(CmmObject *o);   /* wraps as CV_OBJECT */

/* ---- core helpers ---- */
int        cmm_truthy(CmmValue v);
int        cmm_is_empty(CmmValue v);
CmmString *cmm_to_string(CmmValue v);          /* display form */
void       cmm_print(CmmValue v, int newline);

/* ---- operators (work across Int/Float/String/Data) ---- */
CmmValue cmm_add(CmmValue a, CmmValue b);
CmmValue cmm_sub(CmmValue a, CmmValue b);
CmmValue cmm_mul(CmmValue a, CmmValue b);
CmmValue cmm_div(CmmValue a, CmmValue b);
CmmValue cmm_mod(CmmValue a, CmmValue b);
CmmValue cmm_neg(CmmValue a);
CmmValue cmm_eq(CmmValue a, CmmValue b);
CmmValue cmm_ne(CmmValue a, CmmValue b);
CmmValue cmm_lt(CmmValue a, CmmValue b);
CmmValue cmm_le(CmmValue a, CmmValue b);
CmmValue cmm_gt(CmmValue a, CmmValue b);
CmmValue cmm_ge(CmmValue a, CmmValue b);
CmmValue cmm_and(CmmValue a, CmmValue b);
CmmValue cmm_or(CmmValue a, CmmValue b);
CmmValue cmm_not(CmmValue a);

/* ---- indexing ---- */
CmmValue cmm_index_get(CmmValue container, CmmValue index);
void      cmm_index_set(CmmValue container, CmmValue index, CmmValue val);

/* ---- iteration support ---- */
size_t    cmm_iter_count(CmmValue v);
CmmValue cmm_iter_at(CmmValue v, size_t i);

/* ---- mutex / locks ---- */
void cmm_mutex_init(cmm_mutex_t *m);
void cmm_mutex_lock(cmm_mutex_t *m);
void cmm_mutex_unlock(cmm_mutex_t *m);
void cmm_object_lock(CmmObject *o, const int *idx, int n);   /* sorted */
void cmm_object_unlock(CmmObject *o, const int *idx, int n);

/* ---- threads (run / wait) ---- */
CmmJob  *cmm_job_new(void);
void      cmm_thread_start(CmmJob *job, void *(*fn)(void *), void *arg);
CmmValue cmm_job_value(CmmJob *job);   /* wraps as CV_JOB */
CmmValue cmm_job_wait(CmmValue jobv);

/* ---- String methods ---- */
CmmValue cmm_string_length(CmmValue s);
CmmValue cmm_string_substring(CmmValue s, CmmValue a, CmmValue b);
CmmValue cmm_string_indexof(CmmValue s, CmmValue sub);
CmmValue cmm_string_contains(CmmValue s, CmmValue sub);
CmmValue cmm_string_startswith(CmmValue s, CmmValue sub);
CmmValue cmm_string_endswith(CmmValue s, CmmValue sub);
CmmValue cmm_string_upper(CmmValue s);
CmmValue cmm_string_lower(CmmValue s);
CmmValue cmm_string_trim(CmmValue s);
CmmValue cmm_string_split(CmmValue s, CmmValue sep);
CmmValue cmm_string_replace(CmmValue s, CmmValue a, CmmValue b);
CmmValue cmm_string_toint(CmmValue s);
CmmValue cmm_string_tofloat(CmmValue s);
CmmValue cmm_identity(CmmValue s);

/* ---- List methods ---- */
CmmValue cmm_list_add(CmmValue l, CmmValue v);
CmmValue cmm_list_remove(CmmValue l, CmmValue idx);
CmmValue cmm_list_get(CmmValue l, CmmValue idx);
CmmValue cmm_list_set(CmmValue l, CmmValue idx, CmmValue v);
CmmValue cmm_list_length(CmmValue l);
CmmValue cmm_list_contains(CmmValue l, CmmValue v);
CmmValue cmm_list_clear(CmmValue l);

/* ---- Dict methods ---- */
CmmValue cmm_dict_get(CmmValue d, CmmValue k);
CmmValue cmm_dict_set(CmmValue d, CmmValue k, CmmValue v);
CmmValue cmm_dict_has(CmmValue d, CmmValue k);
CmmValue cmm_dict_remove(CmmValue d, CmmValue k);
CmmValue cmm_dict_keys(CmmValue d);
CmmValue cmm_dict_length(CmmValue d);

/* ---- Data methods (Data is just a dynamic value) ---- */
CmmValue cmm_data_get(CmmValue d, CmmValue k);
CmmValue cmm_data_set(CmmValue d, CmmValue k, CmmValue v);
CmmValue cmm_data_has(CmmValue d, CmmValue k);
CmmValue cmm_data_length(CmmValue d);
CmmValue cmm_data_keys(CmmValue d);

/* ---- numeric conversions ---- */
CmmValue cmm_int_tostr(CmmValue v);
CmmValue cmm_int_tofloat(CmmValue v);
CmmValue cmm_float_tostr(CmmValue v);
CmmValue cmm_float_toint(CmmValue v);
CmmValue cmm_bool_tostr(CmmValue v);

/* ---- Console ---- */
CmmValue cmm_console_print(CmmValue v);
CmmValue cmm_console_println(CmmValue v);
CmmValue cmm_console_read(void);

/* ---- Math ---- */
CmmValue cmm_math_sqrt(CmmValue v);
CmmValue cmm_math_abs(CmmValue v);
CmmValue cmm_math_pow(CmmValue a, CmmValue b);
CmmValue cmm_math_floor(CmmValue v);
CmmValue cmm_math_ceil(CmmValue v);
CmmValue cmm_math_min(CmmValue a, CmmValue b);
CmmValue cmm_math_max(CmmValue a, CmmValue b);
CmmValue cmm_math_random(void);
CmmValue cmm_math_pi(void);

/* ---- File ---- */
CmmValue cmm_file_read(CmmValue path);
CmmValue cmm_file_write(CmmValue path, CmmValue data);
CmmValue cmm_file_append(CmmValue path, CmmValue data);
CmmValue cmm_file_exists(CmmValue path);
CmmValue cmm_file_delete(CmmValue path);

/* ---- Date ---- */
CmmValue cmm_date_now(void);
CmmValue cmm_date_format(CmmValue epoch, CmmValue fmt);

/* ---- Json ---- */
CmmValue cmm_json_encode(CmmValue v);
CmmValue cmm_json_decode(CmmValue s);
CmmValue cmm_json_pretty(CmmValue v);
CmmValue cmm_data_type(CmmValue d);
CmmValue cmm_data_is_object(CmmValue d);
CmmValue cmm_data_is_array(CmmValue d);
CmmValue cmm_data_is_null(CmmValue d);
CmmValue cmm_data_at(CmmValue d, CmmValue idx);
CmmValue cmm_data_get_str(CmmValue d, CmmValue key);
CmmValue cmm_data_get_int(CmmValue d, CmmValue key);
CmmValue cmm_data_get_float(CmmValue d, CmmValue key);
CmmValue cmm_data_get_bool(CmmValue d, CmmValue key);
CmmValue cmm_data_path(CmmValue d, CmmValue path);

/* ---- Socket / Http ---- */
CmmValue cmm_socket_connect(CmmValue host, CmmValue port);
CmmValue cmm_socket_write(CmmValue sock, CmmValue data);
CmmValue cmm_socket_read(CmmValue sock, CmmValue n);
CmmValue cmm_socket_readall(CmmValue sock);
CmmValue cmm_socket_close(CmmValue sock);
CmmValue cmm_http_get(CmmValue url);
CmmValue cmm_http_post(CmmValue url, CmmValue body);
CmmValue cmm_http_request(CmmValue method, CmmValue url, CmmValue headers, CmmValue body);
CmmValue cmm_http_send(CmmValue method, CmmValue url, CmmValue headers, CmmValue body);
CmmValue cmm_date_amz(void);
CmmValue cmm_date_date(CmmValue fmt, CmmValue epoch);
CmmValue cmm_date_gmdate(CmmValue fmt, CmmValue epoch);
CmmValue cmm_req_parse(CmmValue evstr);
CmmValue cmm_serve_listen(CmmValue port);
CmmValue cmm_serve_next(void);
CmmValue cmm_serve_respond(CmmValue resp);
CmmValue cmm_mysql_connect(CmmValue host, CmmValue port, CmmValue user, CmmValue pass, CmmValue db);
CmmValue cmm_mysql_connect_tls(CmmValue host, CmmValue port, CmmValue user, CmmValue pass, CmmValue db, CmmValue ca);
CmmValue cmm_mysql_query(CmmValue conn, CmmValue sql);
CmmValue cmm_mysql_exec(CmmValue conn, CmmValue sql);
CmmValue cmm_mysql_insert_id(CmmValue conn);
CmmValue cmm_mysql_affected(CmmValue conn);
CmmValue cmm_mysql_error(CmmValue conn);
CmmValue cmm_mysql_close(CmmValue conn);
CmmValue cmm_preg_match(CmmValue pattern, CmmValue subject);
CmmValue cmm_preg_test(CmmValue pattern, CmmValue subject);
CmmValue cmm_preg_match_all(CmmValue pattern, CmmValue subject);
CmmValue cmm_preg_replace(CmmValue pattern, CmmValue replacement, CmmValue subject);
CmmValue cmm_preg_split(CmmValue pattern, CmmValue subject);
CmmValue cmm_preg_quote(CmmValue str);
CmmValue cmm_crypto_sha256hex(CmmValue data);
CmmValue cmm_crypto_sha1hex(CmmValue data);
CmmValue cmm_crypto_hmac_sha256(CmmValue key, CmmValue data);
CmmValue cmm_crypto_hmac_sha256_hex(CmmValue key, CmmValue data);
CmmValue cmm_crypto_hmac_sha1(CmmValue key, CmmValue data);
CmmValue cmm_crypto_hotp(CmmValue seed, CmmValue counter, CmmValue digits);
CmmValue cmm_crypto_timing_safe_equal(CmmValue a, CmmValue b);
CmmValue cmm_crypto_hex(CmmValue data);
CmmValue cmm_crypto_random_hex(CmmValue nbytes);
CmmValue cmm_base64_encode(CmmValue data);
CmmValue cmm_base64_decode(CmmValue data);
CmmValue cmm_sys_exit(CmmValue code);
CmmValue cmm_sys_run(CmmValue cmd);
CmmValue cmm_sys_shell(CmmValue cmd);
CmmValue cmm_sys_env(CmmValue name);
void cmm_set_args(int argc, char **argv);
CmmValue cmm_sys_args(void);
CmmValue cmm_sys_cwd(void);
CmmValue cmm_sys_chdir(CmmValue path);
CmmValue cmm_lambda_next(void);
CmmValue cmm_lambda_success(CmmValue body);
CmmValue cmm_lambda_failure(CmmValue type, CmmValue msg);
CmmValue cmm_lambda_init_error(CmmValue type, CmmValue msg);
CmmValue cmm_lambda_request_id(void);
CmmValue cmm_lambda_deadline(void);
CmmValue cmm_lambda_arn(void);
CmmValue cmm_lambda_trace(void);
CmmValue cmm_lambda_log(CmmValue msg);
CmmValue cmm_sys_peak_rss(void);
CmmValue cmm_sys_os(void);
CmmValue cmm_sys_arch(void);
CmmValue cmm_sys_platform(void);
CmmValue cmm_zip_unzip(CmmValue, CmmValue);
CmmValue cmm_zip_build(CmmValue entries);
CmmValue cmm_lambda_create(CmmValue name, CmmValue role, CmmValue zip);
CmmValue cmm_lambda_update_code(CmmValue name, CmmValue zip);


#endif /* CMM_RUNTIME_H */
