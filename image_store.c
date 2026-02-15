#define _GNU_SOURCE

#include "image_store.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "setup.h"
#include "utils.h"

static int sanitize_component(const char *src, char *dst, size_t dst_size) {
  size_t i;
  size_t w = 0;

  if (src == NULL || dst == NULL || dst_size == 0) {
    return 1;
  }

  for (i = 0; src[i] != '\0' && w + 1 < dst_size; i++) {
    unsigned char c = (unsigned char)src[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.') {
      dst[w++] = (char)c;
    } else {
      dst[w++] = '_';
    }
  }

  if (src[i] != '\0') {
    return 1;
  }

  dst[w] = '\0';
  return 0;
}

int parse_image_ref(const char *ref, char *name, size_t name_size, char *tag,
                    size_t tag_size) {
  const char *last_colon;
  const char *last_slash;
  size_t name_len;

  if (ref == NULL || name == NULL || tag == NULL) {
    return 1;
  }

  if (ref[0] == '\0') {
    return 1;
  }

  last_colon = strrchr(ref, ':');
  last_slash = strrchr(ref, '/');

  if (last_colon != NULL && (last_slash == NULL || last_colon > last_slash)) {
    name_len = (size_t)(last_colon - ref);
    if (name_len == 0 || name_len >= name_size) {
      return 1;
    }

    memcpy(name, ref, name_len);
    name[name_len] = '\0';

    if (snprintf(tag, tag_size, "%s", last_colon + 1) < 0) {
      return 1;
    }
    if (tag[0] == '\0') {
      return 1;
    }
    return 0;
  }

  if (snprintf(name, name_size, "%s", ref) < 0) {
    return 1;
  }
  if (name[0] == '\0') {
    return 1;
  }

  if (snprintf(tag, tag_size, "latest") < 0) {
    return 1;
  }

  return 0;
}

static int image_meta_path_from_ref(const char *ref, char *path, size_t path_size) {
  char name[192];
  char tag[64];
  char safe_name[256];
  char safe_tag[128];

  if (parse_image_ref(ref, name, sizeof(name), tag, sizeof(tag)) != 0) {
    return 1;
  }

  if (sanitize_component(name, safe_name, sizeof(safe_name)) != 0) {
    return 1;
  }

  if (sanitize_component(tag, safe_tag, sizeof(safe_tag)) != 0) {
    return 1;
  }

  return snprintf(path, path_size, "%s/%s__%s.meta", ZOCKER_IMAGES_DIR, safe_name,
                  safe_tag) < 0
             ? 1
             : 0;
}

static int load_image_meta_from_path(const char *path, struct image_meta *meta) {
  FILE *fp;
  char line[2048];

  if (path == NULL || meta == NULL) {
    return 1;
  }

  memset(meta, 0, sizeof(*meta));

  fp = fopen(path, "r");
  if (fp == NULL) {
    return 1;
  }

  while (fgets(line, sizeof(line), fp) != NULL) {
    char *eq = strchr(line, '=');
    char *key;
    char *value;

    if (eq == NULL) {
      continue;
    }

    *eq = '\0';
    key = trim_whitespace(line);
    value = trim_whitespace(eq + 1);
    value[strcspn(value, "\r\n")] = '\0';

    if (strcmp(key, "name") == 0) {
      snprintf(meta->name, sizeof(meta->name), "%s", value);
    } else if (strcmp(key, "tag") == 0) {
      snprintf(meta->tag, sizeof(meta->tag), "%s", value);
    } else if (strcmp(key, "ref") == 0) {
      snprintf(meta->ref, sizeof(meta->ref), "%s", value);
    } else if (strcmp(key, "top_layer") == 0) {
      snprintf(meta->top_layer, sizeof(meta->top_layer), "%s", value);
    } else if (strcmp(key, "created_at") == 0) {
      snprintf(meta->created_at, sizeof(meta->created_at), "%s", value);
    } else if (strcmp(key, "cmd") == 0) {
      snprintf(meta->cmd, sizeof(meta->cmd), "%s", value);
    }
  }

  fclose(fp);
  return 0;
}

int save_image_meta(const struct image_meta *meta) {
  FILE *fp;
  char path[PATH_MAX];
  char name[192];
  char tag[64];
  char ref[256];
  char created_at[64];

  if (meta == NULL) {
    return 1;
  }

  if (meta->ref[0] != '\0') {
    snprintf(ref, sizeof(ref), "%s", meta->ref);
  } else {
    if (meta->name[0] == '\0') {
      return 1;
    }
    if (meta->tag[0] == '\0') {
      snprintf(ref, sizeof(ref), "%s:latest", meta->name);
    } else {
      snprintf(ref, sizeof(ref), "%s:%s", meta->name, meta->tag);
    }
  }

  if (parse_image_ref(ref, name, sizeof(name), tag, sizeof(tag)) != 0) {
    return 1;
  }

  if (image_meta_path_from_ref(ref, path, sizeof(path)) != 0) {
    return 1;
  }

  if (meta->created_at[0] == '\0') {
    snprintf(created_at, sizeof(created_at), "%ld", (long)time(NULL));
  } else {
    snprintf(created_at, sizeof(created_at), "%s", meta->created_at);
  }

  fp = fopen(path, "w");
  if (fp == NULL) {
    return 1;
  }

  fprintf(fp, "name=%s\n", name);
  fprintf(fp, "tag=%s\n", tag);
  fprintf(fp, "ref=%s:%s\n", name, tag);
  fprintf(fp, "top_layer=%s\n", meta->top_layer);
  fprintf(fp, "created_at=%s\n", created_at);
  fprintf(fp, "cmd=%s\n", meta->cmd);

  fclose(fp);
  return 0;
}

int load_image_meta(const char *ref, struct image_meta *meta) {
  char path[PATH_MAX];

  if (image_meta_path_from_ref(ref, path, sizeof(path)) != 0) {
    return 1;
  }

  return load_image_meta_from_path(path, meta);
}

int image_exists(const char *ref) {
  char path[PATH_MAX];
  if (image_meta_path_from_ref(ref, path, sizeof(path)) != 0) {
    return 0;
  }
  return path_exists(path);
}

int layer_exists(const char *layer_id) {
  char path[PATH_MAX];
  if (layer_id == NULL || layer_id[0] == '\0') {
    return 0;
  }
  snprintf(path, sizeof(path), "%s/%s", ZOCKER_LAYERS_DIR, layer_id);
  return is_directory(path);
}

static int read_layer_link(const char *layer_id, char *out_link, size_t out_link_size) {
  char link_path[PATH_MAX];
  FILE *fp;

  if (layer_id == NULL || layer_id[0] == '\0' || out_link == NULL || out_link_size == 0) {
    return 1;
  }

  snprintf(link_path, sizeof(link_path), "%s/%s/link", ZOCKER_LAYERS_DIR, layer_id);

  fp = fopen(link_path, "r");
  if (fp == NULL) {
    return 1;
  }

  if (fgets(out_link, out_link_size, fp) == NULL) {
    fclose(fp);
    return 1;
  }

  fclose(fp);
  out_link[strcspn(out_link, "\r\n")] = '\0';
  return out_link[0] == '\0';
}

static int layer_mount_entry_from_id(const char *layer_id, char *out, size_t out_size) {
  char link_id[64];
  int n;

  if (read_layer_link(layer_id, link_id, sizeof(link_id)) == 0) {
    n = snprintf(out, out_size, "%s/%s", ZOCKER_LAYER_LINKS_DIR, link_id);
    return (n < 0 || (size_t)n >= out_size) ? 1 : 0;
  }

  n = snprintf(out, out_size, "%s/%s/diff", ZOCKER_LAYERS_DIR, layer_id);
  return (n < 0 || (size_t)n >= out_size) ? 1 : 0;
}

static int extract_layer_id_from_diff_entry(const char *entry, char *out_layer_id,
                                            size_t out_layer_id_size) {
  char prefix[PATH_MAX];
  const char *rest;
  const char *slash;
  size_t layer_id_len;

  if (entry == NULL || out_layer_id == NULL || out_layer_id_size == 0) {
    return 1;
  }

  snprintf(prefix, sizeof(prefix), "%s/", ZOCKER_LAYERS_DIR);
  if (!starts_with(entry, prefix)) {
    return 1;
  }

  rest = entry + strlen(prefix);
  slash = strchr(rest, '/');
  if (slash == NULL) {
    return 1;
  }

  if (strcmp(slash, "/diff") != 0) {
    return 1;
  }

  layer_id_len = (size_t)(slash - rest);
  if (layer_id_len == 0 || layer_id_len + 1 > out_layer_id_size) {
    return 1;
  }

  memcpy(out_layer_id, rest, layer_id_len);
  out_layer_id[layer_id_len] = '\0';
  return 0;
}

static int normalize_chain_entry(const char *entry, char *out, size_t out_size) {
  char layer_id[64];
  int n;

  if (entry == NULL || entry[0] == '\0' || out == NULL || out_size == 0) {
    return 1;
  }

  if (starts_with(entry, ZOCKER_LAYER_LINKS_DIR "/")) {
    n = snprintf(out, out_size, "%s", entry);
    return (n < 0 || (size_t)n >= out_size) ? 1 : 0;
  }

  if (extract_layer_id_from_diff_entry(entry, layer_id, sizeof(layer_id)) == 0) {
    if (layer_mount_entry_from_id(layer_id, out, out_size) == 0) {
      return 0;
    }
  }

  n = snprintf(out, out_size, "%s", entry);
  return (n < 0 || (size_t)n >= out_size) ? 1 : 0;
}

static int append_chain_entry(char *chain, size_t chain_size, const char *entry,
                              int with_separator) {
  size_t used;
  size_t remaining;
  int n;

  if (chain == NULL || chain_size == 0 || entry == NULL || entry[0] == '\0') {
    return 1;
  }

  used = strlen(chain);
  if (used >= chain_size) {
    return 1;
  }
  remaining = chain_size - used;

  if (with_separator) {
    n = snprintf(chain + used, remaining, ":%s", entry);
    return (n < 0 || (size_t)n >= remaining) ? 1 : 0;
  }

  n = snprintf(chain + used, remaining, "%s", entry);
  return (n < 0 || (size_t)n >= remaining) ? 1 : 0;
}

static int normalize_chain(const char *chain, char *out, size_t out_size) {
  char chain_copy[16384];
  char *saveptr = NULL;
  char *token;
  int first = 1;
  int n;

  if (chain == NULL || out == NULL || out_size == 0) {
    return 1;
  }

  out[0] = '\0';
  if (chain[0] == '\0') {
    return 0;
  }

  n = snprintf(chain_copy, sizeof(chain_copy), "%s", chain);
  if (n < 0 || (size_t)n >= sizeof(chain_copy)) {
    return 1;
  }

  token = strtok_r(chain_copy, ":", &saveptr);
  while (token != NULL) {
    char normalized[PATH_MAX];

    if (normalize_chain_entry(token, normalized, sizeof(normalized)) != 0) {
      return 1;
    }

    if (append_chain_entry(out, out_size, normalized, !first) != 0) {
      return 1;
    }

    first = 0;
    token = strtok_r(NULL, ":", &saveptr);
  }

  return 0;
}

int layer_chain_from_top(const char *layer_id, char *out_chain, size_t out_size) {
  char layer_entry[PATH_MAX];
  char lower_path[PATH_MAX];
  FILE *fp;
  char lower_line[8192] = {0};
  char normalized_lower[16384] = {0};
  int n;

  if (layer_id == NULL || layer_id[0] == '\0' || out_chain == NULL || out_size == 0) {
    return 1;
  }

  if (!layer_exists(layer_id)) {
    return 1;
  }

  if (layer_mount_entry_from_id(layer_id, layer_entry, sizeof(layer_entry)) != 0) {
    return 1;
  }
  snprintf(lower_path, sizeof(lower_path), "%s/%s/lower", ZOCKER_LAYERS_DIR, layer_id);

  fp = fopen(lower_path, "r");
  if (fp != NULL) {
    if (fgets(lower_line, sizeof(lower_line), fp) == NULL) {
      lower_line[0] = '\0';
    }
    fclose(fp);
  }

  lower_line[strcspn(lower_line, "\r\n")] = '\0';

  if (lower_line[0] == '\0') {
    n = snprintf(out_chain, out_size, "%s", layer_entry);
    return (n < 0 || (size_t)n >= out_size) ? 1 : 0;
  }

  if (normalize_chain(lower_line, normalized_lower, sizeof(normalized_lower)) != 0) {
    return 1;
  }

  if (normalized_lower[0] == '\0') {
    n = snprintf(out_chain, out_size, "%s", layer_entry);
    return (n < 0 || (size_t)n >= out_size) ? 1 : 0;
  }

  n = snprintf(out_chain, out_size, "%s:%s", layer_entry, normalized_lower);
  return (n < 0 || (size_t)n >= out_size) ? 1 : 0;
}

int resolve_zocker_image_chain(const char *ref, char *out_chain, size_t out_size) {
  struct image_meta meta;

  if (load_image_meta(ref, &meta) != 0) {
    return 1;
  }

  if (meta.top_layer[0] == '\0') {
    return 1;
  }

  return layer_chain_from_top(meta.top_layer, out_chain, out_size);
}

int register_layer_cache(const char *hash, const char *layer_id) {
  char path[PATH_MAX];
  FILE *fp;

  if (hash == NULL || layer_id == NULL) {
    return 1;
  }

  snprintf(path, sizeof(path), "%s/%s", ZOCKER_CACHE_DIR, hash);

  fp = fopen(path, "w");
  if (fp == NULL) {
    return 1;
  }

  fprintf(fp, "%s\n", layer_id);
  fclose(fp);
  return 0;
}

int lookup_layer_cache(const char *hash, char *layer_id, size_t layer_id_size) {
  char path[PATH_MAX];
  FILE *fp;

  if (hash == NULL || layer_id == NULL || layer_id_size == 0) {
    return 1;
  }

  snprintf(path, sizeof(path), "%s/%s", ZOCKER_CACHE_DIR, hash);
  fp = fopen(path, "r");
  if (fp == NULL) {
    return 1;
  }

  if (fgets(layer_id, layer_id_size, fp) == NULL) {
    fclose(fp);
    return 1;
  }

  fclose(fp);
  layer_id[strcspn(layer_id, "\r\n")] = '\0';

  if (!layer_exists(layer_id)) {
    return 1;
  }

  return 0;
}

int write_layer_metadata(const struct layer_meta *meta) {
  char path[PATH_MAX];
  FILE *fp;

  if (meta == NULL || meta->id[0] == '\0') {
    return 1;
  }

  snprintf(path, sizeof(path), "%s/%s/meta", ZOCKER_LAYERS_DIR, meta->id);
  fp = fopen(path, "w");
  if (fp == NULL) {
    return 1;
  }

  fprintf(fp, "id=%s\n", meta->id);
  fprintf(fp, "parent=%s\n", meta->parent);
  fprintf(fp, "hash=%s\n", meta->hash);
  fprintf(fp, "created_at=%ld\n", meta->created_at);
  fprintf(fp, "size=%llu\n", meta->size);
  fprintf(fp, "instruction=%s\n", meta->instruction);
  fprintf(fp, "workdir=%s\n", meta->workdir);

  fclose(fp);
  return 0;
}

int read_layer_metadata(const char *layer_id, struct layer_meta *meta) {
  char path[PATH_MAX];
  FILE *fp;
  char line[2048];

  if (layer_id == NULL || meta == NULL) {
    return 1;
  }

  memset(meta, 0, sizeof(*meta));
  snprintf(meta->id, sizeof(meta->id), "%s", layer_id);

  snprintf(path, sizeof(path), "%s/%s/meta", ZOCKER_LAYERS_DIR, layer_id);
  fp = fopen(path, "r");
  if (fp == NULL) {
    return 1;
  }

  while (fgets(line, sizeof(line), fp) != NULL) {
    char *eq = strchr(line, '=');
    char *key;
    char *value;

    if (eq == NULL) {
      continue;
    }

    *eq = '\0';
    key = trim_whitespace(line);
    value = trim_whitespace(eq + 1);
    value[strcspn(value, "\r\n")] = '\0';

    if (strcmp(key, "parent") == 0) {
      snprintf(meta->parent, sizeof(meta->parent), "%s", value);
    } else if (strcmp(key, "hash") == 0) {
      snprintf(meta->hash, sizeof(meta->hash), "%s", value);
    } else if (strcmp(key, "created_at") == 0) {
      meta->created_at = strtol(value, NULL, 10);
    } else if (strcmp(key, "size") == 0) {
      meta->size = strtoull(value, NULL, 10);
    } else if (strcmp(key, "instruction") == 0) {
      snprintf(meta->instruction, sizeof(meta->instruction), "%s", value);
    } else if (strcmp(key, "workdir") == 0) {
      snprintf(meta->workdir, sizeof(meta->workdir), "%s", value);
    }
  }

  fclose(fp);
  return 0;
}

static void format_age(long created_at, char *out, size_t out_size) {
  long now = (long)time(NULL);
  long delta = now - created_at;

  if (delta < 0) delta = 0;

  if (delta < 60) {
    snprintf(out, out_size, "%lds", delta);
  } else if (delta < 3600) {
    snprintf(out, out_size, "%ldm", delta / 60);
  } else if (delta < 86400) {
    snprintf(out, out_size, "%ldh", delta / 3600);
  } else {
    snprintf(out, out_size, "%ldd", delta / 86400);
  }
}

int list_images(void) {
  DIR *dir;
  struct dirent *ent;

  dir = opendir(ZOCKER_IMAGES_DIR);
  if (dir == NULL) {
    return 1;
  }

  printf("IMAGE\t\tTOP_LAYER\t\tCREATED\n");

  while ((ent = readdir(dir)) != NULL) {
    char path[PATH_MAX];
    struct image_meta meta;

    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
      continue;
    }

    if (!ends_with(ent->d_name, ".meta")) {
      continue;
    }

    snprintf(path, sizeof(path), "%s/%s", ZOCKER_IMAGES_DIR, ent->d_name);
    if (load_image_meta_from_path(path, &meta) == 0) {
      printf("%s\t%s\t%s\n", meta.ref, meta.top_layer, meta.created_at);
    }
  }

  closedir(dir);
  return 0;
}

int print_image_history(const char *ref) {
  struct image_meta image;
  char current[64];
  struct layer_meta *layers = NULL;
  size_t layer_count = 0;

  if (load_image_meta(ref, &image) != 0) {
    fprintf(stderr, "[ERR] Image not found: %s\n", ref);
    return 1;
  }

  snprintf(current, sizeof(current), "%s", image.top_layer);

  while (current[0] != '\0') {
    struct layer_meta meta;
    struct layer_meta *new_layers;

    if (read_layer_metadata(current, &meta) != 0) {
      break;
    }

    new_layers = realloc(layers, sizeof(struct layer_meta) * (layer_count + 1));
    if (new_layers == NULL) {
      free(layers);
      return 1;
    }
    layers = new_layers;
    layers[layer_count++] = meta;

    if (meta.parent[0] == '\0' || strcmp(meta.parent, "-") == 0) {
      break;
    }

    snprintf(current, sizeof(current), "%s", meta.parent);
  }

  printf("LAYER\t\tSIZE\tAGE\tINSTRUCTION\n");
  for (size_t i = 0; i < layer_count; i++) {
    char age[32];
    format_age(layers[i].created_at, age, sizeof(age));
    printf("%s\t%llu\t%s\t%s\n", layers[i].id, layers[i].size, age,
           layers[i].instruction);
  }

  free(layers);
  return 0;
}

int remove_image_ref(const char *ref) {
  char path[PATH_MAX];

  if (image_meta_path_from_ref(ref, path, sizeof(path)) != 0) {
    return 1;
  }

  if (unlink(path) != 0) {
    if (errno == ENOENT) {
      fprintf(stderr, "[ERR] Image not found: %s\n", ref);
    }
    return 1;
  }

  return 0;
}

struct str_set {
  char **items;
  size_t count;
  size_t cap;
};

static void str_set_free(struct str_set *set) {
  if (set == NULL) {
    return;
  }
  for (size_t i = 0; i < set->count; i++) {
    free(set->items[i]);
  }
  free(set->items);
  set->items = NULL;
  set->count = 0;
  set->cap = 0;
}

static int str_set_contains(const struct str_set *set, const char *value) {
  if (set == NULL || value == NULL) {
    return 0;
  }
  for (size_t i = 0; i < set->count; i++) {
    if (strcmp(set->items[i], value) == 0) {
      return 1;
    }
  }
  return 0;
}

static int str_set_add(struct str_set *set, const char *value) {
  char *copy;

  if (set == NULL || value == NULL || value[0] == '\0') {
    return 1;
  }

  if (str_set_contains(set, value)) {
    return 0;
  }

  if (set->count == set->cap) {
    size_t new_cap = set->cap == 0 ? 32 : set->cap * 2;
    char **new_items = realloc(set->items, new_cap * sizeof(char *));
    if (new_items == NULL) {
      return 1;
    }
    set->items = new_items;
    set->cap = new_cap;
  }

  copy = strdup(value);
  if (copy == NULL) {
    return 1;
  }

  set->items[set->count++] = copy;
  return 0;
}

static int mark_layer_chain_used(const char *top_layer, struct str_set *used) {
  char current[64];

  if (top_layer == NULL || top_layer[0] == '\0') {
    return 0;
  }

  snprintf(current, sizeof(current), "%s", top_layer);
  while (current[0] != '\0' && !str_set_contains(used, current)) {
    struct layer_meta meta;

    if (str_set_add(used, current) != 0) {
      return 1;
    }

    if (read_layer_metadata(current, &meta) != 0) {
      break;
    }

    if (meta.parent[0] == '\0' || strcmp(meta.parent, "-") == 0) {
      break;
    }

    snprintf(current, sizeof(current), "%s", meta.parent);
  }

  return 0;
}

static int collect_used_layers(struct str_set *used) {
  DIR *dir;
  struct dirent *ent;

  dir = opendir(ZOCKER_IMAGES_DIR);
  if (dir == NULL) {
    return 1;
  }

  while ((ent = readdir(dir)) != NULL) {
    char path[PATH_MAX];
    struct image_meta meta;

    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
      continue;
    }

    if (!ends_with(ent->d_name, ".meta")) {
      continue;
    }

    snprintf(path, sizeof(path), "%s/%s", ZOCKER_IMAGES_DIR, ent->d_name);
    if (load_image_meta_from_path(path, &meta) != 0) {
      continue;
    }

    if (mark_layer_chain_used(meta.top_layer, used) != 0) {
      closedir(dir);
      return 1;
    }
  }

  closedir(dir);
  return 0;
}

static int cleanup_cache_entries(void) {
  DIR *dir;
  struct dirent *ent;

  dir = opendir(ZOCKER_CACHE_DIR);
  if (dir == NULL) {
    return 1;
  }

  while ((ent = readdir(dir)) != NULL) {
    char path[PATH_MAX];
    char layer_id[128];
    FILE *fp;

    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
      continue;
    }

    snprintf(path, sizeof(path), "%s/%s", ZOCKER_CACHE_DIR, ent->d_name);
    fp = fopen(path, "r");
    if (fp == NULL) {
      continue;
    }

    if (fgets(layer_id, sizeof(layer_id), fp) == NULL) {
      fclose(fp);
      unlink(path);
      continue;
    }
    fclose(fp);
    layer_id[strcspn(layer_id, "\r\n")] = '\0';

    if (!layer_exists(layer_id)) {
      unlink(path);
    }
  }

  closedir(dir);
  return 0;
}

int prune_unused_layers(void) {
  int total_removed = 0;

  while (1) {
    DIR *dir;
    struct dirent *ent;
    struct str_set used = {0};
    int removed_in_round = 0;

    if (collect_used_layers(&used) != 0) {
      str_set_free(&used);
      return 1;
    }

    dir = opendir(ZOCKER_LAYERS_DIR);
    if (dir == NULL) {
      str_set_free(&used);
      return 1;
    }

    while ((ent = readdir(dir)) != NULL) {
      char layer_path[PATH_MAX];

      if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
        continue;
      }

      if (strcmp(ent->d_name, "l") == 0) {
        continue;
      }

      snprintf(layer_path, sizeof(layer_path), "%s/%s", ZOCKER_LAYERS_DIR,
               ent->d_name);
      if (!is_directory(layer_path)) {
        continue;
      }

      if (!str_set_contains(&used, ent->d_name)) {
        if (remove_recursive(layer_path) == 0) {
          removed_in_round++;
        }
      }
    }

    closedir(dir);
    str_set_free(&used);

    cleanup_cache_entries();

    if (removed_in_round == 0) {
      break;
    }

    total_removed += removed_in_round;
  }

  printf("Removed %d unused layers\n", total_removed);
  return 0;
}
