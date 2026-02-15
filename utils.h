#ifndef __UTILS_H__
#define __UTILS_H__

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

uint64_t fnv1a_init(void);
uint64_t fnv1a_update(uint64_t hash, const void *data, size_t len);
void fnv1a_hex(uint64_t hash, char out[17]);
int hash_string(const char *s, char out_hex[17]);
int hash_path_recursive(const char *path, char out_hex[17]);

int generate_uuid(char out[64]);

int path_exists(const char *path);
int is_directory(const char *path);
int ensure_dir_exists(const char *path, mode_t mode);
int ensure_parent_dirs(const char *path, mode_t mode);

int copy_file_data(const char *src, const char *dst, mode_t mode);
int copy_path_recursive(const char *src, const char *dst);
unsigned long long dir_size_bytes(const char *path);
int remove_recursive(const char *path);

int join_paths(const char *a, const char *b, char *out, size_t out_size);
int normalize_container_path(const char *base_workdir, const char *path, char *out,
                             size_t out_size);

char *trim_whitespace(char *s);
int starts_with(const char *s, const char *prefix);
int ends_with(const char *s, const char *suffix);

#endif
