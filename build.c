#define _GNU_SOURCE

#include "build.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "image_store.h"
#include "setup.h"
#include "utils.h"

#define MAX_STAGES 32
#define MAX_LOCAL_ARGS 128

struct arg_map {
  struct build_arg items[MAX_LOCAL_ARGS];
  int count;
};

struct stage_ctx {
  char name[64];
  char base_chain[8192];
  char top_layer[64];
  char state_hash[17];
  char workdir[512];
  struct arg_map args;
  char cmd[1024];
};

static int arg_map_set(struct arg_map *map, const char *key, const char *value) {
  int i;

  if (map == NULL || key == NULL || value == NULL || key[0] == '\0') {
    return 1;
  }

  for (i = 0; i < map->count; i++) {
    if (strcmp(map->items[i].key, key) == 0) {
      snprintf(map->items[i].value, sizeof(map->items[i].value), "%s", value);
      return 0;
    }
  }

  if (map->count >= MAX_LOCAL_ARGS) {
    return 1;
  }

  snprintf(map->items[map->count].key, sizeof(map->items[map->count].key), "%s",
           key);
  snprintf(map->items[map->count].value,
           sizeof(map->items[map->count].value), "%s", value);
  map->count++;
  return 0;
}

static const char *arg_map_get(const struct arg_map *map, const char *key) {
  int i;

  if (map == NULL || key == NULL) {
    return NULL;
  }

  for (i = 0; i < map->count; i++) {
    if (strcmp(map->items[i].key, key) == 0) {
      return map->items[i].value;
    }
  }

  return NULL;
}

static void arg_map_copy(struct arg_map *dst, const struct arg_map *src) {
  if (dst == NULL || src == NULL) {
    return;
  }
  memset(dst, 0, sizeof(*dst));
  memcpy(dst, src, sizeof(*dst));
}

static int init_cli_args_map(const struct config *cfg, struct arg_map *map) {
  int i;

  memset(map, 0, sizeof(*map));
  for (i = 0; i < cfg->build_arg_count; i++) {
    if (arg_map_set(map, cfg->build_args[i].key, cfg->build_args[i].value) != 0) {
      return 1;
    }
  }
  return 0;
}

static int substitute_args(const char *input, const struct arg_map *args, char *out,
                           size_t out_size) {
  size_t i = 0;
  size_t w = 0;

  if (input == NULL || out == NULL || out_size == 0) {
    return 1;
  }

  while (input[i] != '\0') {
    if (input[i] == '$') {
      char key[128] = {0};
      size_t k = 0;
      const char *value;
      size_t value_len;

      if (input[i + 1] == '$') {
        if (w + 1 >= out_size) return 1;
        out[w++] = '$';
        i += 2;
        continue;
      }

      if (input[i + 1] == '{') {
        i += 2;
        while (input[i] != '\0' && input[i] != '}' && k + 1 < sizeof(key)) {
          key[k++] = input[i++];
        }
        if (input[i] == '}') {
          i++;
        }
      } else {
        i++;
        while (input[i] != '\0' &&
               (isalnum((unsigned char)input[i]) || input[i] == '_') &&
               k + 1 < sizeof(key)) {
          key[k++] = input[i++];
        }

        if (k == 0) {
          if (w + 1 >= out_size) return 1;
          out[w++] = '$';
          continue;
        }
      }

      key[k] = '\0';
      value = arg_map_get(args, key);
      if (value == NULL) {
        value = "";
      }

      value_len = strlen(value);
      if (w + value_len >= out_size) {
        return 1;
      }

      memcpy(out + w, value, value_len);
      w += value_len;
      continue;
    }

    if (w + 1 >= out_size) {
      return 1;
    }

    out[w++] = input[i++];
  }

  out[w] = '\0';
  return 0;
}

static int ensure_dir_path(const char *path, mode_t mode) {
  if (ensure_parent_dirs(path, mode) != 0) {
    return 1;
  }

  if (ensure_dir_exists(path, mode) != 0 && !is_directory(path)) {
    return 1;
  }

  return 0;
}

static int make_temp_dir(const char *prefix, char *out, size_t out_size) {
  char uuid[64];

  if (generate_uuid(uuid) != 0) {
    return 1;
  }

  if (snprintf(out, out_size, "%s/%s_%d_%.8s", ZOCKER_BUILD_TMP_DIR, prefix,
               getpid(), uuid) < 0) {
    return 1;
  }

  return mkdir(out, 0755);
}

static int compute_state_hash(const char *parent_hash, const char *descriptor,
                              char out_hash[17]) {
  char raw[16384];

  if (snprintf(raw, sizeof(raw), "%s|%s", parent_hash, descriptor) < 0) {
    return 1;
  }

  return hash_string(raw, out_hash);
}

static int resolve_stage_chain(const struct stage_ctx *stage, char *out_chain,
                               size_t out_size) {
  if (stage == NULL || out_chain == NULL) {
    return 1;
  }

  if (stage->top_layer[0] != '\0') {
    return layer_chain_from_top(stage->top_layer, out_chain, out_size);
  }

  return snprintf(out_chain, out_size, "%s", stage->base_chain) < 0 ? 1 : 0;
}

static int run_in_chroot(const char *rootfs, const char *workdir, const char *command) {
  pid_t pid;
  int status;
  char host_workdir[PATH_MAX];
  char shell_path[PATH_MAX];

  snprintf(host_workdir, sizeof(host_workdir), "%s%s", rootfs, workdir);
  if (ensure_dir_path(host_workdir, 0755) != 0) {
    fprintf(stderr, "[ERR] Failed to prepare WORKDIR: %s\n", host_workdir);
    return 1;
  }

  snprintf(shell_path, sizeof(shell_path), "%s/bin/sh", rootfs);
  if (access(shell_path, X_OK) != 0) {
    fprintf(stderr,
            "[ERR] RUN requires executable /bin/sh inside base rootfs. Missing: %s (%s)\n",
            shell_path, strerror(errno));
    return 1;
  }

  pid = fork();
  if (pid < 0) {
    return 1;
  }

  if (pid == 0) {
    if (chroot(rootfs) != 0) {
      fprintf(stderr, "[ERR] build RUN chroot failed: %s\n", strerror(errno));
      _exit(127);
    }

    if (chdir(workdir) != 0) {
      fprintf(stderr, "[ERR] build RUN chdir failed: %s\n", strerror(errno));
      _exit(127);
    }

    execl("/bin/sh", "sh", "-c", command, NULL);
    if (errno == ENOENT) {
      fprintf(stderr,
              "[ERR] build RUN cannot execute /bin/sh inside rootfs. Usually missing dynamic loader/libs.\n");
    }
    fprintf(stderr, "[ERR] build RUN command failed: %s\n", strerror(errno));
    _exit(127);
  }

  if (waitpid(pid, &status, 0) < 0) {
    return 1;
  }

  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    fprintf(stderr, "[ERR] RUN failed with status=%d\n", WEXITSTATUS(status));
    return 1;
  }

  return 0;
}

static const char *basename_of(const char *path) {
  const char *slash;
  if (path == NULL) {
    return "";
  }
  slash = strrchr(path, '/');
  return slash == NULL ? path : slash + 1;
}

static int copy_into_rootfs(const char *merged_root, const char *src_host_path,
                            const char *dst_in_container,
                            const char *current_workdir) {
  char dst_abs[PATH_MAX];
  char dst_host[PATH_MAX];
  struct stat src_st;
  int dest_is_dir = 0;

  if (normalize_container_path(current_workdir, dst_in_container, dst_abs,
                               sizeof(dst_abs)) != 0) {
    return 1;
  }

  snprintf(dst_host, sizeof(dst_host), "%s%s", merged_root, dst_abs);

  if (lstat(src_host_path, &src_st) != 0) {
    fprintf(stderr, "[ERR] COPY/ADD source not found: %s\n", src_host_path);
    return 1;
  }

  if (dst_in_container[strlen(dst_in_container) - 1] == '/' || is_directory(dst_host)) {
    dest_is_dir = 1;
  }

  if (dest_is_dir) {
    char target[PATH_MAX];
    if (ensure_dir_path(dst_host, 0755) != 0) {
      return 1;
    }
    snprintf(target, sizeof(target), "%s/%s", dst_host, basename_of(src_host_path));
    return copy_path_recursive(src_host_path, target);
  }

  return copy_path_recursive(src_host_path, dst_host);
}

static int mount_overlay(const char *lower_chain, const char *upper, const char *work,
                         const char *merged) {
  char lower_copy[16384];
  char *saveptr = NULL;
  char *token;
  char upper_real[PATH_MAX];
  char work_real[PATH_MAX];
  char mount_opts[16384];

  if (realpath(upper, upper_real) == NULL || realpath(work, work_real) == NULL) {
    fprintf(stderr, "[ERR] Failed to resolve overlay upper/work paths: %s\n",
            strerror(errno));
    return -1;
  }

  snprintf(lower_copy, sizeof(lower_copy), "%s", lower_chain);
  token = strtok_r(lower_copy, ":", &saveptr);
  while (token != NULL) {
    char lower_real[PATH_MAX];
    size_t n;

    if (realpath(token, lower_real) == NULL) {
      fprintf(stderr, "[ERR] Failed to resolve lowerdir path: %s (%s)\n", token,
              strerror(errno));
      return -1;
    }

    n = strlen(lower_real);

    if ((strncmp(upper_real, lower_real, n) == 0 &&
         (upper_real[n] == '\0' || upper_real[n] == '/' ||
          (n == 1 && lower_real[0] == '/'))) ||
        (strncmp(work_real, lower_real, n) == 0 &&
         (work_real[n] == '\0' || work_real[n] == '/' ||
          (n == 1 && lower_real[0] == '/')))) {
      fprintf(stderr,
              "[ERR] Invalid overlay configuration: upper/work is inside lowerdir (%s).\n",
              lower_real);
      return -1;
    }

    token = strtok_r(NULL, ":", &saveptr);
  }

  snprintf(mount_opts, sizeof(mount_opts), "lowerdir=%s,upperdir=%s,workdir=%s",
           lower_chain, upper, work);
  return mount("overlay", merged, "overlay", 0, mount_opts);
}

static int create_layer_dirs(const char *layer_id, const char *lower_chain,
                             char *layer_root, size_t layer_root_size,
                             char *diff_dir, size_t diff_dir_size,
                             char *work_dir, size_t work_dir_size) {
  char lower_path[PATH_MAX];
  char link_path[PATH_MAX];
  char short_id[64];
  char symlink_path[PATH_MAX];
  char symlink_target[PATH_MAX];
  size_t i;
  size_t w = 0;
  FILE *fp;

  snprintf(layer_root, layer_root_size, "%s/%s", ZOCKER_LAYERS_DIR, layer_id);
  snprintf(diff_dir, diff_dir_size, "%s/diff", layer_root);
  snprintf(work_dir, work_dir_size, "%s/work", layer_root);
  snprintf(lower_path, sizeof(lower_path), "%s/lower", layer_root);
  snprintf(link_path, sizeof(link_path), "%s/link", layer_root);

  if (mkdir(layer_root, 0755) != 0) {
    return 1;
  }

  if (mkdir(diff_dir, 0755) != 0 || mkdir(work_dir, 0755) != 0) {
    return 1;
  }

  fp = fopen(lower_path, "w");
  if (fp == NULL) {
    return 1;
  }
  fprintf(fp, "%s\n", lower_chain);
  fclose(fp);

  for (i = 0; layer_id[i] != '\0' && w + 1 < sizeof(short_id); i++) {
    if (layer_id[i] != '-') {
      short_id[w++] = layer_id[i];
      if (w >= 26) break;
    }
  }
  short_id[w] = '\0';

  fp = fopen(link_path, "w");
  if (fp == NULL) {
    return 1;
  }
  fprintf(fp, "%s\n", short_id);
  fclose(fp);

  snprintf(symlink_path, sizeof(symlink_path), "%s/%s", ZOCKER_LAYER_LINKS_DIR,
           short_id);
  snprintf(symlink_target, sizeof(symlink_target), "../%s/diff", layer_id);
  unlink(symlink_path);
  if (symlink(symlink_target, symlink_path) != 0) {
    return 1;
  }

  return 0;
}

static int with_stage_snapshot(const struct stage_ctx *stage, char *merged_out,
                               size_t merged_out_size, char *tmp_dir_out,
                               size_t tmp_dir_out_size) {
  char chain[8192];
  char upper[PATH_MAX];
  char work[PATH_MAX];

  if (resolve_stage_chain(stage, chain, sizeof(chain)) != 0) {
    return 1;
  }

  if (make_temp_dir("snapshot", tmp_dir_out, tmp_dir_out_size) != 0) {
    return 1;
  }

  snprintf(upper, sizeof(upper), "%s/upper", tmp_dir_out);
  snprintf(work, sizeof(work), "%s/work", tmp_dir_out);
  snprintf(merged_out, merged_out_size, "%s/merged", tmp_dir_out);

  if (mkdir(upper, 0755) != 0 || mkdir(work, 0755) != 0 || mkdir(merged_out, 0755) != 0) {
    return 1;
  }

  if (mount_overlay(chain, upper, work, merged_out) != 0) {
    fprintf(stderr, "[ERR] Failed to mount source stage snapshot: %s\n",
            strerror(errno));
    return 1;
  }

  return 0;
}

struct run_apply_ctx {
  char command[1024];
  char workdir[512];
};

static int apply_run_layer(const char *merged, void *ctx_ptr) {
  struct run_apply_ctx *ctx = (struct run_apply_ctx *)ctx_ptr;
  return run_in_chroot(merged, ctx->workdir, ctx->command);
}

struct workdir_apply_ctx {
  char path[512];
};

static int apply_workdir_layer(const char *merged, void *ctx_ptr) {
  struct workdir_apply_ctx *ctx = (struct workdir_apply_ctx *)ctx_ptr;
  char host_path[PATH_MAX];
  snprintf(host_path, sizeof(host_path), "%s%s", merged, ctx->path);
  return ensure_dir_path(host_path, 0755);
}

struct copy_apply_ctx {
  int from_stage;
  const struct stage_ctx *source_stage;
  char source[PATH_MAX];
  char destination[512];
  char workdir[512];
  char context_dir[PATH_MAX];
};

static int apply_copy_layer(const char *merged, void *ctx_ptr) {
  struct copy_apply_ctx *ctx = (struct copy_apply_ctx *)ctx_ptr;

  if (ctx->from_stage) {
    char snapshot_merged[PATH_MAX];
    char snapshot_tmp[PATH_MAX];
    char source_abs[512];
    char host_source[PATH_MAX];
    int rc;

    if (with_stage_snapshot(ctx->source_stage, snapshot_merged, sizeof(snapshot_merged),
                            snapshot_tmp, sizeof(snapshot_tmp)) != 0) {
      return 1;
    }

    if (normalize_container_path("/", ctx->source, source_abs, sizeof(source_abs)) != 0) {
      umount(snapshot_merged);
      remove_recursive(snapshot_tmp);
      return 1;
    }

    snprintf(host_source, sizeof(host_source), "%s%s", snapshot_merged, source_abs);
    rc = copy_into_rootfs(merged, host_source, ctx->destination, ctx->workdir);
    umount(snapshot_merged);
    remove_recursive(snapshot_tmp);
    return rc;
  }

  {
    char src_host[PATH_MAX];

    if (ctx->source[0] == '/') {
      snprintf(src_host, sizeof(src_host), "%s", ctx->source);
    } else {
      snprintf(src_host, sizeof(src_host), "%s/%s", ctx->context_dir, ctx->source);
    }

    return copy_into_rootfs(merged, src_host, ctx->destination, ctx->workdir);
  }
}

struct add_apply_ctx {
  char source[PATH_MAX];
  char destination[512];
  char workdir[512];
  char context_dir[PATH_MAX];
  int is_url;
};

static int download_url_to_file(const char *url, const char *dest) {
  pid_t pid;
  int status;

  pid = fork();
  if (pid < 0) {
    return 1;
  }

  if (pid == 0) {
    execlp("curl", "curl", "-fsSL", url, "-o", dest, NULL);
    _exit(127);
  }

  if (waitpid(pid, &status, 0) < 0) {
    return 1;
  }

  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    return 1;
  }

  return 0;
}

static int apply_add_layer(const char *merged, void *ctx_ptr) {
  struct add_apply_ctx *ctx = (struct add_apply_ctx *)ctx_ptr;

  if (ctx->is_url) {
    char tmp_dir[PATH_MAX];
    char tmp_file[PATH_MAX];
    int rc;

    if (make_temp_dir("add", tmp_dir, sizeof(tmp_dir)) != 0) {
      return 1;
    }

    snprintf(tmp_file, sizeof(tmp_file), "%s/download.bin", tmp_dir);
    if (download_url_to_file(ctx->source, tmp_file) != 0) {
      remove_recursive(tmp_dir);
      return 1;
    }

    rc = copy_into_rootfs(merged, tmp_file, ctx->destination, ctx->workdir);
    remove_recursive(tmp_dir);
    return rc;
  }

  {
    char src_host[PATH_MAX];

    if (ctx->source[0] == '/') {
      snprintf(src_host, sizeof(src_host), "%s", ctx->source);
    } else {
      snprintf(src_host, sizeof(src_host), "%s/%s", ctx->context_dir, ctx->source);
    }

    return copy_into_rootfs(merged, src_host, ctx->destination, ctx->workdir);
  }
}

static int apply_noop_layer(const char *merged, void *ctx_ptr) {
  (void)merged;
  (void)ctx_ptr;
  return 0;
}