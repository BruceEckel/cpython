
/* Support for dynamic loading of extension modules */

#include "Python.h"
#include "pycore_call.h"
#include "pycore_import.h"
#include "pycore_pyerrors.h"      // _PyErr_FormatFromCause()
#include "pycore_pystate.h"
#include "pycore_runtime.h"

/* ./configure sets HAVE_DYNAMIC_LOADING if dynamic loading of modules is
   supported on this platform. configure will then compile and link in one
   of the dynload_*.c files, as appropriate. We will call a function in
   those modules to get a function pointer to the module's init function.
*/
#ifdef HAVE_DYNAMIC_LOADING

#include "pycore_importdl.h"

#ifdef MS_WINDOWS
extern dl_funcptr _PyImport_FindSharedFuncptrWindows(const char *prefix,
                                                     const char *shortname,
                                                     PyObject *pathname,
                                                     FILE *fp);
#else
extern dl_funcptr _PyImport_FindSharedFuncptr(const char *prefix,
                                              const char *shortname,
                                              const char *pathname, FILE *fp);
#endif

static const char * const ascii_only_prefix = "PyInit";
static const char * const nonascii_prefix = "PyInitU";

/* Get the variable part of a module's export symbol name.
 * Returns a bytes instance. For non-ASCII-named modules, the name is
 * encoded as per PEP 489.
 * The hook_prefix pointer is set to either ascii_only_prefix or
 * nonascii_prefix, as appropriate.
 */
static PyObject *
get_encoded_name(PyObject *name, const char **hook_prefix) {
    PyObject *tmp;
    PyObject *encoded = NULL;
    PyObject *modname = NULL;
    Py_ssize_t name_len, lastdot;

    /* Get the short name (substring after last dot) */
    name_len = PyUnicode_GetLength(name);
    if (name_len < 0) {
        return NULL;
    }
    lastdot = PyUnicode_FindChar(name, '.', 0, name_len, -1);
    if (lastdot < -1) {
        return NULL;
    } else if (lastdot >= 0) {
        tmp = PyUnicode_Substring(name, lastdot + 1, name_len);
        if (tmp == NULL)
            return NULL;
        name = tmp;
        /* "name" now holds a new reference to the substring */
    } else {
        Py_INCREF(name);
    }

    /* Encode to ASCII or Punycode, as needed */
    encoded = PyUnicode_AsEncodedString(name, "ascii", NULL);
    if (encoded != NULL) {
        *hook_prefix = ascii_only_prefix;
    } else {
        if (PyErr_ExceptionMatches(PyExc_UnicodeEncodeError)) {
            PyErr_Clear();
            encoded = PyUnicode_AsEncodedString(name, "punycode", NULL);
            if (encoded == NULL) {
                goto error;
            }
            *hook_prefix = nonascii_prefix;
        } else {
            goto error;
        }
    }

    /* Replace '-' by '_' */
    modname = _PyObject_CallMethod(encoded, &_Py_ID(replace), "cc", '-', '_');
    if (modname == NULL)
        goto error;

    Py_DECREF(name);
    Py_DECREF(encoded);
    return modname;
error:
    Py_DECREF(name);
    Py_XDECREF(encoded);
    return NULL;
}

void
_Py_ext_module_loader_info_clear(struct _Py_ext_module_loader_info *info)
{
    Py_CLEAR(info->path);
#ifndef MS_WINDOWS
    Py_CLEAR(info->path_encoded);
#endif
    Py_CLEAR(info->name);
    Py_CLEAR(info->name_encoded);
}

int
_Py_ext_module_loader_info_init_from_spec(
                            struct _Py_ext_module_loader_info *p_info,
                            PyObject *spec)
{
    struct _Py_ext_module_loader_info info = {0};

    info.name = PyObject_GetAttrString(spec, "name");
    if (info.name == NULL) {
        return -1;
    }
    if (!PyUnicode_Check(info.name)) {
        PyErr_SetString(PyExc_TypeError,
                        "spec.name must be a string");
        _Py_ext_module_loader_info_clear(&info);
        return -1;
    }

    info.name_encoded = get_encoded_name(info.name, &info.hook_prefix);
    if (info.name_encoded == NULL) {
        _Py_ext_module_loader_info_clear(&info);
        return -1;
    }

    info.newcontext = PyUnicode_AsUTF8(info.name);
    if (info.newcontext == NULL) {
        _Py_ext_module_loader_info_clear(&info);
        return -1;
    }

    info.path = PyObject_GetAttrString(spec, "origin");
    if (info.path == NULL) {
        _Py_ext_module_loader_info_clear(&info);
        return -1;
    }

#ifndef MS_WINDOWS
    info.path_encoded = PyUnicode_EncodeFSDefault(info.path);
    if (info.path_encoded == NULL) {
        _Py_ext_module_loader_info_clear(&info);
        return -1;
    }
#endif

    *p_info = info;
    return 0;
}

static void
_Py_ext_module_loader_result_apply_error(
                            struct _Py_ext_module_loader_result *res)
{
    if (res->err != NULL) {
        if (PyErr_Occurred()) {
            _PyErr_FormatFromCause(PyExc_SystemError, res->err);
        }
        else {
            PyErr_SetString(PyExc_SystemError, res->err);
        }
    }
    else {
        assert(PyErr_Occurred());
    }
}

static PyModInitFunction
_PyImport_GetModInitFunc(struct _Py_ext_module_loader_info *info,
                         FILE *fp)
{
    const char *name_buf = PyBytes_AS_STRING(info->name_encoded);
    dl_funcptr exportfunc;
#ifdef MS_WINDOWS
    exportfunc = _PyImport_FindSharedFuncptrWindows(
                    info->hook_prefix, name_buf, info->path, fp);
#else
    {
        const char *path_buf = PyBytes_AS_STRING(info->path_encoded);
        exportfunc = _PyImport_FindSharedFuncptr(
                        info->hook_prefix, name_buf, path_buf, fp);
    }
#endif

    if (exportfunc == NULL) {
        if (!PyErr_Occurred()) {
            PyObject *msg;
            msg = PyUnicode_FromFormat(
                "dynamic module does not define "
                "module export function (%s_%s)",
                info->hook_prefix, name_buf);
            if (msg != NULL) {
                PyErr_SetImportError(msg, info->name, info->path);
               Py_DECREF(msg);
            }
        }
        return NULL;
    }

    return (PyModInitFunction)exportfunc;
}

static int
_PyImport_RunModInitFunc(PyModInitFunction p0,
                         struct _Py_ext_module_loader_info *info,
                         struct _Py_ext_module_loader_result *p_res)
{
    struct _Py_ext_module_loader_result res = {
        .singlephase=-1,
    };
    const char *name_buf = PyBytes_AS_STRING(info->name_encoded);

    /* Package context is needed for single-phase init */
    const char *oldcontext = _PyImport_SwapPackageContext(info->newcontext);
    PyObject *m = p0();
    _PyImport_SwapPackageContext(oldcontext);

#ifdef NDEBUG
#  define SET_ERROR(...) \
    (void)snprintf(res.err, Py_ARRAY_LENGTH(res.err),  __VA_ARGS__)
#else
#  define SET_ERROR(...) \
    do { \
        int n = snprintf(res.err, Py_ARRAY_LENGTH(res.err),  __VA_ARGS__); \
        assert(n < Py_ARRAY_LENGTH(res.err)); \
    } while (0)
#endif

    if (m == NULL) {
        if (!PyErr_Occurred()) {
            SET_ERROR(
                "initialization of %s failed without raising an exception",
                name_buf);
        }
        goto error;
    } else if (PyErr_Occurred()) {
        SET_ERROR("initialization of %s raised unreported exception",
                  name_buf);
        /* We would probably be correct to decref m here,
         * but we weren't doing so before,
         * so we stick with doing nothing. */
        m = NULL;
        goto error;
    }

    if (Py_IS_TYPE(m, NULL)) {
        /* This can happen when a PyModuleDef is returned without calling
         * PyModuleDef_Init on it
         */
        SET_ERROR("init function of %s returned uninitialized object",
                  name_buf);
        /* Likewise, decref'ing here makes sense.  However, the original
         * code has a note about "prevent segfault in DECREF",
         * so we play it safe and leave it alone. */
        m = NULL; /* prevent segfault in DECREF */
        goto error;
    }

    if (PyObject_TypeCheck(m, &PyModuleDef_Type)) {
        /* multi-phase init */
        res.singlephase = 0;
        res.def = (PyModuleDef *)m;
    }
    else {
        /* single-phase init (legacy) */
        res.singlephase = 1;
        res.module = m;

        /* Remember pointer to module init function. */
        res.def = PyModule_GetDef(m);
        if (res.def == NULL) {
            SET_ERROR("initialization of %s did not return an extension "
                      "module", name_buf);
            goto error;
        }
        res.def->m_base.m_init = p0;

        if (info->hook_prefix == nonascii_prefix) {
            /* don't allow legacy init for non-ASCII module names */
            SET_ERROR("initialization of %s did not return PyModuleDef",
                      name_buf);
            goto error;
        }
    }
#undef SET_ERROR

    *p_res = res;
    return 0;

error:
    Py_CLEAR(res.module);
    res.def = NULL;
    *p_res = res;
    return -1;
}

int
_PyImport_RunDynamicModule(struct _Py_ext_module_loader_info *info,
                           FILE *fp,
                           struct _Py_ext_module_loader_result *res)
{
    PyModInitFunction p0 = _PyImport_GetModInitFunc(info, fp);
    if (p0 == NULL) {
        return -1;
    }

    if (_PyImport_RunModInitFunc(p0, info, res) < 0) {
        _Py_ext_module_loader_result_apply_error(res);
        return -1;
    }

    if (res->singlephase) {
        /* Remember the filename as the __file__ attribute */
        if (PyModule_AddObjectRef(res->module, "__file__", info->path) < 0) {
            PyErr_Clear(); /* Not important enough to report */
        }

        /* Run _PyImport_FixupExtensionObject() to finish loading the module. */
    }
    /* else: Run PyModule_FromDefAndSpec() to finish loading the module. */

    return 0;
}

#endif /* HAVE_DYNAMIC_LOADING */
