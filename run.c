#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config.h"
#include "run.h"
#include "setup.h"

void container_from_config(struct config cfg, struct container *c) {
  strncpy(c->id, cfg.name, sizeof(c->id));
  strncpy(c->command, cfg.command, sizeof(c->command));
  strncpy(c->base_dir, cfg.base_dir, sizeof(c->base_dir));
  strncpy(c->base_image, cfg.base_image, sizeof(c->base_image));
}

int run_container(struct container cont) {
  pid_t pid;
  int ns_flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWTIME;

  if (unshare(ns_flags) != 0) {
    if (errno == EINVAL) {
      ns_flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS;
      if (unshare(ns_flags) != 0) {
        fprintf(stderr, "[ERR] Failed to unshare namespaces: %s\n",
                strerror(errno));
        return 1;
      }
      fprintf(stderr,
              "[WARN] CLONE_NEWTIME is not supported; running without time namespace.\n");
    } else {
      fprintf(stderr, "[ERR] Failed to unshare namespaces: %s\n", strerror(errno));
      return 1;
    }
  }

  pid = fork();
  if (pid < 0) {
    return 1;
  }

  if (pid == 0) {
    char container_dir[4096];
    if (strlen(cont.base_dir) > 0) {
      snprintf(container_dir, sizeof(container_dir), "%s", cont.base_dir);
    } else if (setup_container_dir(cont.id, container_dir, sizeof(container_dir),
                                   cont.base_image) != 0) {
      fprintf(stderr, "[ERR] Failed to setup container directory for %s\n",
              cont.id);
      return 1;
    }

    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
      fprintf(stderr, "[ERR] Failed to change mount to private: %s\n",
              strerror(errno));
      return 1;
    }

    if (chroot(container_dir) != 0) {
      fprintf(stderr,
              "[ERR] Failed to chroot into container directory for %s: %s\n",
              cont.id, strerror(errno));
      return 1;
    }

    if (chdir("/") != 0) {
      fprintf(stderr, "[ERR] Failed to change directory to root: %s\n",
              strerror(errno));
      return 1;
    }

    if (mkdir("/proc", 0555) != 0) {
      if (errno != EEXIST) {
        fprintf(stderr, "[ERR] Failed to create /proc directory: %s\n",
                strerror(errno));
        return 1;
      }
    }

    if (mount(NULL, "/proc", "proc", 0, NULL) != 0) {
      fprintf(stderr, "[WARN] Failed to remount /proc: %s\n", strerror(errno));
    }

    if (sethostname(cont.id, 64) != 0) {
      fprintf(stderr, "[WARN] Failed to set hostname: %s\n", strerror(errno));
    }

    printf("Running child with pid: %d\n", getpid());
    if (execl("/bin/sh", "sh", "-c", cont.command, NULL) != 0) {
      fprintf(stderr, "[ERR] Failed to call create container process: %s\n",
              strerror(errno));
      return 1;
    }
  } else {
    int status = 0;
    sleep(2);
    if (waitpid(pid, &status, 0) < 0) {
      fprintf(stderr, "[ERR] waitpid failed: %s\n", strerror(errno));
      return 1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      fprintf(stderr, "[ERR] Container child failed with status=%d\n",
              WIFEXITED(status) ? WEXITSTATUS(status) : -1);
      return 1;
    }
    printf("[Parent] Stoping...\n");
  }
  return 0;
}
