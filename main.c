#define _GNU_SOURCE

#include <stdio.h>
#include <string.h>

#include "build.h"
#include "config.h"
#include "image_store.h"
#include "run.h"
#include "setup.h"

static int append_run_command(struct config *cfg, const char *token) {
  size_t current_len = strlen(cfg->command);
  size_t token_len = strlen(token);

  if (current_len == 0) {
    if (token_len + 1 >= sizeof(cfg->command)) {
      return 1;
    }
    strcpy(cfg->command, token);
    return 0;
  }

  if (current_len + 1 + token_len + 1 >= sizeof(cfg->command)) {
    return 1;
  }

  strcat(cfg->command, " ");
  strcat(cfg->command, token);
  return 0;
}

static int parse_build_arg_value(const char *raw, struct config *cfg) {
  const char *eq;
  size_t key_len;

  if (cfg->build_arg_count >= MAX_BUILD_ARGS) {
    fprintf(stderr, "[ERR] Too many --build-arg values\n");
    return 1;
  }

  eq = strchr(raw, '=');
  if (eq == NULL) {
    fprintf(stderr, "[ERR] Invalid --build-arg value: %s (use KEY=VALUE)\n", raw);
    return 1;
  }

  key_len = (size_t)(eq - raw);
  if (key_len == 0 || key_len >= sizeof(cfg->build_args[cfg->build_arg_count].key)) {
    fprintf(stderr, "[ERR] Invalid --build-arg key: %s\n", raw);
    return 1;
  }

  memcpy(cfg->build_args[cfg->build_arg_count].key, raw, key_len);
  cfg->build_args[cfg->build_arg_count].key[key_len] = '\0';

  snprintf(cfg->build_args[cfg->build_arg_count].value,
           sizeof(cfg->build_args[cfg->build_arg_count].value), "%s", eq + 1);

  cfg->build_arg_count++;
  return 0;
}

int main(int argc, char **argv) {
  struct config cfg;
  int i = 1;

  if (setup_zocker_dir() != 0) {
    return 1;
  }

  memset(&cfg, 0, sizeof(cfg));
  cfg.subcommand = NONE;

  while (i < argc) {
    if (strcmp(argv[i], "run") == 0) {
      cfg.subcommand = RUN;
      i++;
      continue;
    }

    if (strcmp(argv[i], "exec") == 0) {
      cfg.subcommand = EXEC;
      i++;
      continue;
    }

    if (strcmp(argv[i], "build") == 0) {
      cfg.subcommand = BUILD;
      i++;
      continue;
    }

    if (strcmp(argv[i], "history") == 0) {
      cfg.subcommand = HISTORY;
      i++;
      continue;
    }

    if (strcmp(argv[i], "images") == 0) {
      cfg.subcommand = IMAGES;
      i++;
      continue;
    }

    if (strcmp(argv[i], "rmi") == 0) {
      cfg.subcommand = RMI;
      i++;
      continue;
    }

    if (strcmp(argv[i], "prune") == 0) {
      cfg.subcommand = PRUNE;
      i++;
      continue;
    }

    if (strcmp(argv[i], "--name") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "[ERR] Missing --name value\n");
        return 1;
      }
      snprintf(cfg.name, sizeof(cfg.name), "%s", argv[++i]);
      i++;
      continue;
    }

    if (strcmp(argv[i], "--base-dir") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "[ERR] Missing --base-dir value\n");
        return 1;
      }
      snprintf(cfg.base_dir, sizeof(cfg.base_dir), "%s", argv[++i]);
      i++;
      continue;
    }

    if (strcmp(argv[i], "--base-image") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "[ERR] Missing --base-image value\n");
        return 1;
      }
      snprintf(cfg.base_image, sizeof(cfg.base_image), "%s", argv[++i]);
      i++;
      continue;
    }

    if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--file") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "[ERR] Missing -f/--file value\n");
        return 1;
      }
      snprintf(cfg.zockerfile, sizeof(cfg.zockerfile), "%s", argv[++i]);
      i++;
      continue;
    }

    if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--tag") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "[ERR] Missing -t/--tag value\n");
        return 1;
      }
      snprintf(cfg.image_ref, sizeof(cfg.image_ref), "%s", argv[++i]);
      i++;
      continue;
    }

    if (strcmp(argv[i], "--build-arg") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "[ERR] Missing --build-arg value\n");
        return 1;
      }
      if (parse_build_arg_value(argv[++i], &cfg) != 0) {
        return 1;
      }
      i++;
      continue;
    }

    if (cfg.subcommand == RUN) {
      if (append_run_command(&cfg, argv[i]) != 0) {
        fprintf(stderr, "[ERR] run command is too long\n");
        return 1;
      }
      i++;
      continue;
    }

    if (cfg.subcommand == HISTORY || cfg.subcommand == RMI) {
      if (cfg.image_ref[0] == '\0') {
        snprintf(cfg.image_ref, sizeof(cfg.image_ref), "%s", argv[i]);
        i++;
        continue;
      }
    }

    fprintf(stderr, "[ERR] Unknown/unsupported argument: %s\n", argv[i]);
    return 1;
  }

  if (validate_config(&cfg) != 0) {
    return 1;
  }

  switch (cfg.subcommand) {
  case RUN: {
    struct container cont;
    memset(&cont, 0, sizeof(cont));
    container_from_config(cfg, &cont);
    if (run_container(cont) != 0) {
      fprintf(stderr,
              "[ERR] Running container failed due to internal errors.\n");
      return 1;
    }
    break;
  }
  case BUILD:
    if (build_image_from_config(&cfg) != 0) {
      return 1;
    }
    break;
  case HISTORY:
    if (print_image_history(cfg.image_ref) != 0) {
      return 1;
    }
    break;
  case IMAGES:
    if (list_images() != 0) {
      return 1;
    }
    break;
  case RMI:
    if (remove_image_ref(cfg.image_ref) != 0) {
      return 1;
    }
    break;
  case PRUNE:
    if (prune_unused_layers() != 0) {
      return 1;
    }
    break;
  case EXEC:
    printf("EXEC subcommand has not been implemented yet...\n");
    break;
  case NONE:
  default:
    break;
  }

  return 0;
}
