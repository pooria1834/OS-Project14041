#include <stdio.h>
#include <string.h>

#include "config.h"

int validate_config(struct config* cfg) {
  if (cfg->subcommand == NONE) {
    fprintf(stderr, "[ERR] Missing subcommand (run|build|history|images|rmi|prune)\n");
    return 1;
  }

  if (cfg->subcommand == RUN) {
    if (strcmp(cfg->name, "") == 0) {
      strncpy(cfg->name, DEFAULT_NAME, sizeof(cfg->name) - 1);
    }

    if (strcmp(cfg->command, "") == 0) {
      fprintf(stderr, "[ERR] Missing command (e.g. 'sleep 1000')\n");
      return 1;
    }

    if (strcmp(cfg->base_image, "") == 0 && strcmp(cfg->base_dir, "") == 0) {
      fprintf(stderr, "[ERR] Missing base image or base dir\n");
      return 1;
    }

    return 0;
  }

  if (cfg->subcommand == BUILD) {
    if (strcmp(cfg->zockerfile, "") == 0) {
      fprintf(stderr, "[ERR] Missing Zockerfile path (use -f /path/to/Zockerfile)\n");
      return 1;
    }

    if (strcmp(cfg->image_ref, "") == 0) {
      fprintf(stderr, "[ERR] Missing image tag (use -t imagename:tag)\n");
      return 1;
    }

    return 0;
  }

  if (cfg->subcommand == HISTORY || cfg->subcommand == RMI) {
    if (strcmp(cfg->image_ref, "") == 0) {
      fprintf(stderr, "[ERR] Missing image reference (e.g. app:latest)\n");
      return 1;
    }
    return 0;
  }

  return 0;
}
