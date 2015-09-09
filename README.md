# Browser Cordon

This is a little program which runs Google Chrome inside of a Linux mount namespace (often used to implement "containers") in which most of your home directory is hidden from it.

Additionally, it is possible to run multiple Chrome profiles simultaneously such that each instance can only see its own profile's data (e.g. cookies), not the data of any other profile.

## Dependencies

Linux, Chrome, `gcc`, and `make`.

(That's it.)

## Usage

1. Build with `make`. Note that you will be prompted for your sudo password in order to make the binary suid-root (see FAQ below for why).
2. Move (don't copy, lest you lose the suid bit) the `browser-cordon` binary to somewhere in your `PATH`.
3. Run `browser-cordon` to start Chrome in a cordon.
4. To create multiple profiles, specify a profile name as a command-line parameter to `browser-cordon`. (The default is `default`.)

Your profiles will be stored under `~/.browser-cordon`.

## Author

I, [Kenton Varda](https://keybase.io/kentonv), am the lead developer of [Sandstorm.io](https://sandstorm.io), a ridiculously secure (and radically easier) way to run web apps on your own server. If `browser-cordon` interests you, you may also be interested in reading about [Sandstorm's security model](https://docs.sandstorm.io/en/latest/developing/security-practices/).

I am not in any way affiliated with Google or Chrome.

## FAQ

### Why cordon your browser?

Your web browser is the most likely entry point to your desktop for a remote attacker. You use it to connect to random untrusted servers far more often than any other program, and the things it does with the data obtained from those server are far more complex than any other program. Your browser is also likely the program you run whose developers have taken the most care to ensure that it is secure -- but no one is perfect.

The purpose of `browser-cordon` is to add an additional layer of defense against possible bugs in Chrome. In particular, it defends against bugs that might otherwise allow a remote attacker to read sensitive files from your hard drive. Such a bug could be used, for example, to steal SSH or PGP private keys. [A bug of this nature was recently found in Firefox.](https://blog.mozilla.org/security/2015/08/06/firefox-exploit-found-in-the-wild/)

### Why only Chrome? Why not other browsers?

I personally use Chrome because I am most confident in its security model. Their sandboxing techniques go far beyond any other browser. The Chrome team is regularly pushing the boundaries of Linux security (e.g. creating seccomp-bpf, a Linux kernel feature that is now critical to Sandstorm).

However, this code could probably easily be adapted to other major browsers as well, and I would be happy to accept patches along those lines.

### Does this use Docker/Rocket/OCI/LXC/etc.?

No. It uses the raw Linux system calls on which all of those are based. This code has no dependencies.

### What directories can the browser still see?

In the sandbox, your browser will be able to see the following from your home directory:

* `Downloads` (read-write)
* `Pictures` (read-only)
* `.pki` (read-write)
* `.config` (read-only)
* `.local/share` (read-only)

The rest of your home directory will be hidden.

Everything outside of `/home` will be visible but mounted read-only, except for `/var`, which will be hidden, except for `/var/tmp`, which will be visible.

### Won't my cookies still be vulnerable?

Yes. But, you can reduce risk by using different profiles to log into sensitive sites vs. suspicious sites. Each profile will only be able to see its own cookies.

### What about running Chrome in a VM?

You could do that, but a VM is much more costly in terms of resources, and will likely run slower. The namespace approach has essentially zero performance overhead.

### What about running Chrome under Docker (or some other "container" engine)?

This would add a lot of complication for little gain. Normally the purpose of a container is to prevent an application from interacting with the rest of the system -- it gets its own copies of libraries, private IPC, etc. But you _want_ Chrome to interact with your desktop, and you _want_ it to be able to use your graphics drivers, etc. You really don't want it to have its own libraries. You just want to keep it away from sensitive files.

### What about using SELinux?

SELinux can indeed achieve a similar effect to browser-cordon, using a very different approach. I generally feel more comfortable with the cordon approach, but this may be a matter of taste.

### Doesn't Chrome have its own sandbox?

Yes, and it's a good one. But, the web platform is large and complicated.

For example, scripts loaded by HTML files on your hard drive (`file:` scheme) have the ability to read other files from the local drive. That means that Chrome's sandbox has to implement the ability for scripts to read files, and then has to implement policy to decide when they are allowed to do so. That policy could have bugs.

Or perhaps more likely, an extension might request local file access permission. If that extension has bugs, a malicious web site might trick it into uploading files from your system.

`browser-cordon` is only a few hundred lines of C code with no dependencies. It implements a simple policy which can easily be reviewed in full. Using it adds an additional layer of defense in case of bugs in Chrome itself.

### Does this mitigate arbitrary code execution exploits?

Not really.

A hypothetical memory bug in Chrome could still allow an attacker to execute code outside the Chrome sandbox. The code would still be stuck inside `browser-cordon`. However, if the attacker is aware of this, they likely could use any number of different Linux privilege escalation exploits to break out of it. `browser-cordon` currently does not install a seccomp filter nor set `NO_NEW_PRIVS` because doing so would break Chrome's own sandboxing (see next question), so cannot stop the kind of local privilege escalation exploits that are found in Linux on an almost weekly basis. Once the attacker has root privileges, they can trivially exit the cordon.

Of course, `browser-cordon` may provide some security-by-obscurity here: if the attacker does not expect to be stuck in a cordon, they probably won't know to break out of it.

### Why does it need to be suid-root?

Historically, setting up Linux mount namespaces has required root privileges. Because of this, Chrome itself uses a setuid binary to set up its own sandbox.

However, since approximately kernel version 3.12, it is now possible to use "user namespaces" to create namespaces without root privileges. Unfortunately, though, inside such a user namespace, setuid binaries won't work, and therefore if we ran Chrome in such a namespace, Chrome's own sandbox would break. We obviously don't want that, therefore browser-cordon itself uses the setuid approach for now, to avoid breaking Chrome.

The Chrome team is currently working on transitioning to user namespaces. Once they do, `browser-cordon` will be updated to no longer require suid-root (and could also perhaps enable seccomp filtering and `NO_NEW_PRIVS` as mentioned in the previous question). See: https://code.google.com/p/chromium/issues/detail?id=312380

While I do not think `browser-cordon`'s suid privileges can be exploited, I nevertheless do not recommend installing `browser-cordon` as a suid binary on a multi-user machine with possibly-malicious users, as I have not been focusing on this threat model.
