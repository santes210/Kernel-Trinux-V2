#ifndef FS_PATH_H
#define FS_PATH_H

#include "../lib/types.h"

#define PATH_MAX 256

/* Normalize a path: collapse "//", resolve "." and "..". Result written to
 * out (size PATH_MAX). `base` is the cwd used when `path` is relative. */
void path_normalize(const char *base, const char *path, char *out);

/* Join two components into out. */
void path_join(const char *a, const char *b, char *out);

/* basename / dirname write into out buffers. */
void path_basename(const char *path, char *out);
void path_dirname(const char *path, char *out);

bool path_is_absolute(const char *path);

#endif /* FS_PATH_H */
