# Fingerprint authentication

SDE uses a fingerprint reader the standard way of Debian and its
derivatives:

- **fprintd** (package `fprintd`, D-Bus service `net.reactivated.Fprint`)
  talks to the reader through libfprint and keeps the enrolled fingerprints;
- **pam_fprintd** (package `libpam-fprintd`) lets PAM ask fprintd for a
  finger;
- **pam-auth-update** puts pam_fprintd into `/etc/pam.d/common-auth`, which
  the login screen (LightDM), sudo, polkit and the other services include.

Control Center > Fingerprint (`sde-settings --page fingerprint`, under
System after Users & Avatar) enrolls your fingers and chooses where the
fingerprint is used. SDE then accepts a finger wherever the system asks you
to authenticate: the lock screen, the login screen, sudo in a terminal and
polkit's administrator prompts (installing software, changing system
settings, opening a folder as administrator). The password always keeps
working.

## What you need

- A reader that libfprint supports: most readers built into laptops and
  many USB readers ([list of supported devices](https://fprint.freedesktop.org/supported-devices.html)).
  Readers that only work with a vendor's own driver do not appear.
- The packages `fprintd` and `libpam-fprintd`. When they are missing the
  page says so and offers to install them (PackageKit's `pkcon`, else
  Merkadu, else it shows the `apt` command). Unlocking the screen needs
  only `fprintd`; signing in and administrator authentication need
  `libpam-fprintd` too.

## Control Center > Fingerprint

- **Fingerprint reader**: the reader fprintd found (`Manager.GetDevices`),
  its name and whether it is touched or swiped. With several readers a
  choice selects one. "No fingerprint reader was found" or "The fingerprint
  service is not installed" say what is missing.
- **Your fingerprints**: both hands, seen from above. Enrolled fingers are
  filled with the accent color and carry a small fingerprint; click a finger
  (or use Tab and the Left/Right keys) to choose it.
  - **Enroll** (a finger not enrolled yet): fprintd asks polkit first
    (`net.reactivated.fprint.device.enroll`: your own password), then the
    reader wants the finger several times. The page shows the step ("Step 2
    of 5") and what to do after every status: lift and place again, the scan
    could not be read, the finger was not centered, the swipe was too short,
    and at the end enrolled, failed, already enrolled (perhaps as another
    finger or for another user), the reader's storage is full, the reader was
    disconnected, or a driver error. **Cancel** stops it.
  - **Test** checks an enrolled finger (`VerifyStart`): recognized, or not.
  - **Delete** removes the chosen finger's fingerprint, **Delete All** (after
    a confirmation) all of yours on this reader. Both ask for your password
    through polkit, like enrolling.
- **Use the fingerprint for**: the three switches below.

fprintd keeps fingerprints as root-only files (`/var/lib/fprint`), or in the
reader itself (match-on-chip readers); they are not in your home folder.
SDE never sees them: it asks fprintd only "enroll this finger" and "is this
one of the user's fingers?".

## Where the fingerprint is used

| Switch | Who asks | Changed how |
| --- | --- | --- |
| Sign in | the login screen (LightDM, slick-greeter), text consoles (`login`) | the system's PAM configuration, for everyone: pam-auth-update |
| Unlock the screen | SDE Screen Lock | your setting: `[lock] fingerprint` in `~/.config/sde/sde.conf` (default on) |
| Administrator authentication (sudo, polkit) | `sudo`, `sudo -i`, polkit (`pkexec`, PackageKit, AccountsService, the Control Center's and Merkadu's administrator actions) | the system's PAM configuration, for everyone: pam-auth-update |

### Signing in and administrator authentication: pam-auth-update

pam-auth-update builds `common-auth` from the profiles in
`/usr/share/pam-configs`. Debian's profile `fprintd` ("Fingerprint
authentication", from libpam-fprintd) adds pam_fprintd for **every**
service that includes common-auth. To let the two switches differ, SDE
ships two more profiles that limit pam_fprintd to some services with a
`pam_succeed_if` guard:

| Switches | Profile | Services |
| --- | --- | --- |
| both on | `fprintd` (Debian's) | every service that includes common-auth (also `su`, `chsh`, ...) |
| only Sign in | `sde-fingerprint-sign-in` | `login`, `lightdm`, `sddm`, `lxdm`, `xdm` |
| only Administrator authentication | `sde-fingerprint-admin` | `sudo`, `sudo-i`, `polkit-1` |
| both off | none | none |

With only administrator authentication on, common-auth reads:

```
auth  [success=1 default=ignore]  pam_succeed_if.so quiet service notin sudo:sudo-i:polkit-1
auth  [success=2 default=ignore]  pam_fprintd.so max-tries=1 timeout=10
auth  [success=1 default=ignore]  pam_unix.so nullok try_first_pass
auth  requisite                   pam_deny.so
auth  required                    pam_permit.so
```

For other services the guard succeeds and skips the pam_fprintd line; for
sudo and polkit pam_fprintd runs first and a recognized finger jumps past
pam_unix. The module line is Debian's own (one try, ten seconds), so a
finger behaves the same with either profile. The three profiles conflict
with each other in pam-auth-update, so they cannot add pam_fprintd twice.

The switches run `/usr/libexec/sde/sde-fingerprint-pam set --sign-in on|off
--admin on|off` through pkexec: polkit action
`org.debion.sde.fingerprint.configure`, an administrator's password
(`auth_admin_keep`). The helper only runs `pam-auth-update --enable ...
--disable ...` (arguments only, a cleared environment, debconf's
noninteractive frontend) and reads common-auth again to check the result.
pam-auth-update never overwrites a common-auth edited by hand (it says so
and changes nothing); the helper and the page report that instead of
pretending. From a terminal:

```sh
/usr/libexec/sde/sde-fingerprint-pam status
sudo /usr/libexec/sde/sde-fingerprint-pam set --sign-in off --admin on
sudo pam-auth-update --enable fprintd        # Debian's way: both
sudo pam-auth-update --disable fprintd sde-fingerprint-sign-in sde-fingerprint-admin
```

### The lock screen

SDE Screen Lock asks fprintd itself, over D-Bus, while the password field
stays usable: "Touch the fingerprint reader" (or "Swipe your finger across
the fingerprint reader") under the field, a message for each retry, and
"Fingerprint not recognized. Try again." After **five** fingers that are not
recognized it stops: "Too many failed fingerprint attempts. Type your
password to unlock." A key or a click wakes the monitors as usual; a touch
on the reader does too.

Why not pam_fprintd: a PAM stack is serial. pam_fprintd waits up to ten
seconds for a finger before the next module may read the password, and it
cannot be interrupted. So the lock screen:

1. finds the reader that has your fingerprints (as pam_fprintd does) and
   **claims** it for the whole time the screen is locked;
2. runs fprintd's `VerifyStart("any")` and believes `VerifyStatus` only
   from the unique bus name that answered its `Claim` and for the claimed
   device (only root may own `net.reactivated.Fprint`; fprintd itself
   checks polkit's `net.reactivated.fprint.device.verify`, which allows the
   user of the active session);
3. after a recognized finger, runs the account stack of the PAM service
   **`sde-lock-fingerprint`** (`@include common-account`): an expired or
   locked account is not unlocked by a finger either. To forbid unlocking
   with a finger on a computer, replace its account line with
   `account requisite pam_deny.so`.

Because the reader stays claimed, a pam_fprintd in the password's own stack
(`sde-lock` includes common-auth; Debian's profile puts pam_fprintd there)
finds it busy and gives up at once, so the password is checked without
waiting for a finger. For the same reason the lock holds the reader without
verifying when "Unlock the screen" is off but common-auth runs pam_fprintd
for `sde-lock`: off really means off. SDE's own two profiles never include
`sde-lock`.

Switch User releases the reader (the login screen may want it for someone
else); the next key or click on the lock screen takes it again. Reader
errors are retried after a pause that grows; nothing polls. The lock's
lines in `~/.local/state/sde/session.log` start with `fingerprint:`.

### The login screen (LightDM)

With "Sign in" on, LightDM's PAM service (`lightdm`, which includes
common-auth) runs pam_fprintd first: slick-greeter shows "Place your finger
on ...", and the password prompt follows after ten seconds or after a
finger that is not recognized. A keyring that opens with your password at
login (GNOME Keyring) stays locked after a fingerprint login and asks for
the password later; leave "Sign in" off if that bothers you.

### sudo in a terminal

With administrator authentication on, `sudo` prints "Place your right index
finger on ..." and waits up to ten seconds; then, or after a finger that is
not recognized, it asks for the password as usual.

### polkit: administrator prompts

SDE's polkit agent (`sde-polkit-agent`) shows pam_fprintd's message with a
fingerprint symbol in the place of the password prompt and keeps the field
open: "Or type your password, then press Enter." PAM asks for the password
only once pam_fprintd stops waiting; a password typed and confirmed before
is answered then, but only to an echo-off password prompt (pam_unix's),
never to another module's question such as a one-time code.

Liria's "Open as Administrator" runs Liria through `sudo` in a terminal;
Merkadu, the Software Updater, Users & Avatar and the other administrator
actions of the Control Center go through polkit. Both use the fingerprint
through PAM.

### Folders

Linux has no standard lock for single folders: who may open a folder is
decided by its owner, group and permissions, and encrypted folders
(gocryptfs, fscrypt) are opened with a passphrase that is a key, which a
fingerprint is not. SDE therefore does not invent a per-folder lock. A
folder "that needs authentication" is one opened as administrator, and
that authentication (sudo or polkit) accepts the fingerprint as described
above.

## Troubleshooting

- `fprintd-list $USER` lists your enrolled fingers; `fprintd-verify` tests
  one from a terminal. `journalctl -u fprintd` shows the reader's errors.
- "The fingerprint reader is in use by another program": a login screen,
  a lock screen or an administrator prompt is waiting for a finger. Try
  again when it is done.
- The page says pam_fprintd was put into common-auth by hand: edit
  `/etc/pam.d/common-auth` yourself, or start over with
  `sudo pam-auth-update --force` (it rewrites the file from the profiles).
- To turn the fingerprint off everywhere: switch everything off on the page,
  or `sudo pam-auth-update --disable fprintd sde-fingerprint-sign-in
  sde-fingerprint-admin` and `[lock] fingerprint=false`.

## Files

| File | Package | What |
| --- | --- | --- |
| `/usr/libexec/sde/sde-fingerprint-pam` | sde | the root helper (pkexec) that switches the profiles |
| `/usr/share/polkit-1/actions/org.debion.sde.fingerprint.policy` | sde | its polkit action |
| `/usr/share/pam-configs/sde-fingerprint-sign-in`, `sde-fingerprint-admin` | sde | SDE's pam-auth-update profiles (`Default: no`) |
| `/etc/pam.d/sde-lock-fingerprint` | sde | the lock screen's account check after a finger |
| `/usr/share/pam-configs/fprintd` | libpam-fprintd | Debian's profile |
| `net.reactivated.Fprint` | fprintd | the fingerprint service |

## Tests

- `sde-fingerprint-model` (`tests/test_fingerprint.cpp`): every enroll and
  verify status and fprintd's errors in words, PAM's lines and which
  services pam_fprintd runs for, the PAM state and plans for every switch
  combination, SDE's profiles as shipped, the lock's account check.
- `sde-fingerprint-pam` (`tests/test_fingerprint_pam.py`): the helper with
  the real pam-auth-update on a fake root with private debconf databases.
- `sde-fingerprint-ui` and `sde-fingerprint-lock`: the page and the lock
  screen on Xvfb against `sde-fake-fprintd` (`tests/fake_fprintd.cpp`), a
  fake fprintd on a private bus with scripted fingers.

No test uses a real reader or a real fingerprint; on real hardware check
enrolling, the lock screen, LightDM, sudo and a polkit prompt.
