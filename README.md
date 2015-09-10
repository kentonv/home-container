# Home Container

This is a little program which "containerizes your home directory" on Linux -- that is, it runs programs in containers which replace your home directory with a container-private directory. The main intended use case is your web browser. This has two main uses:

1. **Hide your sensitive files from apps that don't need them.** For example, [a recent Firefox bug](https://blog.mozilla.org/security/2015/08/06/firefox-exploit-found-in-the-wild/) allowed remote exfiltration of files from the local filesystem and was used in the wild to collect SSH private keys; using a home container would prevent this bug from finding said keys.
2. **Run a program with multiple "profiles", even if the program itself doesn't support any notion of "profiles".** For instance, you can use two different browser profiles to log into the same web site using two different accounts at the same time. (Browsers typically have built-in support for this, but by using home containters instead you get added security: a browser breach in one profile will have a harder time stealing cookies from another profile.)

Note that a home container is **not a sandbox**. An attacker who can run arbitrary native code can easily break out of the container in a number of ways. However, bugs which allow filesystem exfiltration _without_ allowing arbitrary code execution are common, due in part to the complexity of the web platform: there are many situations in which Javascript running in your browser is _supposed_ to be able to read your filesystem, meaning there are lots of chances for the browser to screw up. There are comparatively few situations where Javascript is supposed to be able to execute non-sandboxed code, and comparatively lots of effort spent making sure it can't.

## Dependencies

Linux, `gcc`, and `make`. That's it.

(For `browser-container-launcher.sh`, you'll also need Zenity.)

## Usage

1. Build with `make`. Note that you will be prompted for your sudo password in order to make the binary suid-root (see FAQ below for why).
2. Move (don't copy, lest you lose the suid bit) the `home-container` binary to somewhere in your `PATH`.
3. Run `home-container name command` to run `command` in a container named `name`. Everything you run in the same-named container will see the same version of your home directory, which starts out empty.

Your containers will be stored under `~/.home-container` in your real home directory.

You can optionally map directories from your real home directory into the container with the `-w` (writable) and `-r` (read-only) flags; run with `--help` for details.

For the specific purpose of running a browser, try the included shell script, `browser-container-launcher.sh`; see comments in the script file for details.

## Author

I, [Kenton Varda](https://keybase.io/kentonv), am the lead developer of [Sandstorm.io](https://sandstorm.io), a ridiculously secure (and radically easier) way to run web apps on your own server. If `home-container` interests you, you may also be interested in reading about [Sandstorm's security model](https://docs.sandstorm.io/en/latest/developing/security-practices/).

## FAQ

### Why run my browser in a container?

Your web browser is the most likely entry point to your desktop for a remote attacker. You use it to connect to random untrusted servers far more often than any other program, and the things it does with the data obtained from those server are far more complex than any other program. Your browser is also likely the program you run whose developers have taken the most care to ensure that it is secure -- but no one is perfect.

The purpose of `home-container` is to add an additional layer of defense against possible bugs in your browser. In particular, it defends against bugs that might otherwise allow a remote attacker to read sensitive files from your hard drive. Such a bug could be used, for example, to steal SSH or PGP private keys. [A bug of this nature was recently found in Firefox.](https://blog.mozilla.org/security/2015/08/06/firefox-exploit-found-in-the-wild/)

### What directories can the program still see?

By default, a program running in a home container sees:

* A read-only view of everything *outside* of `/home`, `/var`, and `/tmp`.
* A private writable in-memory tempfs in `/tmp` (different for every invocation of `home-container`).
* A writable view of `/var/tmp`. The rest of `/var` is hidden.
* A private writable view of `/home/<your-username>`, which actually maps to `/home/<your-username>/.home-container/<container-name>` in the real filesystem. The rest of `/home` is hidden.

Additionally, you can set up mappings from the container homedir to your real home directory. The included script `browser-container-launcher.sh` sets up the following mappings:

* `Downloads` (read-write)
* `Uploads` (read-only)
* `Pictures` (read-only)
* `.config/fontconfig` (read-only)
* `.local/share` (read-only)

### Won't my cookies still be vulnerable?

Yes. But, you can reduce risk by using different profiles to log into sensitive sites vs. suspicious sites. Each profile will only be able to see its own cookies.

### Why not use a VM?

You could do that, but a VM is much more costly in terms of resources, will likely run slower, and tends to disrupt things like OpenGL, which you probably want to work in your browser. The namespace approach has essentially zero performance overhead. Also, see the next question.

### Why not use Docker/Rocket/OCI/LXC/etc.?

Those container engines are meant for containerizing the _entire_ filesystem, not just your home directory. For desktop apps, that's not normally what you want: you actually want the app to communicate with the rest of your desktop, use the correct graphics drivers, etc.

### Why not use SELinux?

SELinux implements access control, not containerization. You could perhaps use it to limit what apps can see your SSH keys, but you cannot use it to create an entirely new, empty workspace for an app or implement "profiles".

You can, of course, stack SELinux policies on top of home-container for even more protection.

### Why not use Firejail?

[Firejail](https://github.com/netblue30/firejail) is a much more advanced attempt to solve similar problems. Unlike home-container, Firejail actually aims to be a sandbox that arbitrary code cannot break out of, and they have implemented quite a few measures in that direction. The project is definitely worth keeping an eye on as an eventual way to securely sandbox arbitrary apps.

However, as it stands today, Firejail's sandboxing probably does not provide much practical security compared to home-container, because:

* Out-of-the-box, Firejail's sandbox is not complete. For example, as of this writing, [they do not isolate X11](https://github.com/netblue30/firejail/issues/57). Any X app can arbitrarily keylog and spoof events to any other X app on the same desktop, making it trivial for a desktop app to break out of the sandbox. With careful setup involving running a separate X server for the sandbox with some sort of remote desktop protocol, you might be able to make something secure, but there's a lot of complexity here that's easy to screw up. Usually, for sandboxing to be practical, you need a platform that is designed for it -- the web platform is, whereas the Linux desktop platform is not.

* Currently, it is not possible to run Chromium inside a seccomp sandbox, as doing so would break Chromium's own ability to create its own sandbox, due to its current suid-sandbox approach. Therefore, Firejail's default configuration for Chromium does not set up much of a sandbox at all. (Chromium, of course, sets up its own seccomp sandbox internally. The Chrome team literally invented seccomp-bpf sandboxing.)

Meanwhile, Firejail has some disadvantages vs. home-container:

* Firejail configs appear to be blacklist-y rather than whitelist-y. If you have secrets stored in your home directory that Firejail doesn't know about, they won't be hidden by default.

* home-container is small enough that you could actually review it line-by-line. Firejail is not.

### Doesn't my browser have its own sandbox?

Yes. But, the web platform is large and complicated.

For example, scripts loaded by HTML files on your hard drive (`file:` scheme) have the ability to read other files from the local drive. That means that your browser's sandbox has to implement the ability for scripts to read files, and then has to implement policy to decide when they are allowed to do so. That policy could have bugs.

Or perhaps more likely, an extension might request local file access permission. If that extension has bugs, a malicious web site might trick it into uploading files from your system.

`home-container` is only a few hundred lines of C code with no dependencies. It implements a simple policy which is easily understood and can easily be reviewed in full. Using it adds an additional layer of defense in case of certain types of bugs in the browser itself. It cannot defend against all browser bugs, but it can defend against at least some kinds of bugs actually seen in the wild.

### Is home-container a sandbox?

No!

Malicious native code can escape the container in a number of ways:

* Hijacking your X session and spoofing keypresses to other apps.
* Making desktop requests over dbus.
* Talking to your ssh-agent.
* Exploiting a bug in the Linux kernel for privilege escalation (new bugs are typically found monthly or even weekly; when did you last reboot?).
* Exploiting a bug in a suid-root binary on your system (these are also sadly common).
* Probably many other ways.

Fundamaentally, the Linux desktop platform is not designed to be sandbox-able as-is, and a platform that isn't designed to be sandbox-able likely cannot be forced into any usable sandbox transparently. home-container therefore only aims to protect against bugs in which the app is **not** executing outright malicious code, but rather trusted-but-buggy code is tricked into accessing files it is not supposed to.

### Why does it need to be suid-root?

Historically, setting up Linux mount namespaces has required root privileges. Because of this, Chromium uses a setuid binary to set up its own sandbox.

However, since approximately kernel version 3.12, it is now possible to use "user namespaces" to create namespaces without root privileges. Unfortunately, though, inside such a user namespace, setuid binaries won't work, and therefore if we ran Chromium in such a namespace, Chromium's own sandbox would break. We obviously don't want that, therefore home-container itself uses the setuid approach for now, to avoid breaking Chromium.

The Chrome team is currently working on transitioning to user namespaces. Once they do, `home-container` will be updated to no longer require suid-root (and could also perhaps enable some basic sandboxing of its own like seccomp filtering and `NO_NEW_PRIVS`). See: https://code.google.com/p/chromium/issues/detail?id=312380

While I do not think `home-container`'s suid privileges can be exploited, I nevertheless do not recommend installing `home-container` as a suid binary on a multi-user machine with possibly-malicious users, as I have not been focusing on this threat model.
