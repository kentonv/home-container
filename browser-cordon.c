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

// =======================================================================================
// error handling

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
}

void fail_errno_except_eintr(const char* code) {
  if (errno == EINTR) return;  // interrupted; return and try again
  perror(code);
  stack_trace(2);
  abort();
}

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

#define sys(code) while ((int)(code) == -1) fail_errno_except_eintr(#code);
// Run the given system call and abort if it fails.

// =======================================================================================
// helpers for setting up mount tree

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
      sys(mkdir(dst, 0777));
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
    } else {
      // This directory will be writable. Let's make it noexec, though, to try to disrupt
      // exploits that write a binary to disk then execute it. (Note that this is pretty
      // easy to get around if the attacker knows to expect it.)
      sys(mount(src, dst, NULL, MS_REMOUNT | MS_BIND | MS_REC | MS_NOEXEC, NULL));
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
      sys(mount("tmpfs", dst, "tmpfs", 0, "size=2M,nr_inodes=4096,mode=777"));
      break;
    case NON_DIRECTORY:
      sys(mount("/dev/null", dst, NULL, MS_BIND | MS_REC, NULL));
      break;
  }
}

void bind_in_cordon(enum bind_type type, const char* path) {
  // Assuming the current directory is where we're setting up the cordon, bind the
  // given absolute path from outside the cordon to the same path inside.

  assert(path[0] == '/');
  bind(type, path, path + 1);
}

void hide_in_cordon(const char* path) {
  // Assuming the current directory is where we're setting up the cordon, hide the given
  // absolute path inside the cordon.

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
  snprintf(result, 512, "/home/%s%s%s", user->pw_name, *path == '\0' ? "" : "/", path);
  return result;
}

// =======================================================================================
// Chrome-specific setup

void setup_chrome(struct passwd* user, const char* profile) {
  // Chrome reads system config stuff from ~/.local/share and ~/.config.
  bind_in_cordon(EMPTY, home_path(user, ".local"));
  bind_in_cordon(READONLY, home_path(user, ".local/share"));
  bind_in_cordon(READONLY, home_path(user, ".config"));

  // libnss certificate store -- needs to be writable so that you can edit certificates in
  // Chrome's settings.
  bind_in_cordon(FULL, home_path(user, ".pki"));

  // The browser needs to write to Downloads, obviously.
  bind_in_cordon(FULL, home_path(user, "Downloads"));

  // I think ~90% of my in-browser uploads are from Pictures, so map that in read-only.
  bind_in_cordon(READONLY, home_path(user, "Pictures"));

  // Make the profile directory if it doesn't exist.
  char profile_dir[512];
  snprintf(profile_dir, 512, ".browser-cordon/%s", profile);
  mkdir_user_owned(home_path(user, ".browser-cordon"), 0700, user);
  mkdir_user_owned(home_path(user, profile_dir), 0700, user);

  // Bind in the specific profile.
  bind_in_cordon(EMPTY, home_path(user, ".browser-cordon"));
  bind_in_cordon(FULL, home_path(user, profile_dir));
}

void run_chrome(struct passwd* user, const char* profile) {
  char param[512];
  snprintf(param, 512, "--user-data-dir=/home/%s/.browser-cordon/%s", user->pw_name, profile);
  sys(execlp("google-chrome", "google-chrome", param, NULL));
  die("can't get here");
}

// =======================================================================================

void validate(const char* name) {
  // Disallow path injection (., .., or anything with a /).

  if (*name == '\0' || strchr(name, '/') != NULL ||
      strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
    die("invalid: %s", name);
  }
}

int main(int argc, const char* argv[]) {
  if (argc > 2) {
    fprintf(stderr, "usage: %s [profile-name]\n", argv[0]);
    return 1;
  }
  const char* profile = argc < 2 ? "default" : argv[1];

  validate(profile);

  // Check that we are suid-root, but were not executed by root.
  // TODO: Once Chrome supports uid namespaces rather than using a setuid sandbox, we
  //   should also switch to using uid namespaces and not require setuid. See:
  //   https://code.google.com/p/chromium/issues/detail?id=312380
  uid_t ruid, euid, suid;
  sys(getresuid(&ruid, &euid, &suid));
  if (euid != 0) {
    die("binary needs to be setuid to set up sandbox");
  }
  if (ruid == 0) {
    die("please run as non-root");
  }

  // Get username of the user who executed us.
  struct passwd* user = getpwuid(ruid);
  if (user == NULL) die("getpwuid() failed");

  // Enter a private mount namespace.
  // TODO: Also unshare PID namespace. Requires mounting our own /proc and acting as init.
  // TODO: Also unshare IPC namespace? Or will that screw up desktop interaction?
  sys(unshare(CLONE_NEWNS));

  // To really get our own private mount tree, we have to remount root as "private". Otherwise
  // our changes may be propagated to the original mount namespace and ruin everything.
  sys(mount("none", "/", NULL, MS_REC | MS_PRIVATE, NULL));

  // Start building our new tree under /tmp. First, bind-mount / to /tmp and make it read-only.
  sys(mount("/", "/tmp", NULL, MS_BIND | MS_REC, NULL));
  sys(mount("/", "/tmp", NULL, MS_REMOUNT | MS_BIND | MS_REC | MS_RDONLY, NULL));

  // We'll set the cordon root as our current directory so that the _in_cordon() helpers
  // work.
  sys(chdir("/tmp"));

  // Stuff in /var probably shouldn't be visible in the cordon, except /var/tmp.
  hide_in_cordon("/var");
  bind_in_cordon(FULL, "/var/tmp");

  // Hide /home, then we'll bring back the specific things we need.
  hide_in_cordon("/home");
  bind_in_cordon(EMPTY, home_path(user, ""));

  // Bind in the stuff Chrome needs.
  setup_chrome(user, profile);

  // Use pivot_root() to replace our root directory with the tree we built in /tmp. This is
  // more secure than chroot().
  sys(syscall(SYS_pivot_root, "/tmp", "/tmp/tmp"));
  sys(umount2("/tmp", MNT_DETACH));
  chdir("/");

  // Mount a new tmpfs at our new /tmp, since otherwise we're left with a read-only /tmp
  // (that is shared with apps outside the sandbox).
  sys(mount("tmpfs", "/tmp", "tmpfs", 0, "size=16M,nr_inodes=4096,mode=777"));

  // Drop privileges.
  sys(setresuid(ruid, ruid, ruid));

  // Execute Chrome!
  run_chrome(user, profile);
}
