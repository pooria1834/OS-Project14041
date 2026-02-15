#define _GNU_SOURCE

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>

#include "image_store.h"
#include "setup.h"

static int ensure_dir(const char *path, mode_t mode) {
  struct stat st;

  if (stat(path, &st) == 0) {
    if (!S_ISDIR(st.st_mode)) {
      fprintf(stderr, "[ERR] Path exists and is not a directory: %s\n", path);
      return 1;
    }
    return 0;
  }

  if (errno != ENOENT) {
    fprintf(stderr, "[ERR] stat failed for %s: %s\n", path, strerror(errno));
    return 1;
  }

  if (mkdir(path, mode) != 0) {
    fprintf(stderr, "[ERR] mkdir failed for %s: %s\n", path, strerror(errno));
    return 1;
  }

  return 0;
}

static int resolve_docker_upper_dir(const char *image_name, char *out, size_t out_size) {
  char cmd[1024];
  FILE *fp;

  snprintf(cmd, sizeof(cmd),
           "docker inspect --format='{{.GraphDriver.Data.UpperDir}}' %s 2>/dev/null",
           image_name);

  fp = popen(cmd, "r");
  if (fp == NULL) {
    return 1;
  }

  if (fgets(out, out_size, fp) == NULL) {
    pclose(fp);
    return 1;
  }

  pclose(fp);
  out[strcspn(out, "\r\n")] = '\0';

  return out[0] == '\0';
}

static int validate_overlay_paths(const char *lower_chain, const char *upper,
                                  const char *work) {
  char lower_copy[12288];
  char *saveptr = NULL;
  char *token;
  char upper_real[PATH_MAX];
  char work_real[PATH_MAX];

  if (realpath(upper, upper_real) == NULL || realpath(work, work_real) == NULL) {
    fprintf(stderr, "[ERR] Failed to resolve overlay upper/work paths: %s\n",
            strerror(errno));
    return 1;
  }

  snprintf(lower_copy, sizeof(lower_copy), "%s", lower_chain);
  token = strtok_r(lower_copy, ":", &saveptr);

  while (token != NULL) {
    char lower_real[PATH_MAX];
    size_t n;

    if (realpath(token, lower_real) == NULL) {
      fprintf(stderr, "[ERR] Failed to resolve lowerdir path: %s (%s)\n", token,
              strerror(errno));
      return 1;
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
      return 1;
    }

    token = strtok_r(NULL, ":", &saveptr);
  }

  return 0;
}

int build_docker_chain_from_upper(const char *upper_dir, char *out_chain, size_t out_chain_size) {
  char docker_overlay_path[512];
  char lower_path[520];
  char raw_data[2048];
  char absolute_path[1024];
  FILE *lower_file;
  char *token;

  if (upper_dir == NULL || out_chain == NULL || out_chain_size == 0) {
    return 1;
  }

  if (snprintf(out_chain, out_chain_size, "%s", upper_dir) < 0) {
    return 1;
  }

  strncpy(docker_overlay_path, upper_dir, sizeof(docker_overlay_path) - 1);
  docker_overlay_path[sizeof(docker_overlay_path) - 1] = '\0';

  {
    char *diff_pos = strstr(docker_overlay_path, "/diff");
    if (diff_pos != NULL) {
      *diff_pos = '\0';
    }
  }

  snprintf(lower_path, sizeof(lower_path), "%s/lower", docker_overlay_path);
  lower_file = fopen(lower_path, "r");
  if (lower_file == NULL) {
    return 0;
  }

  if (fgets(raw_data, sizeof(raw_data), lower_file) == NULL) {
    fclose(lower_file);
    return 0;
  }
  fclose(lower_file);

  raw_data[strcspn(raw_data, "\r\n")] = '\0';
  token = strtok(raw_data, ":");

  while (token != NULL) {
    snprintf(absolute_path, sizeof(absolute_path), "%s/../%s", docker_overlay_path,
             token);

    if (strlen(out_chain) + strlen(absolute_path) + 2 >= out_chain_size) {
      fprintf(stderr, "[ERR] Base layer chain is too long.\n");
      return 1;
    }

    strcat(out_chain, ":");
    strcat(out_chain, absolute_path);
    token = strtok(NULL, ":");
  }

  return 0;
}

int resolve_base_chain(const char *base_ref_or_path, char *out_chain, size_t out_chain_size) {
  char docker_upper[512];

  if (base_ref_or_path == NULL || out_chain == NULL || out_chain_size == 0) {
    return 1;
  }

  if (base_ref_or_path[0] == '/') {
    if (strchr(base_ref_or_path, ':') != NULL) {
      snprintf(out_chain, out_chain_size, "%s", base_ref_or_path);
      return 0;
    }

    if (build_docker_chain_from_upper(base_ref_or_path, out_chain, out_chain_size) ==
        0) {
      return 0;
    }

    snprintf(out_chain, out_chain_size, "%s", base_ref_or_path);
    return 0;
  }

  if (resolve_zocker_image_chain(base_ref_or_path, out_chain, out_chain_size) == 0) {
    return 0;
  }

  if (resolve_docker_upper_dir(base_ref_or_path, docker_upper, sizeof(docker_upper)) !=
      0) {
    return 1;
  }

  return build_docker_chain_from_upper(docker_upper, out_chain, out_chain_size);
}

int setup_zocker_dir(void) {
  if (ensure_dir(ZOCKER_PREFIX, 0755) != 0) return 1;
  if (ensure_dir(ZOCKER_CONTAINERS_DIR, 0755) != 0) return 1;
  if (ensure_dir(ZOCKER_LAYERS_DIR, 0755) != 0) return 1;
  if (ensure_dir(ZOCKER_LAYER_LINKS_DIR, 0755) != 0) return 1;
  if (ensure_dir(ZOCKER_IMAGES_DIR, 0755) != 0) return 1;
  if (ensure_dir(ZOCKER_CACHE_DIR, 0755) != 0) return 1;
  if (ensure_dir(ZOCKER_BUILD_TMP_DIR, 0755) != 0) return 1;
  return 0;
}

int setup_container_dir(const char id[64], char *container_dir, size_t container_dir_size,
                        const char base_image[4096]) {
  char container_root[4096];
  char upper[4096];
  char work[4096];
  char merged[4096];
  char base_chain[8192];
  char mount_ops[12288];

  if (container_dir == NULL || container_dir_size == 0) {
    return 1;
  }

  snprintf(container_root, sizeof(container_root), "%s/%s", ZOCKER_CONTAINERS_DIR,
           id);
  if (mkdir(container_root, 0755) != 0) {
    if (errno == EEXIST) {
      fprintf(stderr, "[ERR] Container %s already exists\n", id);
      fprintf(stderr,
              "Use a different container name or use --base-dir instead of --base-image\n");
      return 1;
    }

    fprintf(stderr, "[ERR] Failed to create container directory: %s\n", strerror(errno));
    return 1;
  }

  snprintf(upper, sizeof(upper), "%s/upper", container_root);
  snprintf(work, sizeof(work), "%s/work", container_root);
  snprintf(merged, sizeof(merged), "%s/merged", container_root);

  mkdir(upper, 0755);
  mkdir(work, 0755);
  mkdir(merged, 0755);

  if (resolve_base_chain(base_image, base_chain, sizeof(base_chain)) != 0) {
    fprintf(stderr, "[ERR] Failed to resolve base image/path: %s\n", base_image);
    return 1;
  }

  snprintf(mount_ops, sizeof(mount_ops), "lowerdir=%s,upperdir=%s,workdir=%s",
           base_chain, upper, work);

  if (validate_overlay_paths(base_chain, upper, work) != 0) {
    return 1;
  }

  if (mount("overlay", merged, "overlay", 0, mount_ops) != 0) {
    fprintf(stderr, "[ERR] Overlay mount failed: %s\n", strerror(errno));
    return 1;
  }

  snprintf(container_dir, container_dir_size, "%s", merged);
  return 0;
}
