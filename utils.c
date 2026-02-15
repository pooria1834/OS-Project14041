#define _GNU_SOURCE

#include "utils.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static int str_cmp(const void *a, const void *b) {
  const char *const *sa = (const char *const *)a;
  const char *const *sb = (const char *const *)b;
  return strcmp(*sa, *sb);
}

uint64_t fnv1a_init(void) { return 1469598103934665603ULL; }

uint64_t fnv1a_update(uint64_t hash, const void *data, size_t len) {
  const unsigned char *p = (const unsigned char *)data;
  size_t i;

  for (i = 0; i < len; i++) {
    hash ^= (uint64_t)p[i];
    hash *= 1099511628211ULL;
  }

  return hash;
}

void fnv1a_hex(uint64_t hash, char out[17]) { snprintf(out, 17, "%016llx", (unsigned long long)hash); }

int hash_string(const char *s, char out_hex[17]) {
  uint64_t h = fnv1a_init();
  if (s == NULL) {
    return 1;
  }

  h = fnv1a_update(h, s, strlen(s));
  fnv1a_hex(h, out_hex);
  return 0;
}

static int hash_file_content(const char *path, uint64_t *hash) {
  int fd;
  ssize_t n;
  unsigned char buf[8192];

  fd = open(path, O_RDONLY);
  if (fd < 0) {
    return 1;
  }

  while ((n = read(fd, buf, sizeof(buf))) > 0) {
    *hash = fnv1a_update(*hash, buf, (size_t)n);
  }

  close(fd);
  return n < 0;
}

static int hash_path_internal(const char *path, const char *rel, uint64_t *hash) {
  struct stat st;

  if (lstat(path, &st) != 0) {
    return 1;
  }

  if (S_ISDIR(st.st_mode)) {
    DIR *dir;
    struct dirent *ent;
    char marker = 'D';
    char **names = NULL;
    size_t count = 0;
    size_t i;

    *hash = fnv1a_update(*hash, &marker, 1);
    *hash = fnv1a_update(*hash, rel, strlen(rel));

    dir = opendir(path);
    if (dir == NULL) {
      return 1;
    }

    while ((ent = readdir(dir)) != NULL) {
      char *name_copy;
      if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
        continue;
      }

      name_copy = strdup(ent->d_name);
      if (name_copy == NULL) {
        closedir(dir);
        return 1;
      }

      {
        char **new_names = realloc(names, sizeof(char *) * (count + 1));
        if (new_names == NULL) {
          free(name_copy);
          closedir(dir);
          return 1;
        }
        names = new_names;
      }

      names[count++] = name_copy;
    }
    closedir(dir);

    qsort(names, count, sizeof(char *), str_cmp);

    for (i = 0; i < count; i++) {
      char child_path[PATH_MAX];
      char child_rel[PATH_MAX];
      int rc;

      if (rel[0] == '\0') {
        snprintf(child_rel, sizeof(child_rel), "%s", names[i]);
      } else {
        snprintf(child_rel, sizeof(child_rel), "%s/%s", rel, names[i]);
      }

      snprintf(child_path, sizeof(child_path), "%s/%s", path, names[i]);
      rc = hash_path_internal(child_path, child_rel, hash);
      free(names[i]);

      if (rc != 0) {
        size_t j;
        for (j = i + 1; j < count; j++) {
          free(names[j]);
        }
        free(names);
        return rc;
      }
    }

    free(names);
    return 0;
  }

  if (S_ISREG(st.st_mode)) {
    char marker = 'F';
    *hash = fnv1a_update(*hash, &marker, 1);
    *hash = fnv1a_update(*hash, rel, strlen(rel));
    *hash = fnv1a_update(*hash, &st.st_size, sizeof(st.st_size));
    return hash_file_content(path, hash);
  }

  if (S_ISLNK(st.st_mode)) {
    char marker = 'L';
    char target[PATH_MAX];
    ssize_t n = readlink(path, target, sizeof(target) - 1);

    if (n < 0) {
      return 1;
    }

    target[n] = '\0';
    *hash = fnv1a_update(*hash, &marker, 1);
    *hash = fnv1a_update(*hash, rel, strlen(rel));
    *hash = fnv1a_update(*hash, target, (size_t)n);
    return 0;
  }

  {
    char marker = 'O';
    *hash = fnv1a_update(*hash, &marker, 1);
    *hash = fnv1a_update(*hash, rel, strlen(rel));
    *hash = fnv1a_update(*hash, &st.st_mode, sizeof(st.st_mode));
  }

  return 0;
}

int hash_path_recursive(const char *path, char out_hex[17]) {
  uint64_t h = fnv1a_init();

  if (path == NULL) {
    return 1;
  }

  if (hash_path_internal(path, "", &h) != 0) {
    return 1;
  }

  fnv1a_hex(h, out_hex);
  return 0;
}

int generate_uuid(char out[64]) {
  FILE *fp = fopen("/proc/sys/kernel/random/uuid", "r");

  if (fp != NULL) {
    if (fgets(out, 64, fp) != NULL) {
      fclose(fp);
      out[strcspn(out, "\r\n")] = '\0';
      if (out[0] != '\0') {
        return 0;
      }
    } else {
      fclose(fp);
    }
  }

  {
    unsigned int seed;
    unsigned int r1;
    unsigned int r2;
    unsigned int r3;
    unsigned int r4;

    seed = (unsigned int)time(NULL) ^ (unsigned int)getpid();
    srand(seed);
    r1 = (unsigned int)rand();
    r2 = (unsigned int)rand();
    r3 = (unsigned int)rand();
    r4 = (unsigned int)rand();

    snprintf(out, 64, "%08x-%04x-%04x-%04x-%08x%04x", r1, r2 & 0xffff,
             (r2 >> 8) & 0xffff, r3 & 0xffff, r4, (r3 >> 8) & 0xffff);
  }

  return 0;
}

int path_exists(const char *path) {
  struct stat st;
  return path != NULL && stat(path, &st) == 0;
}

int is_directory(const char *path) {
  struct stat st;
  if (path == NULL) {
    return 0;
  }
  if (stat(path, &st) != 0) {
    return 0;
  }
  return S_ISDIR(st.st_mode);
}

int ensure_dir_exists(const char *path, mode_t mode) {
  struct stat st;

  if (path == NULL || path[0] == '\0') {
    return 1;
  }

  if (stat(path, &st) == 0) {
    return S_ISDIR(st.st_mode) ? 0 : 1;
  }

  if (errno != ENOENT) {
    return 1;
  }

  return mkdir(path, mode);
}

int ensure_parent_dirs(const char *path, mode_t mode) {
  char tmp[PATH_MAX];
  size_t i;
  size_t len;

  if (path == NULL) {
    return 1;
  }

  strncpy(tmp, path, sizeof(tmp) - 1);
  tmp[sizeof(tmp) - 1] = '\0';

  len = strlen(tmp);
  for (i = 1; i < len; i++) {
    if (tmp[i] == '/') {
      char saved = tmp[i];
      tmp[i] = '\0';
      if (tmp[0] != '\0' && ensure_dir_exists(tmp, mode) != 0 && errno != EEXIST) {
        return 1;
      }
      tmp[i] = saved;
    }
  }

  return 0;
}

int copy_file_data(const char *src, const char *dst, mode_t mode) {
  int src_fd;
  int dst_fd;
  unsigned char buf[8192];
  ssize_t n;

  if (ensure_parent_dirs(dst, 0755) != 0) {
    return 1;
  }

  src_fd = open(src, O_RDONLY);
  if (src_fd < 0) {
    return 1;
  }

  dst_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode);
  if (dst_fd < 0) {
    close(src_fd);
    return 1;
  }

  while ((n = read(src_fd, buf, sizeof(buf))) > 0) {
    ssize_t written = 0;
    while (written < n) {
      ssize_t w = write(dst_fd, buf + written, (size_t)(n - written));
      if (w < 0) {
        close(src_fd);
        close(dst_fd);
        return 1;
      }
      written += w;
    }
  }

  close(src_fd);
  close(dst_fd);
  return n < 0;
}

int copy_path_recursive(const char *src, const char *dst) {
  struct stat st;

  if (lstat(src, &st) != 0) {
    return 1;
  }

  if (S_ISDIR(st.st_mode)) {
    DIR *dir;
    struct dirent *ent;

    if (ensure_dir_exists(dst, st.st_mode & 0777) != 0 && errno != EEXIST) {
      return 1;
    }

    dir = opendir(src);
    if (dir == NULL) {
      return 1;
    }

    while ((ent = readdir(dir)) != NULL) {
      char child_src[PATH_MAX];
      char child_dst[PATH_MAX];

      if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
        continue;
      }

      snprintf(child_src, sizeof(child_src), "%s/%s", src, ent->d_name);
      snprintf(child_dst, sizeof(child_dst), "%s/%s", dst, ent->d_name);

      if (copy_path_recursive(child_src, child_dst) != 0) {
        closedir(dir);
        return 1;
      }
    }

    closedir(dir);
    return 0;
  }

  if (S_ISREG(st.st_mode)) {
    return copy_file_data(src, dst, st.st_mode & 0777);
  }

  if (S_ISLNK(st.st_mode)) {
    char target[PATH_MAX];
    ssize_t n = readlink(src, target, sizeof(target) - 1);

    if (n < 0) {
      return 1;
    }

    target[n] = '\0';
    if (ensure_parent_dirs(dst, 0755) != 0) {
      return 1;
    }

    unlink(dst);
    return symlink(target, dst);
  }

  return 1;
}

static unsigned long long dir_size_internal(const char *path) {
  struct stat st;

  if (lstat(path, &st) != 0) {
    return 0;
  }

  if (S_ISREG(st.st_mode)) {
    return (unsigned long long)st.st_size;
  }

  if (S_ISDIR(st.st_mode)) {
    DIR *dir = opendir(path);
    struct dirent *ent;
    unsigned long long total = 0;

    if (dir == NULL) {
      return 0;
    }

    while ((ent = readdir(dir)) != NULL) {
      char child[PATH_MAX];
      if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
        continue;
      }
      snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
      total += dir_size_internal(child);
    }

    closedir(dir);
    return total;
  }

  return 0;
}

unsigned long long dir_size_bytes(const char *path) { return dir_size_internal(path); }

int remove_recursive(const char *path) {
  struct stat st;

  if (lstat(path, &st) != 0) {
    return errno == ENOENT ? 0 : 1;
  }

  if (S_ISDIR(st.st_mode)) {
    DIR *dir = opendir(path);
    struct dirent *ent;

    if (dir == NULL) {
      return 1;
    }

    while ((ent = readdir(dir)) != NULL) {
      char child[PATH_MAX];
      if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
        continue;
      }
      snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
      if (remove_recursive(child) != 0) {
        closedir(dir);
        return 1;
      }
    }

    closedir(dir);
    return rmdir(path);
  }

  return unlink(path);
}

int join_paths(const char *a, const char *b, char *out, size_t out_size) {
  if (b == NULL || out == NULL || out_size == 0) {
    return 1;
  }

  if (b[0] == '/') {
    return snprintf(out, out_size, "%s", b) < 0 ? 1 : 0;
  }

  if (a == NULL || a[0] == '\0') {
    return snprintf(out, out_size, "%s", b) < 0 ? 1 : 0;
  }

  if (a[strlen(a) - 1] == '/') {
    return snprintf(out, out_size, "%s%s", a, b) < 0 ? 1 : 0;
  }

  return snprintf(out, out_size, "%s/%s", a, b) < 0 ? 1 : 0;
}

static int normalize_abs_path(const char *input, char *out, size_t out_size) {
  char buf[PATH_MAX];
  char *segments[256];
  size_t seg_count = 0;
  char *saveptr = NULL;
  char *tok;
  size_t i;

  if (input == NULL || input[0] != '/') {
    return 1;
  }

  strncpy(buf, input, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  tok = strtok_r(buf, "/", &saveptr);
  while (tok != NULL) {
    if (strcmp(tok, ".") == 0 || tok[0] == '\0') {
      tok = strtok_r(NULL, "/", &saveptr);
      continue;
    }

    if (strcmp(tok, "..") == 0) {
      if (seg_count > 0) {
        seg_count--;
      }
      tok = strtok_r(NULL, "/", &saveptr);
      continue;
    }

    if (seg_count >= sizeof(segments) / sizeof(segments[0])) {
      return 1;
    }

    segments[seg_count++] = tok;
    tok = strtok_r(NULL, "/", &saveptr);
  }

  if (seg_count == 0) {
    return snprintf(out, out_size, "/") < 0 ? 1 : 0;
  }

  out[0] = '\0';
  strncat(out, "/", out_size - 1);
  for (i = 0; i < seg_count; i++) {
    if (strlen(out) + strlen(segments[i]) + 2 >= out_size) {
      return 1;
    }
    strcat(out, segments[i]);
    if (i + 1 < seg_count) {
      strcat(out, "/");
    }
  }

  return 0;
}

int normalize_container_path(const char *base_workdir, const char *path, char *out,
                             size_t out_size) {
  char raw[PATH_MAX];

  if (path == NULL || out == NULL) {
    return 1;
  }

  if (path[0] == '/') {
    snprintf(raw, sizeof(raw), "%s", path);
  } else {
    const char *wd = (base_workdir != NULL && base_workdir[0] != '\0') ? base_workdir : "/";
    if (wd[strlen(wd) - 1] == '/') {
      snprintf(raw, sizeof(raw), "%s%s", wd, path);
    } else {
      snprintf(raw, sizeof(raw), "%s/%s", wd, path);
    }
  }

  return normalize_abs_path(raw, out, out_size);
}

char *trim_whitespace(char *s) {
  char *end;

  if (s == NULL) {
    return NULL;
  }

  while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') {
    s++;
  }

  if (*s == '\0') {
    return s;
  }

  end = s + strlen(s) - 1;
  while (end > s && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
    *end = '\0';
    end--;
  }

  return s;
}

int starts_with(const char *s, const char *prefix) {
  size_t n;
  if (s == NULL || prefix == NULL) {
    return 0;
  }
  n = strlen(prefix);
  return strncmp(s, prefix, n) == 0;
}

int ends_with(const char *s, const char *suffix) {
  size_t s_len;
  size_t suffix_len;

  if (s == NULL || suffix == NULL) {
    return 0;
  }

  s_len = strlen(s);
  suffix_len = strlen(suffix);
  if (suffix_len > s_len) {
    return 0;
  }

  return strcmp(s + (s_len - suffix_len), suffix) == 0;
}
