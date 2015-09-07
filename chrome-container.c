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
#include <string.h>
#include <pwd.h>

void fail_errno(const char* code) __attribute__((noreturn));
void fail_errno(const char* code) {
  perror(code);
  abort();
}

void fail(const char* why, ...) __attribute__((noreturn));
void fail(const char* why, ...) {
  va_list args;
  va_start(args, why);
  vfprintf(stderr, why, args);
  abort();
}

#define sys(code) if ((int)(code) == -1) fail_errno(#code);
#define die(reason, ...) fail(reason "\n", ##__VA_ARGS__);

const char* const MAP_FROM_HOME[] = {
  // Things in the user's home directory which Chrome needs to be able to see.
  // TODO: Tighten this. Probably only certain subdirectories are needed, and in some
  //   cases they can probably be bound read-only. But at least .ssh and .gnupg are
  //   not on the list!
  ".config", ".local", ".pki", "Downloads"
};

void validate(const char* name) {
  // Disallow path injection (., .., or anything with a /).

  if (strchr(name, '/') != NULL || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
    die("invalid: %s", name);
  }
}

int main(int argc, const char* argv[]) {
  if (argc < 2 || argc > 3) {
    fprintf(stderr, "usage: %s [google-chrome{,-beta,-dev}] [profile-name]\n", argv[0]);
    return 1;
  }
  const char* chrome_cmd = argc < 2 ? "google-chrome" : argv[1];
  const char* chrome_profile = argc < 3 ? chrome_cmd : argv[2];
  
  validate(chrome_cmd);
  validate(chrome_profile);
  
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
  sys(mount("/tmp", "/tmp", NULL, MS_REMOUNT | MS_BIND | MS_REC | MS_RDONLY, NULL));
  
  // Overmount /home with a tmpfs where we'll then only bind in the things that Chrome needs.
  sys(mount("tmpfs", "/tmp/home", "tmpfs", 0, "size=2M,nr_inodes=4096,mode=777"));
  
  // Create the user's home directory in the new tmpfs.
  char buffer[512];
  sprintf(buffer, "/tmp/home/%s", user->pw_name);
  sys(mkdir(buffer, 0777));
  sys(chown(buffer, ruid, -1));

  // Bind the chrome profile directory into the new mount tree at ~/chrome-profile (regardless
  // of its original filename).
  char chrome_home[512];
  sprintf(chrome_home, "/home/%s/.config/%s", user->pw_name, chrome_profile);

  if (access(chrome_home, F_OK) != 0) {
    sys(mkdir(chrome_home, 0700));
    sys(chown(chrome_home, ruid, -1));
  }

  sprintf(buffer, "/tmp/home/%s/chrome-profile", user->pw_name);
  sys(mkdir(buffer, 0777));
  sys(mount(chrome_home, buffer, NULL, MS_BIND | MS_REC, NULL));

  // Bind in the files and directories from the user's homedir which Chrome needs to operate.
  // (See the list defined above as MAP_FROM_HOME.)
  size_t i;
  for (i = 0; i < sizeof(MAP_FROM_HOME) / sizeof(MAP_FROM_HOME[0]); i++) {
    char from[512], to[512];
    sprintf(from, "/home/%s/%s", user->pw_name, MAP_FROM_HOME[i]);
    sprintf(to, "/tmp/%s", from);
    
    struct stat stats;
    if (stat(from, &stats) >= 0) {
      if (S_ISDIR(stats.st_mode)) {
        sys(mkdir(to, 0777));
      } else {
        sys(mknod(to, S_IFREG | 0777, 0));
      }
      sys(mount(from, to, NULL, MS_BIND | MS_REC, NULL));
    }
  }

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
  sprintf(buffer, "--user-data-dir=/home/%s/chrome-profile", user->pw_name);
  sys(execlp(chrome_cmd, chrome_cmd, buffer, NULL));
}
