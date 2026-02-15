#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>
#include <sys/mount.h>

#include "setup.h"


static int setup_lower_dir(const char* base_image, char* lower_content, size_t size) {
  char docker_overlay_path[512];
  char lower_path[520];
  char raw_data[2048];
  char absolute_path[1024];

  strncpy(docker_overlay_path, base_image, sizeof(docker_overlay_path));
  char *diff_pos = strstr(docker_overlay_path, "/diff");
  if (diff_pos) *diff_pos = '\0';
  
  snprintf(lower_path, sizeof(lower_path), "%s/lower", docker_overlay_path);
  FILE* lower_file = fopen(lower_path, "r");
  if(!lower_file) return -1;

  if(fgets(raw_data, sizeof(raw_data), lower_file) == NULL){
    fclose(lower_file);
     return -1;
  }
  fclose(lower_file);
  raw_data[strcspn(raw_data, "\r\n")] = 0;

  lower_content[0] = '\0';
  char* token = strtok(raw_data, ":");
  while(token!= NULL) {
    snprintf(absolute_path, sizeof(absolute_path), "%s/../%s", docker_overlay_path, token);
    strncat(lower_content, absolute_path, size - strlen(lower_content) - 1);
    token = strtok(NULL, ":");
    if(token != NULL) {
      strncat(lower_content, ":", size - strlen(lower_content) - 1);
    }
  }

  return 0;
}

int setup_zocker_dir(void) {
  struct stat st;
  char prefix[64];

  if (snprintf(prefix, sizeof(prefix), "%s", ZOCKER_PREFIX) < 0) {
    return 1;
  }

  if (stat(prefix, &st) == -1) {
    if (errno == ENOENT) {
      fprintf(stderr, "[ATT] ZOCKER_PREFIX %s does not exists\n",
              ZOCKER_PREFIX);
    }
    mkdir(ZOCKER_PREFIX, 0755);
    return 0;
  }

  if (!S_ISDIR(st.st_mode)) {
    fprintf(stderr, "[ERR] ZOCKER_PREFIX %s is not a directory\n",
            ZOCKER_PREFIX);
    return 1;
  }

  return 0;
}

int setup_container_dir(const char id[64], char container_dir[256], const char base_image[512]) {
  char upper[512], work[512], merged[512], lower_raw[2048], mount_ops[4096];

  snprintf(container_dir, 256, "%s/%s", ZOCKER_PREFIX, id);
  if(mkdir(container_dir, 0755) != 0) {
    if(errno == EEXIST) {
      fprintf(stderr, "[ERR] Container %s already exists\n", id);
      fprintf(stderr, "Use a different container name or user --base-dir instead of --base-image");
      return 1;
    }

    fprintf(stderr, "Failed to create container directory\n");
    return 1;
  }

  snprintf(upper, 512, "%s/upper", container_dir);
  snprintf(work, 512, "%s/work", container_dir);
  snprintf(merged, 512, "%s/merged", container_dir);

  mkdir(upper, 0755);
  mkdir(work, 0755);
  mkdir(merged, 0755);

  if (setup_lower_dir(base_image, lower_raw, sizeof(lower_raw)) != 0) {
    snprintf(mount_ops, sizeof(mount_ops), "lowerdir=%s,upperdir=%s,workdir=%s",base_image, upper, work);
  } else {
    snprintf(mount_ops, sizeof(mount_ops), "lowerdir=%s:%s,upperdir=%s,workdir=%s",base_image, lower_raw, upper, work);
  }

    if (mount("overlay", merged, "overlay", 0, mount_ops) != 0) {
        fprintf(stderr, "[ERR] Overlay mount failed: %s\n", strerror(errno));
        return 1;
    }

    strncpy(container_dir, merged, 255);
    return 0;
}