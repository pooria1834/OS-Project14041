#ifndef __SETUP_H__
#define __SETUP_H__

#include <stddef.h>

#ifndef ZOCKER_PREFIX
#define ZOCKER_PREFIX "/tmp/zocker"
#endif

#ifndef ZOCKER_CONTAINERS_DIR
#define ZOCKER_CONTAINERS_DIR ZOCKER_PREFIX "/containers"
#endif

#ifndef ZOCKER_LAYERS_DIR
#define ZOCKER_LAYERS_DIR ZOCKER_PREFIX "/layers"
#endif

#ifndef ZOCKER_LAYER_LINKS_DIR
#define ZOCKER_LAYER_LINKS_DIR ZOCKER_PREFIX "/layers/l"
#endif

#ifndef ZOCKER_IMAGES_DIR
#define ZOCKER_IMAGES_DIR ZOCKER_PREFIX "/images"
#endif

#ifndef ZOCKER_CACHE_DIR
#define ZOCKER_CACHE_DIR ZOCKER_PREFIX "/cache"
#endif

#ifndef ZOCKER_BUILD_TMP_DIR
#define ZOCKER_BUILD_TMP_DIR ZOCKER_PREFIX "/tmp"
#endif

int setup_zocker_dir(void);
int setup_container_dir(const char id[64], char *container_dir, size_t container_dir_size,
                        const char base_image[4096]);
int resolve_base_chain(const char *base_ref_or_path, char *out_chain, size_t out_chain_size);
int build_docker_chain_from_upper(const char *upper_dir, char *out_chain, size_t out_chain_size);

#endif
