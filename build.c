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

static int create_layer(struct stage_ctx *stage, const char *descriptor,
                        const char *instruction_text,
                        int (*apply_fn)(const char *, void *), void *apply_ctx) {
  char new_hash[17];
  char cached_layer_id[64];
  char parent_chain[8192];
  char old_top[64];
  char layer_id[64];
  char layer_root[PATH_MAX];
  char diff_dir[PATH_MAX];
  char work_dir[PATH_MAX];
  char tmp_dir[PATH_MAX];
  char merged[PATH_MAX];
  struct layer_meta meta;
  int rc = 0;

  if (compute_state_hash(stage->state_hash, descriptor, new_hash) != 0) {
    return 1;
  }

  if (lookup_layer_cache(new_hash, cached_layer_id, sizeof(cached_layer_id)) == 0) {
    snprintf(stage->top_layer, sizeof(stage->top_layer), "%s", cached_layer_id);
    snprintf(stage->state_hash, sizeof(stage->state_hash), "%s", new_hash);
    printf("[CACHE HIT] %s\n", instruction_text);
    return 0;
  }

  snprintf(old_top, sizeof(old_top), "%s", stage->top_layer);

  if (resolve_stage_chain(stage, parent_chain, sizeof(parent_chain)) != 0) {
    return 1;
  }

  if (generate_uuid(layer_id) != 0) {
    return 1;
  }

  if (create_layer_dirs(layer_id, parent_chain, layer_root, sizeof(layer_root), diff_dir,
                        sizeof(diff_dir), work_dir, sizeof(work_dir)) != 0) {
    fprintf(stderr, "[ERR] Failed to create layer layout\n");
    remove_recursive(layer_root);
    return 1;
  }

  if (make_temp_dir("build", tmp_dir, sizeof(tmp_dir)) != 0) {
    remove_recursive(layer_root);
    return 1;
  }

  snprintf(merged, sizeof(merged), "%s/merged", tmp_dir);

  if (mkdir(merged, 0755) != 0) {
    remove_recursive(tmp_dir);
    remove_recursive(layer_root);
    return 1;
  }

  if (mount_overlay(parent_chain, diff_dir, work_dir, merged) != 0) {
    fprintf(stderr, "[ERR] Failed to mount build overlay: %s\n", strerror(errno));
    remove_recursive(tmp_dir);
    remove_recursive(layer_root);
    return 1;
  }

  rc = apply_fn(merged, apply_ctx);

  if (umount(merged) != 0) {
    fprintf(stderr, "[WARN] Failed to unmount merged path %s: %s\n", merged,
            strerror(errno));
  }
  remove_recursive(tmp_dir);

  if (rc != 0) {
    remove_recursive(layer_root);
    return 1;
  }

  memset(&meta, 0, sizeof(meta));
  snprintf(meta.id, sizeof(meta.id), "%s", layer_id);
  if (old_top[0] == '\0') {
    snprintf(meta.parent, sizeof(meta.parent), "-");
  } else {
    snprintf(meta.parent, sizeof(meta.parent), "%s", old_top);
  }
  snprintf(meta.hash, sizeof(meta.hash), "%s", new_hash);
  meta.created_at = (long)time(NULL);
  meta.size = dir_size_bytes(diff_dir);
  snprintf(meta.instruction, sizeof(meta.instruction), "%s", instruction_text);
  snprintf(meta.workdir, sizeof(meta.workdir), "%s", stage->workdir);

  if (write_layer_metadata(&meta) != 0) {
    remove_recursive(layer_root);
    return 1;
  }

  if (register_layer_cache(new_hash, layer_id) != 0) {
    remove_recursive(layer_root);
    return 1;
  }

  snprintf(stage->top_layer, sizeof(stage->top_layer), "%s", layer_id);
  snprintf(stage->state_hash, sizeof(stage->state_hash), "%s", new_hash);

  printf("[BUILT] %s\n", instruction_text);
  return 0;
}

static int parse_two_tokens(const char *input, char *first, size_t first_size,
                            char *second, size_t second_size) {
  char tmp[2048];
  char *saveptr = NULL;
  char *t1;
  char *t2;

  snprintf(tmp, sizeof(tmp), "%s", input);
  t1 = strtok_r(tmp, " \t", &saveptr);
  t2 = strtok_r(NULL, " \t", &saveptr);

  if (t1 == NULL || t2 == NULL) {
    return 1;
  }

  snprintf(first, first_size, "%s", t1);
  snprintf(second, second_size, "%s", t2);
  return 0;
}

static int parse_copy_tokens(const char *input, char *from_stage, size_t from_stage_size,
                             char *src, size_t src_size, char *dst, size_t dst_size) {
  char tmp[2048];
  char *saveptr = NULL;
  char *tok;

  from_stage[0] = '\0';
  snprintf(tmp, sizeof(tmp), "%s", input);

  tok = strtok_r(tmp, " \t", &saveptr);
  if (tok == NULL) {
    return 1;
  }

  if (starts_with(tok, "--from=")) {
    snprintf(from_stage, from_stage_size, "%s", tok + 7);
    tok = strtok_r(NULL, " \t", &saveptr);
    if (tok == NULL) return 1;
  }

  snprintf(src, src_size, "%s", tok);
  tok = strtok_r(NULL, " \t", &saveptr);
  if (tok == NULL) {
    return 1;
  }

  snprintf(dst, dst_size, "%s", tok);
  return 0;
}

static int parse_base_and_alias(const char *input, char *base, size_t base_size,
                                char *alias, size_t alias_size) {
  char tmp[2048];
  char *saveptr = NULL;
  char *t1;
  char *t2;
  char *t3;

  alias[0] = '\0';
  snprintf(tmp, sizeof(tmp), "%s", input);
  t1 = strtok_r(tmp, " \t", &saveptr);

  if (t1 == NULL) {
    return 1;
  }

  snprintf(base, base_size, "%s", t1);

  t2 = strtok_r(NULL, " \t", &saveptr);
  t3 = strtok_r(NULL, " \t", &saveptr);

  if (t2 != NULL && t3 != NULL && strcasecmp(t2, "AS") == 0) {
    snprintf(alias, alias_size, "%s", t3);
  }

  return 0;
}

static int stage_index_by_name(struct stage_ctx stages[MAX_STAGES], int stage_count,
                               const char *name_or_index) {
  int i;

  if (name_or_index == NULL || name_or_index[0] == '\0') {
    return -1;
  }

  {
    int all_digits = 1;
    for (i = 0; name_or_index[i] != '\0'; i++) {
      if (!isdigit((unsigned char)name_or_index[i])) {
        all_digits = 0;
        break;
      }
    }

    if (all_digits) {
      int idx = atoi(name_or_index);
      if (idx >= 0 && idx < stage_count) {
        return idx;
      }
    }
  }

  for (i = 0; i < stage_count; i++) {
    if (strcmp(stages[i].name, name_or_index) == 0) {
      return i;
    }
  }

  return -1;
}

static int parse_arg_kv(const char *raw, char *key, size_t key_size, char *value,
                        size_t value_size, int *has_default) {
  const char *eq;
  size_t key_len;

  if (raw == NULL || key == NULL || value == NULL || has_default == NULL) {
    return 1;
  }

  eq = strchr(raw, '=');
  if (eq == NULL) {
    snprintf(key, key_size, "%s", raw);
    value[0] = '\0';
    *has_default = 0;
    return 0;
  }

  key_len = (size_t)(eq - raw);
  if (key_len == 0 || key_len >= key_size) {
    return 1;
  }

  memcpy(key, raw, key_len);
  key[key_len] = '\0';
  snprintf(value, value_size, "%s", eq + 1);
  *has_default = 1;
  return 0;
}

static int ensure_final_stage_has_layer(struct stage_ctx *stage) {
  if (stage->top_layer[0] != '\0') {
    return 0;
  }
  return create_layer(stage, "NOOP|final-stage", "NOOP", apply_noop_layer, NULL);
}

static int get_context_dir(const char *zockerfile_path, char *out, size_t out_size) {
  char tmp[PATH_MAX];
  char *slash;

  snprintf(tmp, sizeof(tmp), "%s", zockerfile_path);
  slash = strrchr(tmp, '/');
  if (slash == NULL) {
    return snprintf(out, out_size, ".") < 0 ? 1 : 0;
  }

  if (slash == tmp) {
    return snprintf(out, out_size, "/") < 0 ? 1 : 0;
  }

  *slash = '\0';
  return snprintf(out, out_size, "%s", tmp) < 0 ? 1 : 0;
}

int build_image_from_config(const struct config *cfg) {
  FILE *fp;
  char line[4096];
  int line_no = 0;
  char context_dir[PATH_MAX];
  struct stage_ctx stages[MAX_STAGES];
  int stage_count = 0;
  int current_stage = -1;
  struct arg_map cli_args;
  struct arg_map global_args;

  memset(stages, 0, sizeof(stages));
  memset(&global_args, 0, sizeof(global_args));

  if (init_cli_args_map(cfg, &cli_args) != 0) {
    return 1;
  }
  arg_map_copy(&global_args, &cli_args);

  if (get_context_dir(cfg->zockerfile, context_dir, sizeof(context_dir)) != 0) {
    return 1;
  }

  fp = fopen(cfg->zockerfile, "r");
  if (fp == NULL) {
    fprintf(stderr, "[ERR] Failed to open Zockerfile: %s\n", cfg->zockerfile);
    return 1;
  }

  while (fgets(line, sizeof(line), fp) != NULL) {
    char original[4096];
    char *trimmed;
    char cmd[32];
    char *rest;
    int i;

    line_no++;
    snprintf(original, sizeof(original), "%s", line);

    trimmed = trim_whitespace(line);
    if (trimmed[0] == '\0' || trimmed[0] == '#') {
      continue;
    }

    for (i = 0; trimmed[i] != '\0' && !isspace((unsigned char)trimmed[i]) &&
                i < (int)sizeof(cmd) - 1;
         i++) {
      cmd[i] = (char)toupper((unsigned char)trimmed[i]);
    }
    cmd[i] = '\0';

    rest = trimmed + i;
    rest = trim_whitespace(rest);

    if (strcmp(cmd, "ARG") == 0) {
      char key[64];
      char default_val[256];
      char resolved_default[256];
      int has_default = 0;
      const char *cli_val;
      const char *current_val;
      const char *final_val;

      if (parse_arg_kv(rest, key, sizeof(key), default_val, sizeof(default_val),
                       &has_default) != 0) {
        fprintf(stderr, "[ERR] Invalid ARG at line %d\n", line_no);
        fclose(fp);
        return 1;
      }

      if (has_default) {
        if (current_stage >= 0) {
          if (substitute_args(default_val, &stages[current_stage].args, resolved_default,
                              sizeof(resolved_default)) != 0) {
            fclose(fp);
            return 1;
          }
        } else {
          if (substitute_args(default_val, &global_args, resolved_default,
                              sizeof(resolved_default)) != 0) {
            fclose(fp);
            return 1;
          }
        }
      } else {
        resolved_default[0] = '\0';
      }

      cli_val = arg_map_get(&cli_args, key);
      if (cli_val != NULL) {
        final_val = cli_val;
      } else {
        if (current_stage >= 0) {
          current_val = arg_map_get(&stages[current_stage].args, key);
        } else {
          current_val = arg_map_get(&global_args, key);
        }

        if (has_default) {
          final_val = resolved_default;
        } else if (current_val != NULL) {
          final_val = current_val;
        } else {
          final_val = "";
        }
      }

      if (current_stage >= 0) {
        if (arg_map_set(&stages[current_stage].args, key, final_val) != 0) {
          fclose(fp);
          return 1;
        }
      } else {
        if (arg_map_set(&global_args, key, final_val) != 0) {
          fclose(fp);
          return 1;
        }
      }

      continue;
    }

    if (strcmp(cmd, "FROM") == 0 || strcmp(cmd, "BASEDIR") == 0) {
      struct stage_ctx *stage;
      char base_raw[1024];
      char base_value[1024];
      char alias[64];
      char seed[9000];

      if (stage_count >= MAX_STAGES) {
        fprintf(stderr, "[ERR] Too many stages\n");
        fclose(fp);
        return 1;
      }

      if (parse_base_and_alias(rest, base_raw, sizeof(base_raw), alias,
                               sizeof(alias)) != 0) {
        fprintf(stderr, "[ERR] Invalid %s at line %d\n", cmd, line_no);
        fclose(fp);
        return 1;
      }

      if (substitute_args(base_raw, &global_args, base_value, sizeof(base_value)) != 0) {
        fclose(fp);
        return 1;
      }

      stage = &stages[stage_count];
      memset(stage, 0, sizeof(*stage));
      arg_map_copy(&stage->args, &global_args);
      snprintf(stage->workdir, sizeof(stage->workdir), "/");

      if (alias[0] != '\0') {
        snprintf(stage->name, sizeof(stage->name), "%s", alias);
      } else {
        snprintf(stage->name, sizeof(stage->name), "%d", stage_count);
      }

      if (strcmp(cmd, "FROM") == 0) {
        if (resolve_base_chain(base_value, stage->base_chain, sizeof(stage->base_chain)) !=
            0) {
          fprintf(stderr, "[ERR] Failed to resolve FROM at line %d: %s\n", line_no,
                  base_value);
          fclose(fp);
          return 1;
        }
      } else {
        char resolved_base[PATH_MAX];

        if (base_value[0] == '/') {
          snprintf(resolved_base, sizeof(resolved_base), "%s", base_value);
        } else {
          snprintf(resolved_base, sizeof(resolved_base), "%s/%s", context_dir,
                   base_value);
        }

        if (!is_directory(resolved_base)) {
          fprintf(stderr, "[ERR] BASEDIR path is not a directory (line %d): %s\n",
                  line_no, resolved_base);
          fclose(fp);
          return 1;
        }

        snprintf(stage->base_chain, sizeof(stage->base_chain), "%s", resolved_base);
      }

      snprintf(seed, sizeof(seed), "BASE|%s", stage->base_chain);
      if (hash_string(seed, stage->state_hash) != 0) {
        fclose(fp);
        return 1;
      }

      current_stage = stage_count;
      stage_count++;
      continue;
    }

    if (current_stage < 0) {
      fprintf(stderr, "[ERR] %s used before FROM/BASEDIR at line %d\n", cmd, line_no);
      fclose(fp);
      return 1;
    }

    {
      struct stage_ctx *stage = &stages[current_stage];

      if (strcmp(cmd, "RUN") == 0) {
        struct run_apply_ctx run_ctx;
        char command[1024];
        char descriptor[4096];
        char instruction[1200];

        if (substitute_args(rest, &stage->args, command, sizeof(command)) != 0) {
          fclose(fp);
          return 1;
        }

        snprintf(descriptor, sizeof(descriptor), "RUN|wd=%s|cmd=%s", stage->workdir,
                 command);
        snprintf(instruction, sizeof(instruction), "RUN %s", command);

        snprintf(run_ctx.command, sizeof(run_ctx.command), "%s", command);
        snprintf(run_ctx.workdir, sizeof(run_ctx.workdir), "%s", stage->workdir);

        if (create_layer(stage, descriptor, instruction, apply_run_layer, &run_ctx) != 0) {
          fprintf(stderr, "[ERR] Failed at line %d: %s", line_no, original);
          fclose(fp);
          return 1;
        }

        continue;
      }

      if (strcmp(cmd, "WORKDIR") == 0) {
        struct workdir_apply_ctx wd_ctx;
        char path_arg[512];
        char new_workdir[512];
        char descriptor[2048];
        char instruction[768];

        if (substitute_args(rest, &stage->args, path_arg, sizeof(path_arg)) != 0) {
          fclose(fp);
          return 1;
        }

        if (normalize_container_path(stage->workdir, path_arg, new_workdir,
                                     sizeof(new_workdir)) != 0) {
          fclose(fp);
          return 1;
        }

        snprintf(descriptor, sizeof(descriptor), "WORKDIR|path=%s", new_workdir);
        snprintf(instruction, sizeof(instruction), "WORKDIR %s", new_workdir);
        snprintf(wd_ctx.path, sizeof(wd_ctx.path), "%s", new_workdir);

        if (create_layer(stage, descriptor, instruction, apply_workdir_layer, &wd_ctx) !=
            0) {
          fprintf(stderr, "[ERR] Failed at line %d: %s", line_no, original);
          fclose(fp);
          return 1;
        }

        snprintf(stage->workdir, sizeof(stage->workdir), "%s", new_workdir);
        continue;
      }

      if (strcmp(cmd, "COPY") == 0) {
        struct copy_apply_ctx copy_ctx;
        char substituted[2048];
        char from_stage[64];
        char src[PATH_MAX];
        char dst[512];
        char dst_abs[512];
        char descriptor[8192];
        char instruction[2300];

        memset(&copy_ctx, 0, sizeof(copy_ctx));

        if (substitute_args(rest, &stage->args, substituted, sizeof(substituted)) != 0) {
          fclose(fp);
          return 1;
        }

        if (parse_copy_tokens(substituted, from_stage, sizeof(from_stage), src,
                              sizeof(src), dst, sizeof(dst)) != 0) {
          fprintf(stderr, "[ERR] Invalid COPY at line %d\n", line_no);
          fclose(fp);
          return 1;
        }

        if (normalize_container_path(stage->workdir, dst, dst_abs, sizeof(dst_abs)) != 0) {
          fclose(fp);
          return 1;
        }

        if (from_stage[0] != '\0') {
          int from_idx = stage_index_by_name(stages, stage_count, from_stage);
          if (from_idx < 0) {
            fprintf(stderr, "[ERR] COPY --from stage not found at line %d: %s\n", line_no,
                    from_stage);
            fclose(fp);
            return 1;
          }

          copy_ctx.from_stage = 1;
          copy_ctx.source_stage = &stages[from_idx];

          snprintf(descriptor, sizeof(descriptor),
                   "COPY|from=%s|src=%s|src_state=%s|dst=%s", from_stage, src,
                   stages[from_idx].state_hash, dst_abs);
          snprintf(instruction, sizeof(instruction), "COPY --from=%s %s %s", from_stage,
                   src, dst);
        } else {
          char src_host[PATH_MAX];
          char src_hash[17];

          if (src[0] == '/') {
            snprintf(src_host, sizeof(src_host), "%s", src);
          } else {
            snprintf(src_host, sizeof(src_host), "%s/%s", context_dir, src);
          }

          if (hash_path_recursive(src_host, src_hash) != 0) {
            fprintf(stderr, "[ERR] COPY source not found/unreadable at line %d: %s\n",
                    line_no, src_host);
            fclose(fp);
            return 1;
          }

          snprintf(descriptor, sizeof(descriptor), "COPY|src=%s|src_hash=%s|dst=%s",
                   src_host, src_hash, dst_abs);
          snprintf(instruction, sizeof(instruction), "COPY %s %s", src, dst);
        }

        snprintf(copy_ctx.source, sizeof(copy_ctx.source), "%s", src);
        snprintf(copy_ctx.destination, sizeof(copy_ctx.destination), "%s", dst);
        snprintf(copy_ctx.workdir, sizeof(copy_ctx.workdir), "%s", stage->workdir);
        snprintf(copy_ctx.context_dir, sizeof(copy_ctx.context_dir), "%s", context_dir);

        if (create_layer(stage, descriptor, instruction, apply_copy_layer, &copy_ctx) != 0) {
          fprintf(stderr, "[ERR] Failed at line %d: %s", line_no, original);
          fclose(fp);
          return 1;
        }

        continue;
      }

      if (strcmp(cmd, "ADD") == 0) {
        struct add_apply_ctx add_ctx;
        char substituted[2048];
        char src[PATH_MAX];
        char dst[512];
        char dst_abs[512];
        char descriptor[8192];
        char instruction[2300];

        memset(&add_ctx, 0, sizeof(add_ctx));

        if (substitute_args(rest, &stage->args, substituted, sizeof(substituted)) != 0) {
          fclose(fp);
          return 1;
        }

        if (parse_two_tokens(substituted, src, sizeof(src), dst, sizeof(dst)) != 0) {
          fprintf(stderr, "[ERR] Invalid ADD at line %d\n", line_no);
          fclose(fp);
          return 1;
        }

        if (normalize_container_path(stage->workdir, dst, dst_abs, sizeof(dst_abs)) != 0) {
          fclose(fp);
          return 1;
        }

        add_ctx.is_url = starts_with(src, "http://") || starts_with(src, "https://");

        if (add_ctx.is_url) {
          snprintf(descriptor, sizeof(descriptor), "ADD|url=%s|dst=%s", src, dst_abs);
        } else {
          char src_host[PATH_MAX];
          char src_hash[17];

          if (src[0] == '/') {
            snprintf(src_host, sizeof(src_host), "%s", src);
          } else {
            snprintf(src_host, sizeof(src_host), "%s/%s", context_dir, src);
          }

          if (hash_path_recursive(src_host, src_hash) != 0) {
            fprintf(stderr, "[ERR] ADD source not found/unreadable at line %d: %s\n",
                    line_no, src_host);
            fclose(fp);
            return 1;
          }

          snprintf(descriptor, sizeof(descriptor), "ADD|src=%s|src_hash=%s|dst=%s",
                   src_host, src_hash, dst_abs);
        }

        snprintf(instruction, sizeof(instruction), "ADD %s %s", src, dst);

        snprintf(add_ctx.source, sizeof(add_ctx.source), "%s", src);
        snprintf(add_ctx.destination, sizeof(add_ctx.destination), "%s", dst);
        snprintf(add_ctx.workdir, sizeof(add_ctx.workdir), "%s", stage->workdir);
        snprintf(add_ctx.context_dir, sizeof(add_ctx.context_dir), "%s", context_dir);

        if (create_layer(stage, descriptor, instruction, apply_add_layer, &add_ctx) != 0) {
          fprintf(stderr, "[ERR] Failed at line %d: %s", line_no, original);
          fclose(fp);
          return 1;
        }

        continue;
      }

      if (strcmp(cmd, "CMD") == 0) {
        char cmd_value[1024];
        if (substitute_args(rest, &stage->args, cmd_value, sizeof(cmd_value)) != 0) {
          fclose(fp);
          return 1;
        }
        snprintf(stage->cmd, sizeof(stage->cmd), "%s", cmd_value);
        continue;
      }

      fprintf(stderr, "[ERR] Unsupported instruction at line %d: %s\n", line_no, cmd);
      fclose(fp);
      return 1;
    }
  }

  fclose(fp);

  if (stage_count == 0) {
    fprintf(stderr, "[ERR] Zockerfile has no FROM/BASEDIR\n");
    return 1;
  }

  {
    struct stage_ctx *final_stage = &stages[stage_count - 1];
    struct image_meta image;

    if (ensure_final_stage_has_layer(final_stage) != 0) {
      return 1;
    }

    memset(&image, 0, sizeof(image));
    if (parse_image_ref(cfg->image_ref, image.name, sizeof(image.name), image.tag,
                        sizeof(image.tag)) != 0) {
      fprintf(stderr, "[ERR] Invalid image reference: %s\n", cfg->image_ref);
      return 1;
    }

    snprintf(image.ref, sizeof(image.ref), "%s:%s", image.name, image.tag);
    snprintf(image.top_layer, sizeof(image.top_layer), "%s", final_stage->top_layer);
    snprintf(image.cmd, sizeof(image.cmd), "%s", final_stage->cmd);

    if (save_image_meta(&image) != 0) {
      fprintf(stderr, "[ERR] Failed to save image metadata\n");
      return 1;
    }

    printf("Successfully built image %s (top layer: %s)\n", image.ref,
           image.top_layer);
  }

  return 0;
}
