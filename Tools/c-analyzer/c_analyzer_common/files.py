import glob
import os
import os.path


C_SOURCE_SUFFIXES = ('.c', '.h')


def _walk_tree(root):
    # A wrapper around os.walk that resolves the filenames.
    for parent, _, names in _walk(root):
        for name in names:
            yield os.path.join(parent, name)


def walk_tree(root, *,
              suffix=None,
              walk=_walk_tree,
              ):
    """Yield each file in the tree under the given directory name.

    If "suffix" is provided then only files with that suffix will
    be included.
    """
    if suffix and not isinstance(suffix, str):
        raise ValueError('suffix must be a string')

    for parent, _, names in walk(root):
        for name in names:
            if suffix and not name.endswith(suffix):
                continue
            yield os.path.join(parent, name)


def glob_tree(root, *,
              suffix=None,
              _glob=glob.iglob,
              ):
    """Yield each file in the tree under the given directory name.

    If "suffix" is provided then only files with that suffix will
    be included.
    """
    suffix = suffix or ''
    if not isinstance(suffix, str):
        raise ValueError('suffix must be a string')

    for filename in _glob(f'{root}/*{suffix}'):
        yield filename
    for filename in _glob(f'{root}/**/*{suffix}'):
        yield filename


def iter_files(root, suffix=None, relparent=None, *,
               get_files=os.walk,
               _glob=glob_tree,
               _walk=walk_tree,
               ):
    """Yield each file in the tree under the given directory name.

    If "root" is a non-string iterable then do the same for each of
    those trees.

    If "suffix" is provided then only files with that suffix will
    be included.

    if "relparent" is provided then it is used to resolve each
    filename as a relative path.
    """
    if not isinstance(root, str):
        roots = root
        for root in roots:
            yield from iter_files(root, suffix, relparent,
                                  get_files=get_files,
                                  _glob=_glob, _walk=_walk)
        return

    # Use the right "walk" function.
    if get_files in (glob.glob, glob.iglob, glob_tree):
        get_files = _glob
    else:
        _files = _walk_tree if get_files in (os.walk, walk_tree) else get_files
        get_files = (lambda *a, **k: _walk(*a, walk=_files, **k))

    # Handle a single suffix.
    if suffix and not isinstance(suffix, str):
        filenames = get_files(root)
        suffix = tuple(suffix)
    else:
        filenames = get_files(root, suffix=suffix)
        suffix = None

    for filename in filenames:
        if suffix and not isinstance(suffix, str):  # multiple suffixes
            if not filename.endswith(suffix):
                continue
        if relparent:
            filename = os.path.relpath(filename, relparent)
        yield filename


def iter_files_by_suffix(root, suffixes, relparent=None, *,
                         walk=walk_tree,
                         _iter_files=iter_files,
                         ):
    """Yield each file in the tree that has the given suffixes.

    Unlike iter_files(), the results are in the original suffix order.
    """
    if isinstance(suffixes, str):
        suffixes = [suffixes]
    # XXX Ignore repeated suffixes?
    for suffix in suffixes:
        yield from _iter_files(root, None, relparent,
                               get_files=get_files,
                               )


def iter_cpython_files(*,
                       walk=walk_tree,
                       _files=iter_files_by_suffix,
                       ):
    """Yield each file in the tree for each of the given directory names."""
    excludedtrees = [
        os.path.join('Include', 'cpython', ''),
        ]
    def is_excluded(filename):
        for root in excludedtrees:
            if filename.startswith(root):
                return True
        return False
    for filename in _files(SOURCE_DIRS, C_SOURCE_SUFFIXES, REPO_ROOT,
                           walk=walk,
                           ):
        if is_excluded(filename):
            continue
        yield filename
