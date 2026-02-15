#ifndef __RUN_H__
#define __RUN_H__

#include "config.h"

struct container {
  char id[64];
  char command[1024];
  char base_dir[4096];
  char base_image[4096];
};

int run_container(struct container cont);
void container_from_config(struct config cfg, struct container *c);

#endif
