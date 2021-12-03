#ifndef Py_INTERNAL_RUNTIME_H
#define Py_INTERNAL_RUNTIME_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "pycore_atomic.h"    /* _Py_atomic_address */
#include "pycore_gil.h"       // struct _gil_runtime_state
#include "pycore_interp.h"    // struct _is

#define _PY_NSMALLPOSINTS           257
#define _PY_NSMALLNEGINTS           5

// _PyLong_GetZero() and _PyLong_GetOne() must always be available
#if _PY_NSMALLPOSINTS < 2
#  error "_PY_NSMALLPOSINTS must be greater than 1"
#endif

/* ceval state */

struct _ceval_runtime_state {
    /* Request for checking signals. It is shared by all interpreters (see
       bpo-40513). Any thread of any interpreter can receive a signal, but only
       the main thread of the main interpreter can handle signals: see
       _Py_ThreadCanHandleSignals(). */
    _Py_atomic_int signals_pending;
#ifndef EXPERIMENTAL_ISOLATED_SUBINTERPRETERS
    struct _gil_runtime_state gil;
#endif
};

/* GIL state */

struct _gilstate_runtime_state {
    /* bpo-26558: Flag to disable PyGILState_Check().
       If set to non-zero, PyGILState_Check() always return 1. */
    int check_enabled;
    /* Assuming the current thread holds the GIL, this is the
       PyThreadState for the current thread. */
    _Py_atomic_address tstate_current;
    /* The single PyInterpreterState used by this process'
       GILState implementation
    */
    /* TODO: Given interp_main, it may be possible to kill this ref */
    PyInterpreterState *autoInterpreterState;
    Py_tss_t autoTSSkey;
};

/* Runtime audit hook state */

typedef struct _Py_AuditHookEntry {
    struct _Py_AuditHookEntry *next;
    Py_AuditHookFunction hookCFunction;
    void *userData;
} _Py_AuditHookEntry;

struct _Py_unicode_runtime_ids {
    PyThread_type_lock lock;
    // next_index value must be preserved when Py_Initialize()/Py_Finalize()
    // is called multiple times: see _PyUnicode_FromId() implementation.
    Py_ssize_t next_index;
};

/* Full Python runtime state */

typedef struct pyruntimestate {
    /* Is the struct data low-level initialized? */
    int _initialized;

    /* Is running Py_PreInitialize()? */
    int preinitializing;

    /* Is Python preinitialized? Set to 1 by Py_PreInitialize() */
    int preinitialized;

    /* Is Python core initialized? Set to 1 by _Py_InitializeCore() */
    int core_initialized;

    /* Is Python fully initialized? Set to 1 by Py_Initialize() */
    int initialized;

    /* Set by Py_FinalizeEx(). Only reset to NULL if Py_Initialize()
       is called again.

       Use _PyRuntimeState_GetFinalizing() and _PyRuntimeState_SetFinalizing()
       to access it, don't access it directly. */
    _Py_atomic_address _finalizing;

    struct pyinterpreters {
        PyThread_type_lock mutex;
        PyInterpreterState *head;
        PyInterpreterState *main;
        /* _next_interp_id is an auto-numbered sequence of small
           integers.  It gets initialized in _PyInterpreterState_Init(),
           which is called in Py_Initialize(), and used in
           PyInterpreterState_New().  A negative interpreter ID
           indicates an error occurred.  The main interpreter will
           always have an ID of 0.  Overflow results in a RuntimeError.
           If that becomes a problem later then we can adjust, e.g. by
           using a Python int. */
        int64_t next_id;
    } interpreters;
    // XXX Remove this field once we have a tp_* slot.
    struct _xidregistry {
        PyThread_type_lock mutex;
        struct _xidregitem *head;
    } xidregistry;

    unsigned long main_thread;

    /* Small integers are preallocated in this array so that they
     * can be shared.
     * The integers that are preallocated are those in the range
     *-_PY_NSMALLNEGINTS (inclusive) to _PY_NSMALLPOSINTS (not inclusive).
     */
    PyLongObject small_ints[_PY_NSMALLNEGINTS + _PY_NSMALLPOSINTS];

#define NEXITFUNCS 32
    void (*exitfuncs[NEXITFUNCS])(void);
    int nexitfuncs;

    struct _ceval_runtime_state ceval;
    struct _gilstate_runtime_state gilstate;

    PyPreConfig preconfig;

    // Audit values must be preserved when Py_Initialize()/Py_Finalize()
    // is called multiple times.
    Py_OpenCodeHookFunction open_code_hook;
    void *open_code_userdata;
    _Py_AuditHookEntry *audit_hook_head;

    struct _Py_unicode_runtime_ids unicode_ids;

    // XXX Consolidate globals found via the check-c-globals script.

    // This must be last.
    struct {
        struct _is interpreters_main;
    } _preallocated;
} _PyRuntimeState;

#define _PyRuntimeState_INIT \
    { \
        ._preallocated = { \
            .interpreters_main = _PyInterpreterState_INIT, \
        }, \
    }
/* Note: _PyRuntimeState_INIT sets other fields to 0/NULL */

static inline void
_PyRuntimeState_reset(_PyRuntimeState *runtime)
{
    /* Make it match _PyRuntimeState_INIT. */
    memset(runtime, 0, sizeof(_PyRuntimeState) - sizeof(runtime->_preallocated));
    memset(runtime, 0, (ssize_t)(&runtime->_preallocated) - (ssize_t)runtime);
}


PyAPI_DATA(_PyRuntimeState) _PyRuntime;

PyAPI_FUNC(PyStatus) _PyRuntimeState_Init(_PyRuntimeState *runtime);
PyAPI_FUNC(void) _PyRuntimeState_Fini(_PyRuntimeState *runtime);

#ifdef HAVE_FORK
extern PyStatus _PyRuntimeState_ReInitThreads(_PyRuntimeState *runtime);
#endif

/* Initialize _PyRuntimeState.
   Return NULL on success, or return an error message on failure. */
PyAPI_FUNC(PyStatus) _PyRuntime_Initialize(void);

PyAPI_FUNC(void) _PyRuntime_Finalize(void);


static inline PyThreadState*
_PyRuntimeState_GetFinalizing(_PyRuntimeState *runtime) {
    return (PyThreadState*)_Py_atomic_load_relaxed(&runtime->_finalizing);
}

static inline void
_PyRuntimeState_SetFinalizing(_PyRuntimeState *runtime, PyThreadState *tstate) {
    _Py_atomic_store_relaxed(&runtime->_finalizing, (uintptr_t)tstate);
}

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_RUNTIME_H */
