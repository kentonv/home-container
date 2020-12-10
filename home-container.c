// Copyright (c) 2015 Sandstorm Development Group, Inc. and contributors
// Licensed under the MIT License:
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <string.h>
#include <pwd.h>
#include <errno.h>
#include <assert.h>
#include <execinfo.h>
#include <fcntl.h>

// =======================================================================================
// generic error handling

void stack_trace(int skip) {
  void* trace[32];
  int size = backtrace(trace, 32);

  char exe[128];
  snprintf(exe, 128, "/proc/%d/exe", getpid());

  pid_t child = fork();
  if (child == 0) {
    char* cmd[36];
    cmd[0] = "addr2line";
    cmd[1] = "-e";
    cmd[2] = exe;

    char addrs[64][32];
    int i;
    for (i = 0; i < size - skip; i++) {
      snprintf(addrs[i], 64, "%p", trace[i + skip]);
      cmd[3 + i] = addrs[i];
    }
    cmd[3 + size - skip] = NULL;

    execvp("addr2line", cmd);
    perror("addr2line");
    exit(1);
  }

  int status;
  waitpid(child, &status, 0);

  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    fprintf(stderr, "raw trace:");
    for (int i = skip; i < size; i++) {
      fprintf(stderr, " %p", (char*)trace[i] - 1);
    }
    fprintf(stderr, "\n");
  }
}

void fail_errno_except_eintr(const char* code) {
  if (errno == EINTR) return;  // interrupted; return and try again
  perror(code);
  stack_trace(2);
  abort();
}

#define sys(code) while ((int)(code) == -1) fail_errno_except_eintr(#code);
// Run the given system call and abort if it fails.

void die(const char* why, ...) __attribute__((noreturn));
void die(const char* why, ...) {
  // Abort with the given error message.
  va_list args;
  va_start(args, why);
  vfprintf(stderr, why, args);
  putc('\n', stderr);
  stack_trace(2);
  abort();
}

// =======================================================================================
// helpers for setting up mount tree

unsigned long writable_mount_flags = 0;

enum file_type {
  NONEXISTENT,
  NON_DIRECTORY,
  DIRECTORY,
};

enum file_type get_file_type(const char* path) {
  // Determine if the path is a directory, a non-directory file, or doesn't exist.

  struct stat stats;
  if (stat(path, &stats) < 0) {
    if (errno == ENOENT || errno == ENOTDIR) {
      return NONEXISTENT;
    } else {
      perror(path);
      abort();
    }
  }
  return S_ISDIR(stats.st_mode) ? DIRECTORY : NON_DIRECTORY;
}

enum bind_type {
  EMPTY,       // just make an empty node of the same type (file or dir)
  READONLY,    // bind the destination to the source, read-only
  FULL,        // bind the destination to the source, read-write
};

void bind(enum bind_type type, const char* src, const char* dst) {
  // Bind-mount src to dst, such that dst becomes an alias for src.

  switch (get_file_type(src)) {
    case NONEXISTENT:
      // Skip files that don't exist.
      return;
    case DIRECTORY:
      while (mkdir(dst, 0777) < 0 && errno != EEXIST) {
        fail_errno_except_eintr("mkdir(dst, 0777)");
      }
      break;
    case NON_DIRECTORY:
      // Make an empty regular file to bind over.
      sys(mknod(dst, S_IFREG | 0777, 0));
      break;
  }

  if (type == EMPTY) {
    // Don't bind, just copy permissions.
    struct stat stats;
    sys(stat(src, &stats));
    sys(chown(dst, stats.st_uid, stats.st_gid));
    sys(chmod(dst, stats.st_mode));
  } else {
    // Bind the source file over the destination.
    sys(mount(src, dst, NULL, MS_BIND | MS_REC, NULL));
    if (type == READONLY) {
      // Setting the READONLY flag requires a remount. (If we tried to set it in the
      // first mount it would be silently ignored.)
      sys(mount(src, dst, NULL, MS_REMOUNT | MS_BIND | MS_REC | MS_RDONLY, NULL));
    } else if (writable_mount_flags) {
      // Need to remount to apply writable_mount_flags.
      sys(mount(src, dst, NULL, MS_REMOUNT | MS_BIND | MS_REC | writable_mount_flags, NULL));
    }
  }
}

void hide(const char* dst) {
  // If the given path exists, hide it by overmounting it with an empty file/dir.

  switch (get_file_type(dst)) {
    case NONEXISTENT:
      return;
    case DIRECTORY:
      // Empty tmpfs.
      sys(mount("tmpfs", dst, "tmpfs", writable_mount_flags, "size=2M,nr_inodes=4096,mode=755"));
      break;
    case NON_DIRECTORY:
      sys(mount("/dev/null", dst, NULL, MS_BIND | MS_REC, NULL));
      break;
  }
}

void bind_in_container(enum bind_type type, const char* path) {
  // Assuming the current directory is where we're setting up the container, bind the
  // given absolute path from outside the container to the same path inside.

  assert(path[0] == '/');

  // Verify parent has been bound, or bind it "empty".
  char parent[strlen(path + 1)];
  strcpy(parent, path);
  char* slashPos = strrchr(parent + 1, '/');
  if (slashPos != NULL) {
    *slashPos = '\0';
    if (access(parent + 1, F_OK) != 0) {
      bind_in_container(EMPTY, parent);
    }
  }

  // OK, bind child.
  bind(type, path, path + 1);
}

void hide_in_container(const char* path) {
  // Assuming the current directory is where we're setting up the container, hide the given
  // absolute path inside the container.

  assert(path[0] == '/');
  hide(path + 1);
}

int mkdir_user_owned(const char* path, mode_t mode, struct passwd* user) {
  int result = mkdir(path, mode);
  if (result >= 0) {
    sys(chown(path, user->pw_uid, user->pw_gid));
  }
  return result;
}

const char* home_path(struct passwd* user, const char* path) {
  static char result[512];
  if (path == NULL) {
    snprintf(result, 512, "/home/%s", user->pw_name);
  } else {
    snprintf(result, 512, "/home/%s/%s", user->pw_name, path);
  }
  return result;
}

// =======================================================================================

void usage(const char* self) {
  fprintf(stderr,
      "usage: %1$s NAME OPTIONS COMMAND\n"
      "\n"
      "Runs COMMAND inside the home directory container with the given name.\n"
      "Within the container, your real home directory will be invisible (modulo\n"
      "options below), replaced by a directory that starts out empty, but which\n"
      "persists across runs with the same container name.\n"
      "\n"
      "Hint: You can maintain multiple \"profiles\" (different configurations\n"
      "of the same app) by running the same app in multiple containers.\n"
      "\n"
      "Options:\n"
      "    --nx      Prevent executing files from locations that are writable.\n"
      "    -r <dir>  Make <dir> from your real homedir accessible in the\n"
      "              container read-only.\n"
      "    -w <dir>  Make <dir> from your real homedir accessible in the\n"
      "              container with full access.\n"
      "    -h <dir>  Hide <dir>, a subdirectory of a <dir> passed to a previous\n"
      "              -w or -r. This makes the directory inaccessible in the\n"
      "              container (it will appear empty and unwritable).\n"
      "\n"
      "Example:\n"
      "    %1$s browser -w Downloads google-chrome\n"
      "        Runs Google Chrome in a container but lets it put downloads in\n"
      "        your real \"Downloads\" directory.\n", self);
}

void validate_map_path_piece(const char* path, const char* piece) {
  if (strcmp(piece, "") == 0 ||
      strcmp(piece, ".") == 0 ||
      strcmp(piece, "..") == 0) {
    die("invalid: %s", path);
  }
}

void validate_map_path(const char* path) {
  // Disallow path injection (., .., absolute paths) in mappings.

  if (strlen(path) >= 256) {
    die("too long: %s", path);
  }

  char copy[256];
  strcpy(copy, path);

  const char* piece = copy;
  for (char* pos = copy; *pos != '\0'; ++pos) {
    if (*pos == '/') {
      *pos = '\0';
      // Now `piece` points to just one path component.
      validate_map_path_piece(path, piece);
      piece = pos + 1;
    }
  }
  validate_map_path_piece(path, piece);
}

void write_file(const char* filename, const char* content) {
  int fd = open(filename, O_WRONLY);
  ssize_t n;
  sys(n = write(fd, content, strlen(content)));
  if (n < strlen(content)) {
    die("incomplete write");
  }
  sys(close(fd));
}

int main(int argc, const char* argv[]) {
  if (argc < 1) die("no argv[0]?");  // shouldn't happen
  const char* self = argv[0];
  --argc;
  ++argv;

  if (argc < 1 || argv[0][0] == '-') {
    usage(self);
    return strcmp(argv[0], "--help");
  }
  const char* container_name = argv[0];
  --argc;
  ++argv;

  if (argc > 0 && strcmp(argv[0], "--nx") == 0) {
    writable_mount_flags = MS_NOEXEC;
    --argc;
    ++argv;
  }

  // Disallow path injection in container name (., .., or anything with a /).
  //
  // Also disallow overly long values as this is C and I'm too lazy to dynamically allocate
  // strings.
  if (*container_name == '\0' || strchr(container_name, '/') != NULL ||
      strcmp(container_name, ".") == 0 || strcmp(container_name, "..") == 0 ||
      strlen(container_name) > 128) {
    die("invalid: %s", container_name);
  }

  // Check that we are suid-root, but were not executed by root.
  // TODO: Once Chrome supports uid namespaces rather than using a setuid sandbox, we
  //   should also switch to using uid namespaces and not require setuid. See:
  //   https://code.google.com/p/chromium/issues/detail?id=312380
  uid_t ruid, euid, suid;
  sys(getresuid(&ruid, &euid, &suid));
  if (ruid == 0) {
    die("please run as non-root");
  }
  if (euid == 0 || suid == 0) {
    die("please don't use setuid-root binary anymore");
  }

  gid_t gid = getgid();

  // Get username of the user who executed us.
  struct passwd* user = getpwuid(ruid);
  if (user == NULL) die("getpwuid() failed");
  if (strlen(user->pw_name) > 128) {
    // This is C and I'm too lazy to allocate strings dynamically so let's just prevent ridiculous
    // usernames.
    die("username too long");
  }

  // Enter a private mount namespace.
  // TODO: Also unshare PID namespace. Requires mounting our own /proc and acting as init.
  // TODO: Also unshare IPC namespace? Or will that screw up desktop interaction?
  sys(unshare(CLONE_NEWUSER | CLONE_NEWNS));

  char user_map[64];
  write_file("/proc/self/setgroups", "deny\n");
  snprintf(user_map, 64, "1000 %d 1\n", ruid);
  write_file("/proc/self/uid_map", user_map);
  snprintf(user_map, 64, "1000 %d 1\n", gid);
  write_file("/proc/self/gid_map", user_map);

  // To really get our own private mount tree, we have to remount root as "private". Otherwise
  // our changes may be propagated to the original mount namespace and ruin everything.
  sys(mount("none", "/", NULL, MS_REC | MS_PRIVATE, NULL));

  // Start building our new tree under /tmp. First, bind-mount / to /tmp and make it read-only.
  sys(mount("/", "/tmp", NULL, MS_BIND | MS_REC, NULL));
  sys(mount("/", "/tmp", NULL, MS_REMOUNT | MS_BIND | MS_REC | MS_RDONLY, NULL));

  // We'll set the container root as our current directory so that the _in_container() helpers
  // work.
  sys(chdir("/tmp"));

  // Stuff in /var probably shouldn't be visible in the container, except /var/tmp.
  hide_in_container("/var");
  bind_in_container(FULL, "/var/tmp");

  // Mount /tmp into the sandbox with full access.
  char tmp_dir[512];
  snprintf(tmp_dir, 512, "/var/tmp/home-container.%s.%s", user->pw_name, container_name);
  mkdir_user_owned(tmp_dir, 0700, user);
  bind(FULL, tmp_dir, "tmp");

  // Hide /home, then we'll bring back the specific things we need.
  hide_in_container("/home");

  // Make the container directory if it doesn't exist, then bind it as the home directory.
  mkdir_user_owned(home_path(user, ".home-container"), 0700, user);
  char container_dir[512];
  snprintf(container_dir, 512, "/home/%s/.home-container/%s", user->pw_name, container_name);
  mkdir_user_owned(container_dir, 0700, user);
  bind(FULL, container_dir, home_path(user, NULL) + 1);

  // Interpret options.
  while (argc > 0 && argv[0][0] == '-') {
    if (strcmp(argv[0], "-w") == 0 && argc > 1) {
      validate_map_path(argv[1]);
      bind_in_container(FULL, home_path(user, argv[1]));
      argc -= 2;
      argv += 2;
    } else if (strcmp(argv[0], "-r") == 0 && argc > 1) {
      validate_map_path(argv[1]);
      bind_in_container(READONLY, home_path(user, argv[1]));
      argc -= 2;
      argv += 2;
    } else if (strcmp(argv[0], "-h") == 0 && argc > 1) {
      validate_map_path(argv[1]);
      hide_in_container(home_path(user, argv[1]));
      argc -= 2;
      argv += 2;
    } else if (strcmp(argv[0], "--nx") == 0) {
      die("--nx must be specified before other flags");
    } else if (strcmp(argv[0], "--help") == 0) {
      usage(self);
      return 0;
    } else {
      usage(self);
      return 1;
    }
  }

  if (argc == 0) {
    fprintf(stderr, "missing command");
    usage(self);
    return 1;
  }

  // Use pivot_root() to replace our root directory with the tree we built in /tmp. This is
  // more secure than chroot().
  sys(syscall(SYS_pivot_root, "/tmp", "/tmp/tmp"));
  sys(umount2("/tmp", MNT_DETACH));
  chdir("/");

  // Drop privileges.
  sys(setresuid(ruid, ruid, ruid));

  assert(argv[argc] == NULL);

  // Execute!
  sys(execvp(argv[0], (char**)argv));
  die("can't get here");
}
