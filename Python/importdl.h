#ifndef Py_IMPORTDL_H
#define Py_IMPORTDL_H

#ifdef __cplusplus
extern "C" {
#endif


#ifdef HAVE_DYNAMIC_LOADING

extern const char *_PyImport_DynLoadFiletab[];

extern PyObject *_PyImport_LoadDynamicModuleWithSpec(
        PyObject *spec, PyObject *fullname, PyObject *path, FILE *);

/* Max length of module suffix searched for -- accommodates "module.slb" */
#define MAXSUFFIXSIZE 12

#ifdef MS_WINDOWS
#include <windows.h>
typedef FARPROC dl_funcptr;
#else
typedef void (*dl_funcptr)(void);
#endif

#endif /* HAVE_DYNAMIC_LOADING */


/* The following are also used for builtin modules. */

typedef PyObject *(*PyModInitFunction)(void);

#if defined(__EMSCRIPTEN__) && defined(PY_CALL_TRAMPOLINE)
extern PyObject *_PyImport_InitFunc_TrampolineCall(PyModInitFunction func);
#else
#define _PyImport_InitFunc_TrampolineCall(func) (func)()
#endif


#ifdef __cplusplus
}
#endif
#endif /* !Py_IMPORTDL_H */
