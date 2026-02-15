#include <stdio.h>
#include <string.h>

#include "config.h"

int validate_config(struct config* cfg) {
  if (cfg->subcommand == NONE) {
    fprintf(stderr, "[ERR] Mssing subcommand (run|exec)\n");
    return 1;
  }

  if (strcmp(cfg->name, "") == 0) {
    strncpy(cfg->name, DEFAULT_NAME, sizeof(cfg->name) - 1);
  }

  if (strcmp(cfg->command, "") == 0) {
    fprintf(stderr, "[ERR] Mssing command (e.g. 'sleep 1000')\n");
    return 1;
  }

  if (strcmp(cfg->base_image, "") == 0 && strcmp(cfg->base_dir, "") == 0) {
    fprintf(stderr, "[ERR] Mssing base image or base dir\n");
    return 1;
  }
  return 0;
}
