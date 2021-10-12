/* Return the initial module search path. */

#include "Python.h"
#include "pycore_fileutils.h"
#include "pycore_initconfig.h"
#include "pycore_pathconfig.h"
#include "osdefs.h"               // DELIM

#include <sys/types.h>
#include <string.h>
#include <stdbool.h>

#ifdef __APPLE__
#  include <mach-o/dyld.h>
#endif

/* Search in some common locations for the associated Python libraries.
 *
 * Two directories must be found, the platform independent directory
 * (prefix), containing the common .py and .pyc files, and the platform
 * dependent directory (exec_prefix), containing the shared library
 * modules.  Note that prefix and exec_prefix can be the same directory,
 * but for some installations, they are different.
 *
 * Py_GetPath() carries out separate searches for prefix and exec_prefix.
 * Each search tries a number of different locations until a ``landmark''
 * file or directory is found.  If no prefix or exec_prefix is found, a
 * warning message is issued and the preprocessor defined PREFIX and
 * EXEC_PREFIX are used (even though they will not work); python carries on
 * as best as is possible, but most imports will fail.
 *
 * Before any searches are done, the location of the executable is
 * determined.  If argv[0] has one or more slashes in it, it is used
 * unchanged.  Otherwise, it must have been invoked from the shell's path,
 * so we search $PATH for the named executable and use that.  If the
 * executable was not found on $PATH (or there was no $PATH environment
 * variable), the original argv[0] string is used.
 *
 * Next, the executable location is examined to see if it is a symbolic
 * link.  If so, the link is chased (correctly interpreting a relative
 * pathname if one is found) and the directory of the link target is used.
 *
 * Finally, argv0_dir is set to the directory containing the executable
 * (i.e. the last component is stripped).
 *
 * With argv0_dir in hand, we perform a number of steps.  The same steps
 * are performed for prefix and for exec_prefix, but with a different
 * landmark.
 *
 * Step 1. Are we running python out of the build directory?  This is
 * checked by looking for a different kind of landmark relative to
 * argv0_dir.  For prefix, the landmark's path is derived from the VPATH
 * preprocessor variable (taking into account that its value is almost, but
 * not quite, what we need).  For exec_prefix, the landmark is
 * pybuilddir.txt.  If the landmark is found, we're done.
 *
 * For the remaining steps, the prefix landmark will always be
 * lib/python$VERSION/os.py and the exec_prefix will always be
 * lib/python$VERSION/lib-dynload, where $VERSION is Python's version
 * number as supplied by the Makefile.  Note that this means that no more
 * build directory checking is performed; if the first step did not find
 * the landmarks, the assumption is that python is running from an
 * installed setup.
 *
 * Step 2. See if the $PYTHONHOME environment variable points to the
 * installed location of the Python libraries.  If $PYTHONHOME is set, then
 * it points to prefix and exec_prefix.  $PYTHONHOME can be a single
 * directory, which is used for both, or the prefix and exec_prefix
 * directories separated by a colon.
 *
 * Step 3. Try to find prefix and exec_prefix relative to argv0_dir,
 * backtracking up the path until it is exhausted.  This is the most common
 * step to succeed.  Note that if prefix and exec_prefix are different,
 * exec_prefix is more likely to be found; however if exec_prefix is a
 * subdirectory of prefix, both will be found.
 *
 * Step 4. Search the directories pointed to by the preprocessor variables
 * PREFIX and EXEC_PREFIX.  These are supplied by the Makefile but can be
 * passed in as options to the configure script.
 *
 * That's it!
 *
 * Well, almost.  Once we have determined prefix and exec_prefix, the
 * preprocessor variable PYTHONPATH is used to construct a path.  Each
 * relative path on PYTHONPATH is prefixed with prefix.  Then the directory
 * containing the shared library modules is appended.  The environment
 * variable $PYTHONPATH is inserted in front of it all.  Finally, the
 * prefix and exec_prefix globals are tweaked so they reflect the values
 * expected by other code, by stripping the "lib/python$VERSION/..." stuff
 * off.  If either points to the build directory, the globals are reset to
 * the corresponding preprocessor variables (so sys.prefix will reflect the
 * installation location, even though sys.path points into the build
 * directory).  This seems to make more sense given that currently the only
 * known use of sys.prefix and sys.exec_prefix is for the ILU installation
 * process to find the installed Python tree.
 *
 * An embedding application can use Py_SetPath() to override all of
 * these automatic path computations.
 *
 * NOTE: Windows MSVC builds use PC/getpathp.c instead!
 */

#ifdef __cplusplus
extern "C" {
#endif


#if (!defined(PREFIX) || !defined(EXEC_PREFIX) \
        || !defined(VERSION) || !defined(VPATH))
#error "PREFIX, EXEC_PREFIX, VERSION and VPATH macros must be defined"
#endif

#ifndef LANDMARK
#define LANDMARK L"os.py"
#endif

#define BUILD_LANDMARK L"Modules/Setup.local"

#define PATHLEN_ERR() _PyStatus_ERR("path configuration: path too long")

#define LOCATION_FOUND (LOCATION_EXISTS | LOCATION_FORCED)

typedef struct {
    wchar_t *path_env;                 /* PATH environment variable */

    wchar_t *pythonpath_macro;         /* PYTHONPATH macro */
    wchar_t *prefix_macro;             /* PREFIX macro */
    wchar_t *exec_prefix_macro;        /* EXEC_PREFIX macro */
    wchar_t *vpath_macro;              /* VPATH macro */

    wchar_t *lib_python;               /* <platlibdir> / "pythonX.Y" */

    int warnings;
    const wchar_t *pythonpath_env;
    const wchar_t *platlibdir;

    wchar_t *argv0_dir;
    int argv0_dir_verified;     /* bit vector of verified LOCATION_* flags */

    wchar_t *stdlib_dir;
    int stdlib_dir_verified;    /* bit vector of verified LOCATION_* flags */

    wchar_t *prefix;
    int prefix_verified;        /* bit vector of verified LOCATION_* flags */
    bool prefix_found;          /* found platform independent libraries? */

    wchar_t *extensions;        /* path under exec_prefix where stdlib ext modules are found */
    int extensions_verified;    /* bit vector of verified LOCATION_* flags */

    wchar_t *exec_prefix;
    int exec_prefix_verified;   /* bit vector of verified LOCATION_* flags */
    bool exec_prefix_found;     /* found the platform dependent libraries? */

    wchar_t *zip_path;
} PyCalculatePath;

static const wchar_t delimiter[2] = {DELIM, '\0'};
static const wchar_t separator[2] = {SEP, '\0'};


static void
reduce(wchar_t *dir)
{
    size_t i = wcslen(dir);
    while (i > 0 && dir[i] != SEP) {
        --i;
    }
    dir[i] = '\0';
}


/* Is file, not directory */
static int
isfile(const wchar_t *filename)
{
    struct stat buf;
    if (_Py_wstat(filename, &buf) != 0) {
        return 0;
    }
    if (!S_ISREG(buf.st_mode)) {
        return 0;
    }
    return 1;
}


/* Is executable file */
static int
isxfile(const wchar_t *filename)
{
    struct stat buf;
    if (_Py_wstat(filename, &buf) != 0) {
        return 0;
    }
    if (!S_ISREG(buf.st_mode)) {
        return 0;
    }
    if ((buf.st_mode & 0111) == 0) {
        return 0;
    }
    return 1;
}


/* Is directory */
static int
isdir(const wchar_t *filename)
{
    struct stat buf;
    if (_Py_wstat(filename, &buf) != 0) {
        return 0;
    }
    if (!S_ISDIR(buf.st_mode)) {
        return 0;
    }
    return 1;
}


/* Add a path component, by appending stuff to buffer.
   buflen: 'buffer' length in characters including trailing NUL.

   If path2 is empty:

   - if path doesn't end with SEP and is not empty, add SEP to path
   - otherwise, do nothing. */
static PyStatus
joinpath(wchar_t *path, const wchar_t *path2, size_t path_len)
{
    if (_Py_isabs(path2)) {
        if (wcslen(path2) >= path_len) {
            return PATHLEN_ERR();
        }
        wcscpy(path, path2);
    }
    else {
        if (_Py_add_relfile(path, path2, path_len) < 0) {
            return PATHLEN_ERR();
        }
        return _PyStatus_OK();
    }
    return _PyStatus_OK();
}


static wchar_t*
substring(const wchar_t *str, size_t len)
{
    wchar_t *substr = PyMem_RawMalloc((len + 1) * sizeof(wchar_t));
    if (substr == NULL) {
        return NULL;
    }

    if (len) {
        memcpy(substr, str, len * sizeof(wchar_t));
    }
    substr[len] = L'\0';
    return substr;
}


static wchar_t*
joinpath2(const wchar_t *path, const wchar_t *path2)
{
    if (_Py_isabs(path2)) {
        return _PyMem_RawWcsdup(path2);
    }
    return _Py_join_relfile(path, path2);
}


static inline int
safe_wcscpy(wchar_t *dst, const wchar_t *src, size_t n)
{
    size_t srclen = wcslen(src);
    if (n <= srclen) {
        dst[0] = L'\0';
        return -1;
    }
    memcpy(dst, src, (srclen + 1) * sizeof(wchar_t));
    return 0;
}


/* copy_absolute requires that path be allocated at least
   'abs_path_len' characters (including trailing NUL). */
static PyStatus
copy_absolute(wchar_t *abs_path, const wchar_t *path, size_t abs_path_len)
{
    if (_Py_isabs(path)) {
        if (safe_wcscpy(abs_path, path, abs_path_len) < 0) {
            return PATHLEN_ERR();
        }
    }
    else {
        if (!_Py_wgetcwd(abs_path, abs_path_len)) {
            /* unable to get the current directory */
            if (safe_wcscpy(abs_path, path, abs_path_len) < 0) {
                return PATHLEN_ERR();
            }
            return _PyStatus_OK();
        }
        if (path[0] == '.' && path[1] == SEP) {
            path += 2;
        }
        PyStatus status = joinpath(abs_path, path, abs_path_len);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
    }
    return _PyStatus_OK();
}


/* path_len: path length in characters including trailing NUL */
static PyStatus
absolutize(wchar_t **path_p)
{
    assert(!_Py_isabs(*path_p));

    wchar_t abs_path[MAXPATHLEN+1];
    wchar_t *path = *path_p;

    PyStatus status = copy_absolute(abs_path, path, Py_ARRAY_LENGTH(abs_path));
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }

    PyMem_RawFree(*path_p);
    *path_p = _PyMem_RawWcsdup(abs_path);
    if (*path_p == NULL) {
        return _PyStatus_NO_MEMORY();
    }
    return _PyStatus_OK();
}


/* Is module -- check for .pyc too */
static PyStatus
ismodule(const wchar_t *path, int *result)
{
    wchar_t *filename = joinpath2(path, LANDMARK);
    if (filename == NULL) {
        return _PyStatus_NO_MEMORY();
    }

    if (isfile(filename)) {
        PyMem_RawFree(filename);
        *result = 1;
        return _PyStatus_OK();
    }

    /* Check for the compiled version of prefix. */
    size_t len = wcslen(filename);
    wchar_t *pyc = PyMem_RawMalloc((len + 2) * sizeof(wchar_t));
    if (pyc == NULL) {
        PyMem_RawFree(filename);
        return _PyStatus_NO_MEMORY();
    }

    memcpy(pyc, filename, len * sizeof(wchar_t));
    pyc[len] = L'c';
    pyc[len + 1] = L'\0';
    *result = isfile(pyc);

    PyMem_RawFree(filename);
    PyMem_RawFree(pyc);

    return _PyStatus_OK();
}


#if defined(__CYGWIN__) || defined(__MINGW32__)
#ifndef EXE_SUFFIX
#define EXE_SUFFIX L".exe"
#endif

/* pathlen: 'path' length in characters including trailing NUL */
static PyStatus
add_exe_suffix(wchar_t **progpath_p)
{
    wchar_t *progpath = *progpath_p;

    /* Check for already have an executable suffix */
    size_t n = wcslen(progpath);
    size_t s = wcslen(EXE_SUFFIX);
    if (wcsncasecmp(EXE_SUFFIX, progpath + n - s, s) == 0) {
        return _PyStatus_OK();
    }

    wchar_t *progpath2 = PyMem_RawMalloc((n + s + 1) * sizeof(wchar_t));
    if (progpath2 == NULL) {
        return _PyStatus_NO_MEMORY();
    }

    memcpy(progpath2, progpath, n * sizeof(wchar_t));
    memcpy(progpath2 + n, EXE_SUFFIX, s * sizeof(wchar_t));
    progpath2[n+s] = L'\0';

    if (isxfile(progpath2)) {
        PyMem_RawFree(*progpath_p);
        *progpath_p = progpath2;
    }
    else {
        PyMem_RawFree(progpath2);
    }
    return _PyStatus_OK();
}
#endif


/* search_for_prefix requires that argv0_dir be no more than MAXPATHLEN
   bytes long.
*/
static PyStatus
search_for_stdlib_dir(PyCalculatePath *calculate,
                      wchar_t *stdlib, size_t stdlib_len, int *verified)
{
    PyStatus status;

    /* Check to see if argv0_dir is in the build directory

       Path: <argv0_dir> / <BUILD_LANDMARK define> */
    wchar_t *path = joinpath2(calculate->argv0_dir, BUILD_LANDMARK);
    if (path == NULL) {
        return _PyStatus_NO_MEMORY();
    }

    int is_build_dir = isfile(path);
    PyMem_RawFree(path);

    if (is_build_dir) {
        /* argv0_dir is the build directory (BUILD_LANDMARK exists),
           now also check LANDMARK using ismodule(). */

        /* Path: <argv0_dir> / <VPATH macro> / Lib */
        /* or if VPATH is empty: <argv0_dir> / Lib */
        if (safe_wcscpy(stdlib, calculate->argv0_dir, stdlib_len) < 0) {
            return PATHLEN_ERR();
        }

        status = joinpath(stdlib, calculate->vpath_macro, stdlib_len);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }

        status = joinpath(stdlib, L"Lib", stdlib_len);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }

        int module;
        status = ismodule(stdlib, &module);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
        if (module) {
            /* BUILD_LANDMARK and LANDMARK found */
            *verified |= LOCATION_EXISTS |
                         LOCATION_NEAR_ARGV0 | LOCATION_WITH_FILE |
                         LOCATION_IN_SOURCE_TREE | LOCATION_IN_BUILD_DIR;
            return _PyStatus_OK();
        }
    }

    /* Search from argv0_dir, until root is found */
    status = copy_absolute(stdlib, calculate->argv0_dir, stdlib_len);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }

    int flag = LOCATION_NEAR_ARGV0 | LOCATION_WITH_FILE;
    do {
        /* Path: <argv0_dir or substring> / <lib_python> / LANDMARK */
        size_t n = wcslen(stdlib);
        status = joinpath(stdlib, calculate->lib_python, stdlib_len);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }

        int module;
        status = ismodule(stdlib, &module);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
        if (module) {
            *verified |= LOCATION_EXISTS | flag;
            return _PyStatus_OK();
        }
        stdlib[n] = L'\0';
        reduce(stdlib);
        flag = LOCATION_NEAR_ARGV0;
    } while (stdlib[0]);

    /* Look at configure's PREFIX.
       Path: <PREFIX macro> / <lib_python> / LANDMARK */
    if (safe_wcscpy(stdlib, calculate->prefix_macro, stdlib_len) < 0) {
        return PATHLEN_ERR();
    }
    status = joinpath(stdlib, calculate->lib_python, stdlib_len);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }

    int module;
    status = ismodule(stdlib, &module);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }
    if (module) {
        *verified |= LOCATION_EXISTS | LOCATION_PREFIX;
        return _PyStatus_OK();
    }

    /* Fail */
    return _PyStatus_OK();
}

static PyStatus
calculate_stdlib_dir(PyCalculatePath *calculate, _PyPathConfig *pathconfig)
{
    assert(calculate->stdlib_dir == NULL);
    assert(calculate->stdlib_dir_verified == LOCATION_UNKNOWN);

    PyStatus status;
    wchar_t stdlib[MAXPATHLEN+1];
    memset(stdlib, 0, sizeof(stdlib));
    size_t stdlib_len = Py_ARRAY_LENGTH(stdlib);
    const wchar_t *prefix = NULL;
    int verified = LOCATION_UNKNOWN;

    /* If PYTHONHOME is set, we believe it unconditionally. */
    if (pathconfig->home != NULL) {
        /* Path: <home> / <lib_python> */
        prefix = pathconfig->home;
        if (safe_wcscpy(stdlib, prefix, stdlib_len) < 0) {
            return PATHLEN_ERR();
        }
        wchar_t *delim = wcschr(stdlib, DELIM);
        if (delim) {
            *delim = L'\0';
        }
        verified |= LOCATION_FORCED | LOCATION_CUSTOM;
    }
    else {
        status = search_for_stdlib_dir(calculate,
                                       stdlib, stdlib_len, &verified);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
        if (*stdlib == L'\0') {
            /* Fall back to PREFIX / <lib_python>. */
            prefix = calculate->prefix_macro;
            if (safe_wcscpy(stdlib, prefix, stdlib_len) < 0) {
                return PATHLEN_ERR();
            }
            verified |= LOCATION_DEFAULT | LOCATION_PREFIX;
        }
        else if (verified & LOCATION_WITH_FILE) {
            calculate->argv0_dir_verified |= verified & LOCATION_EXISTS;
            calculate->argv0_dir_verified |= verified & LOCATION_IN_BUILD_DIR;
            calculate->argv0_dir_verified |= verified & LOCATION_IN_SOURCE_TREE;
        }
    }

    if (prefix != NULL) {
        status = joinpath(stdlib, calculate->lib_python, stdlib_len);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
    }

    calculate->stdlib_dir = _PyMem_RawWcsdup(stdlib);
    if (calculate->stdlib_dir == NULL) {
        return _PyStatus_NO_MEMORY();
    }
    calculate->stdlib_dir_verified = verified;

    return _PyStatus_OK();
}


static PyStatus
calculate_prefix(PyCalculatePath *calculate, _PyPathConfig *pathconfig)
{
    assert(calculate->stdlib_dir != NULL);
    assert(calculate->prefix == NULL);
    assert(calculate->prefix_verified == LOCATION_UNKNOWN);

    wchar_t prefix[MAXPATHLEN+1];
    memset(prefix, 0, sizeof(prefix));
    size_t prefix_len = Py_ARRAY_LENGTH(prefix);

    /* Reduce stdlib_dir to the essence of the prefix,
     * e.g. /usr/local/lib/python1.5 is reduced to /usr/local.
    */
    if (safe_wcscpy(prefix, calculate->stdlib_dir, prefix_len) < 0) {
        return PATHLEN_ERR();
    }
    reduce(prefix);
    reduce(prefix);
    if (*prefix == L'\0') {
        /* The prefix is the root directory, but reduce() chopped
           off the "/". */
        wcscpy(prefix, separator);
    }

    calculate->prefix = _PyMem_RawWcsdup(prefix);
    if (calculate->prefix == NULL) {
        return _PyStatus_NO_MEMORY();
    }
    calculate->prefix_verified = calculate->stdlib_dir_verified;
    calculate->prefix_found = calculate->prefix_verified & LOCATION_FOUND;

    if (!calculate->prefix_found) {
        if (calculate->warnings) {
            fprintf(stderr,
                    "Could not find platform independent libraries <prefix>\n");
        }
    }

    return _PyStatus_OK();
}

static PyStatus
calculate_set_prefix(PyCalculatePath *calculate, _PyPathConfig *pathconfig)
{
    assert(pathconfig->prefix == NULL);
    const wchar_t *prefix = calculate->prefix;
    if (calculate->prefix_verified & LOCATION_IN_SOURCE_TREE) {
        /* We're loading relative to the build directory,
         * so return the compiled-in defaults instead.
         */
        prefix = calculate->prefix_macro;
    }
    pathconfig->prefix = _PyMem_RawWcsdup(prefix);
    if (pathconfig->prefix == NULL) {
        return _PyStatus_NO_MEMORY();
    }
    return _PyStatus_OK();
}


static PyStatus
calculate_pybuilddir(const wchar_t *argv0_dir,
                     wchar_t *ext_dir, size_t ext_dir_len, int *verified)
{
    PyStatus status;

    /* Check to see if argv[0] is in the build directory. "pybuilddir.txt"
       is written by setup.py and contains the relative path to the location
       of shared library modules.

       Filename: <argv0_dir> / "pybuilddir.txt" */
    wchar_t *filename = joinpath2(argv0_dir, L"pybuilddir.txt");
    if (filename == NULL) {
        return _PyStatus_NO_MEMORY();
    }

    FILE *fp = _Py_wfopen(filename, L"rb");
    PyMem_RawFree(filename);
    if (fp == NULL) {
        errno = 0;
        return _PyStatus_OK();
    }

    char buf[MAXPATHLEN + 1];
    size_t n = fread(buf, 1, Py_ARRAY_LENGTH(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);

    size_t dec_len;
    wchar_t *pybuilddir = _Py_DecodeUTF8_surrogateescape(buf, n, &dec_len);
    if (!pybuilddir) {
        return DECODE_LOCALE_ERR("pybuilddir.txt", dec_len);
    }

    /* Path: <argv0_dir> / <pybuilddir content> */
    if (safe_wcscpy(ext_dir, argv0_dir, ext_dir_len) < 0) {
        PyMem_RawFree(pybuilddir);
        return PATHLEN_ERR();
    }
    status = joinpath(ext_dir, pybuilddir, ext_dir_len);
    PyMem_RawFree(pybuilddir);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }

    *verified |= LOCATION_IN_BUILD_DIR | LOCATION_CUSTOM;
    return _PyStatus_OK();
}


/* search_for_exec_prefix requires that argv0_dir be no more than
   MAXPATHLEN bytes long.
*/
static PyStatus
search_for_extensions(PyCalculatePath *calculate,
                      wchar_t *ext_dir, size_t ext_dir_len, int *verified)
{
    assert(*verified == LOCATION_UNKNOWN);
    PyStatus status;

    /* Check for pybuilddir.txt */
    status = calculate_pybuilddir(calculate->argv0_dir,
                                  ext_dir, ext_dir_len, verified);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }
    if (*verified) {
        return _PyStatus_OK();
    }

    // XXX Try <stdlib_dir> / "lib-dynload" here first?

    /* Search from argv0_dir, until root is found */
    status = copy_absolute(ext_dir, calculate->argv0_dir, ext_dir_len);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }

    int flag = LOCATION_NEAR_ARGV0 | LOCATION_WITH_FILE;
    do {
        /* Path: <argv0_dir or substring> / <lib_python> / "lib-dynload" */
        size_t n = wcslen(ext_dir);
        status = joinpath(ext_dir, calculate->lib_python, ext_dir_len);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
        status = joinpath(ext_dir, L"lib-dynload", ext_dir_len);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
        if (isdir(ext_dir)) {
            *verified |= LOCATION_EXISTS | flag;
            return _PyStatus_OK();
        }
        ext_dir[n] = L'\0';
        reduce(ext_dir);
        flag = LOCATION_NEAR_ARGV0;
    } while (ext_dir[0]);

    /* Look at configure's EXEC_PREFIX.

       Path: <EXEC_PREFIX macro> / <lib_python> / "lib-dynload" */
    if (safe_wcscpy(ext_dir, calculate->exec_prefix_macro, ext_dir_len) < 0) {
        return PATHLEN_ERR();
    }
    status = joinpath(ext_dir, calculate->lib_python, ext_dir_len);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }
    status = joinpath(ext_dir, L"lib-dynload", ext_dir_len);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }
    if (isdir(ext_dir)) {
        *verified |= LOCATION_EXISTS | LOCATION_EXEC_PREFIX;
        return _PyStatus_OK();
    }

    /* Fail */
    return _PyStatus_OK();
}


static PyStatus
calculate_extensions_dir(PyCalculatePath *calculate, _PyPathConfig *pathconfig)
{
    PyStatus status;
    wchar_t extensions[MAXPATHLEN+1];
    memset(extensions, 0, sizeof(extensions));
    size_t extensions_len = Py_ARRAY_LENGTH(extensions);
    const wchar_t *exec_prefix = NULL;
    int verified = LOCATION_UNKNOWN;

    /* If PYTHONHOME is set, we believe it unconditionally */
    if (pathconfig->home != NULL) {
        /* Path: <home> / <lib_python> */
        wchar_t *delim = wcschr(pathconfig->home, DELIM);
        if (delim) {
            exec_prefix = delim + 1;
        }
        else {
            exec_prefix = pathconfig->home;
        }
        if (safe_wcscpy(extensions, exec_prefix, extensions_len) < 0) {
            return PATHLEN_ERR();
        }
        verified |= LOCATION_FORCED | LOCATION_CUSTOM;
    }
    else {
        status = search_for_extensions(calculate,
                                       extensions, extensions_len, &verified);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
        if (*extensions == L'\0') {
            /* Fall back to EXEC_PREFIX / <lib_python> / "lib-dynload". */
            exec_prefix = calculate->exec_prefix_macro;
            if (safe_wcscpy(extensions, exec_prefix, extensions_len) < 0) {
                return PATHLEN_ERR();
            }
            verified |= LOCATION_DEFAULT | LOCATION_EXEC_PREFIX;
        }
        else if (verified & LOCATION_WITH_FILE) {
            calculate->argv0_dir_verified |= verified & LOCATION_EXISTS;
        }
    }

    if (exec_prefix != NULL) {
        status = joinpath(extensions, calculate->lib_python, extensions_len);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
        status = joinpath(extensions, L"lib-dynload", extensions_len);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
    }

    calculate->extensions= _PyMem_RawWcsdup(extensions);
    if (calculate->extensions== NULL) {
        return _PyStatus_NO_MEMORY();
    }
    calculate->extensions_verified = verified;

    return _PyStatus_OK();
}

static PyStatus
calculate_exec_prefix(PyCalculatePath *calculate, _PyPathConfig *pathconfig)
{
    assert(calculate->extensions != NULL);
    assert(calculate->exec_prefix == NULL);
    assert(calculate->exec_prefix_verified == LOCATION_UNKNOWN);

    wchar_t exec_prefix[MAXPATHLEN+1];
    memset(exec_prefix, 0, sizeof(exec_prefix));
    size_t exec_prefix_len = Py_ARRAY_LENGTH(exec_prefix);

    int verified = calculate->extensions_verified;
    bool found = verified & LOCATION_FOUND;
    if (verified & LOCATION_IN_BUILD_DIR) {
        /* Fall back to EXEC_PREFIX. */
        if (safe_wcscpy(exec_prefix, calculate->exec_prefix_macro,
                        exec_prefix_len) < 0) {
            return PATHLEN_ERR();
        }
        verified = LOCATION_DEFAULT | LOCATION_EXEC_PREFIX;
        found = true;
    }
    else {
        /* Reduce extensions to the essence of the exc prefix,
         * e.g. /usr/local/lib/python1.5/lib-dynload is reduced to /usr/local.
        */
        if (safe_wcscpy(exec_prefix, calculate->extensions, exec_prefix_len) < 0) {
            return PATHLEN_ERR();
        }
        reduce(exec_prefix);
        reduce(exec_prefix);
        reduce(exec_prefix);
        if (*exec_prefix == L'\0') {
            /* exec_prefix is the root directory, but reduce() chopped
               off the "/". */
            wcscpy(exec_prefix, separator);
        }
    }

    calculate->exec_prefix = _PyMem_RawWcsdup(exec_prefix);
    if (calculate->exec_prefix == NULL) {
        return _PyStatus_NO_MEMORY();
    }
    calculate->exec_prefix_verified = verified;
    calculate->exec_prefix_found = found;

    if (!calculate->exec_prefix_found) {
        if (calculate->warnings) {
            fprintf(stderr,
                    "Could not find platform dependent libraries <exec_prefix>\n");
        }
    }

    return _PyStatus_OK();
}


static PyStatus
calculate_set_exec_prefix(PyCalculatePath *calculate,
                          _PyPathConfig *pathconfig)
{
    assert(calculate->exec_prefix != NULL);
    assert(pathconfig->exec_prefix == NULL);
    pathconfig->exec_prefix = _PyMem_RawWcsdup(calculate->exec_prefix);
    if (pathconfig->exec_prefix == NULL) {
        return _PyStatus_NO_MEMORY();
    }
    return _PyStatus_OK();
}


/* Similar to shutil.which().
   If found, write the path into *abs_path_p. */
static PyStatus
calculate_which(const wchar_t *path_env, wchar_t *program_name,
                wchar_t **abs_path_p)
{
    while (1) {
        wchar_t *delim = wcschr(path_env, DELIM);
        wchar_t *abs_path;

        if (delim) {
            wchar_t *path = substring(path_env, delim - path_env);
            if (path == NULL) {
                return _PyStatus_NO_MEMORY();
            }
            abs_path = joinpath2(path, program_name);
            PyMem_RawFree(path);
        }
        else {
            abs_path = joinpath2(path_env, program_name);
        }

        if (abs_path == NULL) {
            return _PyStatus_NO_MEMORY();
        }

        if (isxfile(abs_path)) {
            *abs_path_p = abs_path;
            return _PyStatus_OK();
        }
        PyMem_RawFree(abs_path);

        if (!delim) {
            break;
        }
        path_env = delim + 1;
    }

    /* not found */
    return _PyStatus_OK();
}


#ifdef __APPLE__
static PyStatus
calculate_program_macos(wchar_t **abs_path_p)
{
    char execpath[MAXPATHLEN + 1];
    uint32_t nsexeclength = Py_ARRAY_LENGTH(execpath) - 1;

    /* On Mac OS X, if a script uses an interpreter of the form
       "#!/opt/python2.3/bin/python", the kernel only passes "python"
       as argv[0], which falls through to the $PATH search below.
       If /opt/python2.3/bin isn't in your path, or is near the end,
       this algorithm may incorrectly find /usr/bin/python. To work
       around this, we can use _NSGetExecutablePath to get a better
       hint of what the intended interpreter was, although this
       will fail if a relative path was used. but in that case,
       absolutize() should help us out below
     */
    if (_NSGetExecutablePath(execpath, &nsexeclength) != 0
        || (wchar_t)execpath[0] != SEP)
    {
        /* _NSGetExecutablePath() failed or the path is relative */
        return _PyStatus_OK();
    }

    size_t len;
    *abs_path_p = Py_DecodeLocale(execpath, &len);
    if (*abs_path_p == NULL) {
        return DECODE_LOCALE_ERR("executable path", len);
    }
    return _PyStatus_OK();
}
#endif  /* __APPLE__ */


static PyStatus
calculate_program_impl(PyCalculatePath *calculate, _PyPathConfig *pathconfig)
{
    assert(pathconfig->program_full_path == NULL);

    PyStatus status;

    /* If there is no slash in the argv0 path, then we have to
     * assume python is on the user's $PATH, since there's no
     * other way to find a directory to start the search from.  If
     * $PATH isn't exported, you lose.
     */
    if (wcschr(pathconfig->program_name, SEP)) {
        pathconfig->program_full_path = _PyMem_RawWcsdup(pathconfig->program_name);
        if (pathconfig->program_full_path == NULL) {
            return _PyStatus_NO_MEMORY();
        }
        return _PyStatus_OK();
    }

#ifdef __APPLE__
    wchar_t *abs_path = NULL;
    status = calculate_program_macos(&abs_path);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }
    if (abs_path) {
        pathconfig->program_full_path = abs_path;
        return _PyStatus_OK();
    }
#endif /* __APPLE__ */

    if (calculate->path_env) {
        wchar_t *abs_path = NULL;
        status = calculate_which(calculate->path_env, pathconfig->program_name,
                                 &abs_path);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
        if (abs_path) {
            pathconfig->program_full_path = abs_path;
            return _PyStatus_OK();
        }
    }

    /* In the last resort, use an empty string */
    pathconfig->program_full_path = _PyMem_RawWcsdup(L"");
    if (pathconfig->program_full_path == NULL) {
        return _PyStatus_NO_MEMORY();
    }
    return _PyStatus_OK();
}


/* Calculate pathconfig->program_full_path */
static PyStatus
calculate_program(PyCalculatePath *calculate, _PyPathConfig *pathconfig)
{
    PyStatus status;

    status = calculate_program_impl(calculate, pathconfig);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }

    if (pathconfig->program_full_path[0] != '\0') {
        /* program_full_path is not empty */

        /* Make sure that program_full_path is an absolute path */
        if (!_Py_isabs(pathconfig->program_full_path)) {
            status = absolutize(&pathconfig->program_full_path);
            if (_PyStatus_EXCEPTION(status)) {
                return status;
            }
        }

#if defined(__CYGWIN__) || defined(__MINGW32__)
        /* For these platforms it is necessary to ensure that the .exe suffix
         * is appended to the filename, otherwise there is potential for
         * sys.executable to return the name of a directory under the same
         * path (bpo-28441).
         */
        status = add_exe_suffix(&pathconfig->program_full_path);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
#endif
    }
    return _PyStatus_OK();
}


#if HAVE_READLINK
static PyStatus
resolve_symlinks(wchar_t *buf, size_t bufsize)
{
    wchar_t new_path[MAXPATHLEN + 1];
    const size_t new_path_len = Py_ARRAY_LENGTH(new_path);
    unsigned int nlink = 0;

    while (1) {
        int linklen = _Py_wreadlink(buf, new_path, new_path_len);
        if (linklen == -1) {
            /* We could not read a symbolic link: we are done.
             * Note that we are silencing errors here. */
            break;
        }

        if (_Py_isabs(new_path)) {
            if (safe_wcscpy(buf, new_path, bufsize) < 0) {
                return PATHLEN_ERR();
            }
        }
        else {
            /* new_path is relative to the current one. */
            reduce(buf);
            PyStatus status = joinpath(buf, new_path, bufsize);
            if (_PyStatus_EXCEPTION(status)) {
                return status;
            }
        }

        nlink++;
        /* 40 is the Linux kernel 4.2 limit */
        if (nlink >= 40) {
            return _PyStatus_ERR("maximum number of symbolic links reached");
        }
    }
    return _PyStatus_OK();
}
#endif /* HAVE_READLINK */


#ifdef WITH_NEXT_FRAMEWORK
static PyStatus
calculate_argv0_dir_framework(PyCalculatePath *calculate, _PyPathConfig *pathconfig,
                              wchar_t *argv0, size_t argv0_len, int *verified)
{
    NSModule pythonModule;

    /* On Mac OS X we have a special case if we're running from a framework.
       This is because the python home should be set relative to the library,
       which is in the framework, not relative to the executable, which may
       be outside of the framework. Except when we're in the build
       directory... */
    pythonModule = NSModuleForSymbol(NSLookupAndBindSymbol("_Py_Initialize"));

    /* Use dylib functions to find out where the framework was loaded from */
    const char* modPath = NSLibraryNameForModule(pythonModule);
    if (modPath == NULL) {
        return _PyStatus_OK();
    }

    /* We're in a framework.
       See if we might be in the build directory. The framework in the
       build directory is incomplete, it only has the .dylib and a few
       needed symlinks, it doesn't have the Lib directories and such.
       If we're running with the framework from the build directory we must
       be running the interpreter in the build directory, so we use the
       build-directory-specific logic to find Lib and such. */
    size_t len;
    wchar_t* framework_exe = Py_DecodeLocale(modPath, &len);
    if (framework_exe == NULL) {
        return DECODE_LOCALE_ERR("framework location", len);
    }

    /* Path: reduce(modPath) / lib_python / LANDMARK */
    wchar_t stdlib[MAXPATHLEN+1];
    size_t stdlib_len = Py_ARRAY_LENGTH(stdlib);
    if (safe_wcscpy(stdlib, framework_exe, stdlib_len) < 0) {
        return PATHLEN_ERR();
    }
    reduce(stdlib);

    PyStatus status = joinpath(stdlib, calculate->lib_python, stdlib_len);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }

    int module;
    status = ismodule(stdlib, &module);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }
    if (module) {
        /* Use the location of the library as argv0_dir */
        if (safe_wcscpy(argv0, framework_exe, argv0_len) < 0) {
            return PATHLEN_ERR();
        }
        *verified |= LOCATION_EXISTS;
        // XXX Set calculate->stdlib_dir, etc.?
        return _PyStatus_OK();
    }

    /* We are in the build directory so use the name of the
       executable - we know that the absolute path is passed */
    assert(wcscmp(argv0, pathconfig->program_full_path) == 0);
    *verified |= LOCATION_IN_BUILD_DIR;
    return _PyStatus_OK();
}
#endif


static PyStatus
calculate_argv0_dir(PyCalculatePath *calculate,
                     _PyPathConfig *pathconfig)
{
    assert(calculate->argv0_dir == NULL);
    assert(calculate->argv0_dir_verified == LOCATION_UNKNOWN);

    PyStatus status;
    wchar_t argv0[MAXPATHLEN+1];
    size_t argv0_len = Py_ARRAY_LENGTH(argv0);
    int verified = LOCATION_UNKNOWN;

    if (safe_wcscpy(argv0, pathconfig->program_full_path, argv0_len) < 0) {
        return PATHLEN_ERR();
    }

#ifdef WITH_NEXT_FRAMEWORK
    status = calculate_argv0_dir_framework(calculate, pathconfig,
                                           argv0, argv0_len, &verified);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }
#endif

    // XXX Don't we need to wrap this in "#if HAVE_READLINK"?
    status = resolve_symlinks(argv0, argv0_len);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }

    reduce(argv0);
    calculate->argv0_dir = _PyMem_RawWcsdup(argv0);
    if (calculate->argv0_dir == NULL) {
        return _PyStatus_NO_MEMORY();
    }
    calculate->argv0_dir_verified = verified |
                                    LOCATION_NEAR_ARGV0 | LOCATION_WITH_FILE;

    return _PyStatus_OK();
}


static PyStatus
calculate_open_pyenv(PyCalculatePath *calculate, FILE **env_file_p)
{
    *env_file_p = NULL;

    const wchar_t *env_cfg = L"pyvenv.cfg";

    /* Filename: <argv0_dir> / "pyvenv.cfg" */
    wchar_t *filename = joinpath2(calculate->argv0_dir, env_cfg);
    if (filename == NULL) {
        return _PyStatus_NO_MEMORY();
    }

    *env_file_p = _Py_wfopen(filename, L"r");
    PyMem_RawFree(filename);

    if (*env_file_p != NULL) {
        return _PyStatus_OK();

    }

    /* fopen() failed: reset errno */
    errno = 0;

    /* Path: <basename(argv0_dir)> / "pyvenv.cfg" */
    wchar_t *parent = _PyMem_RawWcsdup(calculate->argv0_dir);
    if (parent == NULL) {
        return _PyStatus_NO_MEMORY();
    }
    reduce(parent);

    filename = joinpath2(parent, env_cfg);
    PyMem_RawFree(parent);
    if (filename == NULL) {
        return _PyStatus_NO_MEMORY();
    }

    *env_file_p = _Py_wfopen(filename, L"r");
    PyMem_RawFree(filename);

    if (*env_file_p == NULL) {
        /* fopen() failed: reset errno */
        errno = 0;
    }
    return _PyStatus_OK();
}


/* Search for an "pyvenv.cfg" environment configuration file, first in the
   executable's directory and then in the parent directory.
   If found, open it for use when searching for prefixes.

   Write the 'home' variable of pyvenv.cfg into calculate->argv0_dir. */
static PyStatus
calculate_read_pyenv(PyCalculatePath *calculate)
{
    PyStatus status;
    FILE *env_file = NULL;

    status = calculate_open_pyenv(calculate, &env_file);
    if (_PyStatus_EXCEPTION(status)) {
        assert(env_file == NULL);
        return status;
    }
    if (env_file == NULL) {
        /* pyvenv.cfg not found */
        return _PyStatus_OK();
    }

    /* Look for a 'home' variable and set argv0_dir to it, if found */
    wchar_t *home = NULL;
    status = _Py_FindEnvConfigValue(env_file, L"home", &home);
    if (_PyStatus_EXCEPTION(status)) {
        fclose(env_file);
        return status;
    }

    if (home) {
        PyMem_RawFree(calculate->argv0_dir);
        calculate->argv0_dir = home;
    }
    fclose(env_file);
    return _PyStatus_OK();
}


static PyStatus
calculate_zip_path(PyCalculatePath *calculate)
{
    /* Use the reduced prefix returned by Py_GetPrefix().
       Note that we're using PREFIX if we couldn't find the prefix already.

       Path: <prefix> / <platlibdir> / "pythonXY.zip" */

    wchar_t *relpath = joinpath2(
        calculate->platlibdir,
        L"python" Py_STRINGIFY(PY_MAJOR_VERSION) Py_STRINGIFY(PY_MINOR_VERSION) L".zip");
    if (relpath == NULL) {
        return _PyStatus_NO_MEMORY();
    }

    calculate->zip_path = joinpath2(calculate->prefix, relpath);
    PyMem_RawFree(relpath);
    if (calculate->zip_path == NULL) {
        return _PyStatus_NO_MEMORY();
    }

    return _PyStatus_OK();
}


static PyStatus
calculate_module_search_path(PyCalculatePath *calculate,
                             _PyPathConfig *pathconfig)
{
    assert(calculate->stdlib_dir != NULL);

    /* Calculate size of return buffer */
    size_t bufsz = 0;
    if (calculate->pythonpath_env != NULL) {
        bufsz += wcslen(calculate->pythonpath_env) + 1;
    }

    wchar_t *defpath = calculate->pythonpath_macro;
    size_t stdlibsz = wcslen(calculate->stdlib_dir) + 1;
    while (1) {
        wchar_t *delim = wcschr(defpath, DELIM);

        if (!_Py_isabs(defpath)) {
            /* Paths are relative to prefix */
            bufsz += stdlibsz;
        }

        if (delim) {
            bufsz += delim - defpath + 1;
        }
        else {
            bufsz += wcslen(defpath) + 1;
            break;
        }
        defpath = delim + 1;
    }

    bufsz += wcslen(calculate->zip_path) + 1;
    bufsz += wcslen(calculate->extensions) + 1;

    /* Allocate the buffer */
    wchar_t *buf = PyMem_RawMalloc(bufsz * sizeof(wchar_t));
    if (buf == NULL) {
        return _PyStatus_NO_MEMORY();
    }
    buf[0] = '\0';

    /* Run-time value of $PYTHONPATH goes first */
    if (calculate->pythonpath_env) {
        wcscpy(buf, calculate->pythonpath_env);
        wcscat(buf, delimiter);
    }

    /* Next is the default zip path */
    wcscat(buf, calculate->zip_path);
    wcscat(buf, delimiter);

    /* Next goes merge of compile-time $PYTHONPATH with
     * dynamically located stdlib.
     */
    defpath = calculate->pythonpath_macro;
    while (1) {
        wchar_t *delim = wcschr(defpath, DELIM);

        if (!_Py_isabs(defpath)) {
            wcscat(buf, calculate->stdlib_dir);
            if (stdlibsz >= 2 && calculate->stdlib_dir[stdlibsz - 2] != SEP &&
                defpath[0] != (delim ? DELIM : L'\0'))
            {
                /* not empty */
                wcscat(buf, separator);
            }
        }

        if (delim) {
            size_t len = delim - defpath + 1;
            size_t end = wcslen(buf) + len;
            wcsncat(buf, defpath, len);
            buf[end] = '\0';
        }
        else {
            wcscat(buf, defpath);
            break;
        }
        defpath = delim + 1;
    }
    wcscat(buf, delimiter);

    /* Finally, on goes the directory for dynamic-load modules */
    wcscat(buf, calculate->extensions);

    pathconfig->module_search_path = buf;
    return _PyStatus_OK();
}


static PyStatus
calculate_init(PyCalculatePath *calculate, const PyConfig *config)
{
    size_t len;

    calculate->warnings = config->pathconfig_warnings;
    calculate->pythonpath_env = config->pythonpath_env;
    calculate->platlibdir = config->platlibdir;

    const char *path = getenv("PATH");
    if (path) {
        calculate->path_env = Py_DecodeLocale(path, &len);
        if (!calculate->path_env) {
            return DECODE_LOCALE_ERR("PATH environment variable", len);
        }
    }

    /* Decode macros */
    calculate->pythonpath_macro = Py_DecodeLocale(PYTHONPATH, &len);
    if (!calculate->pythonpath_macro) {
        return DECODE_LOCALE_ERR("PYTHONPATH macro", len);
    }
    calculate->prefix_macro = Py_DecodeLocale(PREFIX, &len);
    if (!calculate->prefix_macro) {
        return DECODE_LOCALE_ERR("PREFIX macro", len);
    }
    calculate->exec_prefix_macro = Py_DecodeLocale(EXEC_PREFIX, &len);
    if (!calculate->exec_prefix_macro) {
        return DECODE_LOCALE_ERR("EXEC_PREFIX macro", len);
    }
    calculate->vpath_macro = Py_DecodeLocale(VPATH, &len);
    if (!calculate->vpath_macro) {
        return DECODE_LOCALE_ERR("VPATH macro", len);
    }

    // <platlibdir> / "pythonX.Y"
    wchar_t *pyversion = Py_DecodeLocale("python" VERSION, &len);
    if (!pyversion) {
        return DECODE_LOCALE_ERR("VERSION macro", len);
    }
    calculate->lib_python = joinpath2(config->platlibdir, pyversion);
    PyMem_RawFree(pyversion);
    if (calculate->lib_python == NULL) {
        return _PyStatus_NO_MEMORY();
    }

    return _PyStatus_OK();
}


static void
calculate_free(PyCalculatePath *calculate)
{
    PyMem_RawFree(calculate->pythonpath_macro);
    PyMem_RawFree(calculate->prefix_macro);
    PyMem_RawFree(calculate->exec_prefix_macro);
    PyMem_RawFree(calculate->vpath_macro);
    PyMem_RawFree(calculate->lib_python);
    PyMem_RawFree(calculate->path_env);
    PyMem_RawFree(calculate->zip_path);
    PyMem_RawFree(calculate->argv0_dir);
    PyMem_RawFree(calculate->stdlib_dir);
    PyMem_RawFree(calculate->extensions);
    PyMem_RawFree(calculate->prefix);
    PyMem_RawFree(calculate->exec_prefix);
}


static PyStatus
calculate_path(PyCalculatePath *calculate, _PyPathConfig *pathconfig)
{
    PyStatus status;

    if (pathconfig->program_full_path == NULL) {
        status = calculate_program(calculate, pathconfig);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
    }

    status = calculate_argv0_dir(calculate, pathconfig);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }

    /* If a pyvenv.cfg configure file is found,
       argv0_dir is overridden with its 'home' variable. */
    status = calculate_read_pyenv(calculate);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }

    status = calculate_stdlib_dir(calculate, pathconfig);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }

    status = calculate_prefix(calculate, pathconfig);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }

    status = calculate_zip_path(calculate);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }

    status = calculate_extensions_dir(calculate, pathconfig);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }

    status = calculate_exec_prefix(calculate, pathconfig);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }

    if ((!calculate->prefix_found || !calculate->exec_prefix_found)
        && calculate->warnings)
    {
        fprintf(stderr,
                "Consider setting $PYTHONHOME to <stdlib>[:<exec_prefix>]\n");
    }

    if (pathconfig->module_search_path == NULL) {
        status = calculate_module_search_path(calculate, pathconfig);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
    }

    if (pathconfig->stdlib_dir == NULL) {
        // XXX Drop the prefix_found check.  stdlib_dir_verified can be
        // used where the distinction is needed.
        if (calculate->stdlib_dir != NULL && calculate->prefix_found) {
            pathconfig->stdlib_dir = _PyMem_RawWcsdup(calculate->stdlib_dir);
            if (pathconfig->stdlib_dir == NULL) {
                return _PyStatus_NO_MEMORY();
            }
        }
    }

    if (pathconfig->prefix == NULL) {
        status = calculate_set_prefix(calculate, pathconfig);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
    }

    if (pathconfig->exec_prefix == NULL) {
        status = calculate_set_exec_prefix(calculate, pathconfig);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
    }
    return _PyStatus_OK();
}


/* Calculate the Python path configuration.

   Inputs:

   - PATH environment variable
   - Macros: PYTHONPATH, PREFIX, EXEC_PREFIX, VERSION (ex: "3.9").
     PREFIX and EXEC_PREFIX are generated by the configure script.
     PYTHONPATH macro is the default search path.
   - pybuilddir.txt file
   - pyvenv.cfg configuration file
   - PyConfig fields ('config' function argument):

     - pathconfig_warnings
     - pythonpath_env (PYTHONPATH environment variable)

   - _PyPathConfig fields ('pathconfig' function argument):

     - program_name: see config_init_program_name()
     - home: Py_SetPythonHome() or PYTHONHOME environment variable

   - current working directory: see copy_absolute()

   Outputs, 'pathconfig' fields:

   - program_full_path
   - module_search_path
   - prefix
   - exec_prefix

   If a field is already set (non NULL), it is left unchanged. */
PyStatus
_PyPathConfig_Calculate(_PyPathConfig *pathconfig, const PyConfig *config)
{
    PyStatus status;
    PyCalculatePath calculate;
    memset(&calculate, 0, sizeof(calculate));

    status = calculate_init(&calculate, config);
    if (_PyStatus_EXCEPTION(status)) {
        goto done;
    }

    status = calculate_path(&calculate, pathconfig);
    if (_PyStatus_EXCEPTION(status)) {
        goto done;
    }

    /* program_full_path must an either an empty string or an absolute path */
    assert(wcslen(pathconfig->program_full_path) == 0
           || _Py_isabs(pathconfig->program_full_path));

    status = _PyStatus_OK();

done:
    calculate_free(&calculate);
    return status;
}

#ifdef __cplusplus
}
#endif
