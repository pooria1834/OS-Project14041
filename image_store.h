#ifndef __IMAGE_STORE_H__
#define __IMAGE_STORE_H__

#include <stddef.h>

struct image_meta {
  char name[192];
  char tag[64];
  char ref[256];
  char top_layer[64];
  char created_at[64];
  char cmd[1024];
};

struct layer_meta {
  char id[64];
  char parent[64];
  char hash[32];
  long created_at;
  unsigned long long size;
  char instruction[1024];
  char workdir[512];
};

int parse_image_ref(const char *ref, char *name, size_t name_size, char *tag,
                    size_t tag_size);

int save_image_meta(const struct image_meta *meta);
int load_image_meta(const char *ref, struct image_meta *meta);
int image_exists(const char *ref);

int resolve_zocker_image_chain(const char *ref, char *out_chain, size_t out_size);
int layer_chain_from_top(const char *layer_id, char *out_chain, size_t out_size);

int register_layer_cache(const char *hash, const char *layer_id);
int lookup_layer_cache(const char *hash, char *layer_id, size_t layer_id_size);