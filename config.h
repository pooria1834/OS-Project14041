#ifndef __CONFIG_H__
#define __CONFIG_H__

#ifndef DEFAULT_NAME
#define DEFAULT_NAME "bib"
#endif

#ifndef MAX_BUILD_ARGS
#define MAX_BUILD_ARGS 64
#endif

enum COMMAND {
  NONE = 0,
  RUN = 10,
  EXEC = 11,
  BUILD = 12,
  HISTORY = 13,
  IMAGES = 14,
  RMI = 15,
  PRUNE = 16,
};

struct build_arg {
  char key[64];
  char value[256];
};

struct config {
  enum COMMAND subcommand;
  char name[64];
  char command[1024];
  char base_dir[4096];
  char base_image[4096];
  char zockerfile[1024];
  char image_ref[256];
  struct build_arg build_args[MAX_BUILD_ARGS];
  int build_arg_count;
};

int validate_config(struct config *cfg);
#endif
