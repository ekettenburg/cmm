"""Type model + native class signatures, shared by analyzer and codegen."""


class Type:
    __slots__ = ("name", "args")

    def __init__(self, name, args=()):
        self.name = name          # Int Float Bool String Data List Dict Job
                                  # Socket Void Namespace <ClassName>
        self.args = list(args)

    def __eq__(self, other):
        return (isinstance(other, Type) and self.name == other.name
                and self.args == other.args)

    def __hash__(self):
        return hash((self.name, tuple(self.args)))

    def __repr__(self):
        if self.args:
            return f"{self.name}[{', '.join(map(repr, self.args))}]"
        return self.name

    # convenience predicates
    def is_numeric(self):
        return self.name in ("Int", "Float")


INT = Type("Int")
FLOAT = Type("Float")
BOOL = Type("Bool")
STRING = Type("String")
DATA = Type("Data")
VOID = Type("Void")
SOCKET = Type("Socket")


def list_of(t):
    return Type("List", [t])


def dict_of(k, v):
    return Type("Dict", [k, v])


def job_of(t):
    return Type("Job", [t])


def namespace(name):
    return Type("Namespace", [Type(name)])


PRIMITIVES = {"Int", "Float", "Bool", "String", "Data"}

# Native "namespace" classes used as  Name.method(...)
NATIVE_NAMESPACES = {"Console", "Math", "File", "Date", "Json", "Socket", "Http", "Sys", "Lambda", "Zip", "Crypto", "Base64", "Mysql", "Preg"}

# All native class names (for `use` validation; these need no file).
NATIVE_CLASSES = {
    "String", "Int", "Float", "Bool", "List", "Dict", "Data", "Json",
    "File", "Socket", "Console", "Date", "Math", "Job", "Http", "Sys", "Lambda", "Zip", "Crypto", "Base64", "Mysql", "Preg",
}


# --- Namespace method signatures --------------------------------------------
# key: (Namespace, method) -> (c_function, return_type, arity_or_None)
# return_type may be a Type or the string 'arg0'/'voidish'. None arity = any.
NAMESPACE_METHODS = {
    ("Console", "print"):   ("cmm_console_print", VOID, None),
    ("Console", "println"): ("cmm_console_println", VOID, None),
    ("Console", "read"):    ("cmm_console_read", STRING, 0),

    ("Math", "sqrt"):   ("cmm_math_sqrt", FLOAT, 1),
    ("Math", "abs"):    ("cmm_math_abs", FLOAT, 1),
    ("Math", "pow"):    ("cmm_math_pow", FLOAT, 2),
    ("Math", "floor"):  ("cmm_math_floor", INT, 1),
    ("Math", "ceil"):   ("cmm_math_ceil", INT, 1),
    ("Math", "min"):    ("cmm_math_min", FLOAT, 2),
    ("Math", "max"):    ("cmm_math_max", FLOAT, 2),
    ("Math", "random"): ("cmm_math_random", FLOAT, 0),
    ("Math", "pi"):     ("cmm_math_pi", FLOAT, 0),

    ("File", "read"):   ("cmm_file_read", STRING, 1),
    ("File", "write"):  ("cmm_file_write", BOOL, 2),
    ("File", "append"): ("cmm_file_append", BOOL, 2),
    ("File", "exists"): ("cmm_file_exists", BOOL, 1),
    ("File", "delete"): ("cmm_file_delete", BOOL, 1),

    ("Date", "now"):    ("cmm_date_now", INT, 0),
    ("Date", "format"): ("cmm_date_format", STRING, 2),
    ("Date", "amzDate"): ("cmm_date_amz", STRING, 0),
    ("Date", "date"): ("cmm_date_date", STRING, 2),
    ("Date", "gmdate"): ("cmm_date_gmdate", STRING, 2),

    ("Json", "encode"): ("cmm_json_encode", STRING, 1),
    ("Json", "decode"): ("cmm_json_decode", DATA, 1),
    ("Json", "pretty"): ("cmm_json_pretty", STRING, 1),
    ("Json", "parse"):  ("cmm_json_decode", DATA, 1),
    ("Json", "stringify"): ("cmm_json_encode", STRING, 1),

    ("Socket", "connect"): ("cmm_socket_connect", SOCKET, 2),

    ("Http", "get"):  ("cmm_http_get", STRING, 1),
    ("Http", "post"): ("cmm_http_post", STRING, 2),
    ("Http", "request"): ("cmm_http_request", STRING, 4),

    ("Sys", "exit"):  ("cmm_sys_exit", VOID, 1),
    ("Sys", "exec"):  ("cmm_sys_run", STRING, 1),
    ("Sys", "shell"): ("cmm_sys_shell", INT, 1),
    ("Sys", "env"):   ("cmm_sys_env", STRING, 1),
    ("Sys", "args"):  ("cmm_sys_args", list_of(STRING), 0),
    ("Sys", "cwd"):   ("cmm_sys_cwd", STRING, 0),
    ("Sys", "chdir"): ("cmm_sys_chdir", BOOL, 1),
    ("Sys", "peakRss"): ("cmm_sys_peak_rss", INT, 0),
    ("Lambda", "next"):       ("cmm_lambda_next", STRING, 0),
    ("Lambda", "success"):    ("cmm_lambda_success", BOOL, 1),
    ("Lambda", "failure"):    ("cmm_lambda_failure", BOOL, 2),
    ("Lambda", "initError"):  ("cmm_lambda_init_error", BOOL, 2),
    ("Lambda", "requestId"):  ("cmm_lambda_request_id", STRING, 0),
    ("Lambda", "deadlineMs"): ("cmm_lambda_deadline", INT, 0),
    ("Lambda", "invokedArn"): ("cmm_lambda_arn", STRING, 0),
    ("Lambda", "traceId"):    ("cmm_lambda_trace", STRING, 0),
    ("Lambda", "log"):        ("cmm_lambda_log", VOID, 1),
    ("Zip", "build"): ("cmm_zip_build", STRING, 1),
    ("Zip", "unzip"): ("cmm_zip_unzip", INT, 2),
    ("Http", "send"): ("cmm_http_send", DATA, 4),
    ("Crypto", "sha256Hex"): ("cmm_crypto_sha256hex", STRING, 1),
    ("Crypto", "sha1Hex"): ("cmm_crypto_sha1hex", STRING, 1),
    ("Crypto", "hmacSha256"): ("cmm_crypto_hmac_sha256", STRING, 2),
    ("Crypto", "hmacSha256Hex"): ("cmm_crypto_hmac_sha256_hex", STRING, 2),
    ("Crypto", "hex"): ("cmm_crypto_hex", STRING, 1),
    ("Crypto", "randomHex"): ("cmm_crypto_random_hex", STRING, 1),
    ("Base64", "encode"): ("cmm_base64_encode", STRING, 1),
    ("Base64", "decode"): ("cmm_base64_decode", STRING, 1),
    ("Mysql", "connect"): ("cmm_mysql_connect", INT, 5),
    ("Mysql", "query"): ("cmm_mysql_query", list_of(DATA), 2),
    ("Mysql", "exec"): ("cmm_mysql_exec", INT, 2),
    ("Mysql", "insertId"): ("cmm_mysql_insert_id", INT, 1),
    ("Mysql", "affected"): ("cmm_mysql_affected", INT, 1),
    ("Mysql", "error"): ("cmm_mysql_error", STRING, 1),
    ("Mysql", "close"): ("cmm_mysql_close", BOOL, 1),
    ("Preg", "match"): ("cmm_preg_match", list_of(STRING), 2),
    ("Preg", "test"): ("cmm_preg_test", BOOL, 2),
    ("Preg", "matchAll"): ("cmm_preg_match_all", list_of(STRING), 2),
    ("Preg", "replace"): ("cmm_preg_replace", STRING, 3),
    ("Preg", "split"): ("cmm_preg_split", list_of(STRING), 2),
    ("Preg", "quote"): ("cmm_preg_quote", STRING, 1),
    ("Sys", "os"):       ("cmm_sys_os", STRING, 0),
    ("Sys", "arch"):     ("cmm_sys_arch", STRING, 0),
    ("Sys", "platform"): ("cmm_sys_platform", STRING, 0),
    ("Lambda", "create"):     ("cmm_lambda_create", STRING, 3),
    ("Lambda", "updateCode"): ("cmm_lambda_update_code", STRING, 2),
}


# --- Value-type method signatures -------------------------------------------
# Keyed by the receiver's base type name. Each entry:
#   method -> (c_function, return_type_resolver, arity)
# return_type_resolver: a Type, or a callable(recv_type)->Type for generics.

def _elem(recv):       # List[T] -> T
    return recv.args[0] if recv.args else DATA


def _val(recv):        # Dict[K,V] -> V
    return recv.args[1] if len(recv.args) > 1 else DATA


VALUE_METHODS = {
    "String": {
        "length":    ("cmm_string_length", INT, 0),
        "substring": ("cmm_string_substring", STRING, 2),
        "indexOf":   ("cmm_string_indexof", INT, 1),
        "contains":  ("cmm_string_contains", BOOL, 1),
        "startsWith":("cmm_string_startswith", BOOL, 1),
        "endsWith":  ("cmm_string_endswith", BOOL, 1),
        "upper":     ("cmm_string_upper", STRING, 0),
        "lower":     ("cmm_string_lower", STRING, 0),
        "trim":      ("cmm_string_trim", STRING, 0),
        "split":     ("cmm_string_split", list_of(STRING), 1),
        "replace":   ("cmm_string_replace", STRING, 2),
        "toInt":     ("cmm_string_toint", INT, 0),
        "toFloat":   ("cmm_string_tofloat", FLOAT, 0),
        "toStr":     ("cmm_identity", STRING, 0),
    },
    "List": {
        "add":      ("cmm_list_add", VOID, 1),
        "remove":   ("cmm_list_remove", VOID, 1),
        "get":      ("cmm_list_get", _elem, 1),
        "set":      ("cmm_list_set", VOID, 2),
        "length":   ("cmm_list_length", INT, 0),
        "contains": ("cmm_list_contains", BOOL, 1),
        "clear":    ("cmm_list_clear", VOID, 0),
    },
    "Dict": {
        "get":    ("cmm_dict_get", _val, 1),
        "set":    ("cmm_dict_set", VOID, 2),
        "has":    ("cmm_dict_has", BOOL, 1),
        "remove": ("cmm_dict_remove", VOID, 1),
        "keys":   ("cmm_dict_keys", list_of(STRING), 0),
        "length": ("cmm_dict_length", INT, 0),
    },
    "Data": {
        "get":    ("cmm_data_get", DATA, 1),
        "set":    ("cmm_data_set", VOID, 2),
        "has":    ("cmm_data_has", BOOL, 1),
        "length": ("cmm_data_length", INT, 0),
        "keys":   ("cmm_data_keys", list_of(STRING), 0),
        "type":     ("cmm_data_type", STRING, 0),
        "at":       ("cmm_data_at", DATA, 1),
        "path":     ("cmm_data_path", DATA, 1),
        "getStr":   ("cmm_data_get_str", STRING, 1),
        "getInt":   ("cmm_data_get_int", INT, 1),
        "getFloat": ("cmm_data_get_float", FLOAT, 1),
        "getBool":  ("cmm_data_get_bool", BOOL, 1),
        "isObject": ("cmm_data_is_object", BOOL, 0),
        "isArray":  ("cmm_data_is_array", BOOL, 0),
        "isNull":   ("cmm_data_is_null", BOOL, 0),
        "add":      ("cmm_list_add", VOID, 1),
    },
    "Int":   {"toStr": ("cmm_int_tostr", STRING, 0),
              "toFloat": ("cmm_int_tofloat", FLOAT, 0)},
    "Float": {"toStr": ("cmm_float_tostr", STRING, 0),
              "toInt": ("cmm_float_toint", INT, 0)},
    "Bool":  {"toStr": ("cmm_bool_tostr", STRING, 0)},
    "Socket": {
        "write":   ("cmm_socket_write", BOOL, 1),
        "read":    ("cmm_socket_read", STRING, 1),
        "readAll": ("cmm_socket_readall", STRING, 0),
        "close":   ("cmm_socket_close", BOOL, 0),
    },
}


def type_from_ref(ref):
    """Convert an ast.TypeRef to a Type."""
    args = [type_from_ref(a) for a in ref.args]
    return Type(ref.name, args)
