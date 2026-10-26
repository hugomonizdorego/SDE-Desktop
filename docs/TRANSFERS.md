# Copying and moving: the dialog and safe copying

Since 0.2.0 Alpha 3 the SDE desktop (drops and Paste, `src/desktop`) and
Liria (its file operations window) show copies, moves, deletions and the
Trash in one dialog of SDE's own design, and a copy to a USB stick, an SD
card, an external disk or a network share is **on the drive when the dialog
says it is done**: the drive can be pulled out at once.

## The dialog

```
 ┌─────────────────────────────────────────────────────────────┐
 │        [folder]      a penguin carrying a paper    [folder] │
 │  Copying 3 items to Backup                        ⏸   ✕     │
 │  ███████████████████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░           │
 │  780 MB of 1.2 GB · 22 MB/s · About 40 seconds left         │
 │  holiday.mov                                                │
 │  ⌄ Details      From  Videos                                │
 │                 To    Backup on SANDISK                     │
 │                 Items 2 of 3                                │
 │  ─────────────────────────────────────────────────────────  │
 │  1 more operation                                           │
 │  Moving 12 items to Documents          ████░░░ 35 %  ⏸  ✕   │
 └─────────────────────────────────────────────────────────────┘
```

* **The picture.** A penguin takes a paper out of the source folder, carries
  it on its head to the destination folder and drops it in, again and again
  while the operation runs. For a move the source folder empties as the
  operation goes on; for Delete and the Trash the destination is a bin whose
  lid opens. Paused, the penguin sits down with its paper (pause bars over
  it; queued in Liria: a small clock). While the last data is written to a
  drive it pushes the last paper into the folder. Done: a little jump with
  the flippers up and a green check mark. Failed: the paper on the ground,
  the head tilted, a question mark.
  The picture is SDE's own drawing in the style of SDE's icons (flat fills,
  a dark outline, the colours of Liria's folder icon); it is not Tux and not
  someone else's animation. It is drawn as vectors with cairo
  (`src/common/transfer_scene.h`, plain C shared by both programs), so it is
  sharp at every size and scale, in light and dark themes.
* **Motion.** About 30 frames per second, only while the picture is visible
  and something moves: it stops when the operation is paused or queued,
  after the jump, while the window is minimized, and with reduced motion
  (`[aero] animations=false` or `[appearance] low_resource=true` in
  sde.conf, `SDE_REDUCED_MOTION=1`; Liria also follows
  `gtk-enable-animations`), where every state has a still picture. Only the
  picture is repainted. `sde-transfer-demo` on Xvfb: 2.7 % of one CPU core
  while the penguin walks, 0.9 % when it stands still (the texts change ten
  times a second either way), so the animation costs about 2 % of a core.
* **Texts.** The headline says what goes where ("Copying 3 items to
  Backup", "Moving ...", "Deleting ...", "Moving 3 items to the Trash"),
  the line under the bar how much of how much, how fast and how long ("780
  MB of 1.2 GB · 22 MB/s · About 40 seconds left"; the speed is smoothed
  over about 2 s and the time left weighs the average and the current speed,
  `src/common/transfer_model.h`), the line under it the item being copied.
  Paused: "780 MB of 1.2 GB · Paused". The window title is the progress
  and the headline ("42% · Copying 3 items to Backup").
* **Details** shows where from, where to (with the drive: "Backup on
  SANDISK") and how many items are done.
* **Several operations.** One window holds every operation: the first one
  with the picture, the others under it as the queue, each with its bar,
  Pause and Cancel (Liria also Queue). Closing the window cancels what still
  runs.
* **The end.** An operation that ends well shows the jump and "Copied 3
  items to Backup" with, for a removable drive, **"The files are on SANDISK
  — you can remove it."**, and goes after a few seconds (three; six when it
  names a drive). On a FAT drive it adds: "Windows may offer to scan it as
  it was not ejected; the files are complete." (see below). A failed
  operation stays, with its errors, until its cross is clicked. A cancelled
  one goes at once — after its last data is on the drive.
* **Writing the last data.** When the last pieces are still on their way
  the line says "Writing the last data to SANDISK…" and "Do not remove the
  drive yet."; Pause is off then (the last data must reach the drive). With
  the safe writer this takes about a second at most (see below).
* **Small screens.** On 1024x600 at 150 % the picture gets smaller so the
  window with its details and a queued operation fits.
* **Keyboard.** In the SDE dialog no button has a focus ring until the
  keyboard is used; Tab then goes through Pause, Cancel and Details.

The SDE-FLTK dialog is `sde::TransferDialog` (`src/common/transfer_dialog.*`,
see [UI-KIT.md](UI-KIT.md#copy-and-move-progress-transferdialog)); Liria's
window is `libliria-private/liria-sde-progress.c` (an overlay, see
[SUITE.md](SUITE.md)). Both use the same picture and model and the same
words.

## Safe copying

### The problem

Linux writes data into memory (the page cache) first and to the drive
later: up to about 20 % of the memory can wait there, and the kernel writes
it out when a lot has piled up or after about 30 seconds. A copy to a USB stick therefore
reported "done" while most of it was still in memory: the bar, the speed and
the time left were wrong, Eject then waited for minutes, and a stick pulled
out after "done" lost or corrupted files. Measured on a drive limited to
20 MB/s: 200 MiB were "copied" after 1.4 s; the drive had them after 12 s.

### What SDE does

`src/common/safe_copy.h` (plain C, shared by the desktop's
`src/desktop/file_ops.cpp` and Liria's `liria-sde-copy.c`, where
`suite/redesign.py` copies it in as `liria-sde-safe-copy.h`) writes every
copy to such a drive through a writer that

* **sends each piece to the drive as soon as it is written**
  (`sync_file_range (SYNC_FILE_RANGE_WRITE)`) and keeps only a small window
  in flight: about **one second of the drive's own speed**, 4 to 64 MiB,
  measured as it goes, and at most 64 pieces (a small file is one piece).
  When more is in flight, or a piece has been on its way for 0.3 s, it
  waits for the oldest piece. **The progress counts only pieces the drive
  has taken** (and none after an error of the drive), so the bar, the speed
  and the time left are the drive's from the start, also for many small
  files;
* **makes everything durable at checkpoints**, every 5 seconds between files
  and at the end: the window drained, `syncfs ()` (data and metadata:
  directory entries, the FAT, the journal) and `fsync ()` of the destination
  folder, which on FAT also empties the drive's own write cache. **An
  operation is done only after its last checkpoint** — also after a Cancel,
  for what was written up to then. The last checkpoint is short, because at
  most the window is left;
* **moves**: a source is deleted only after a checkpoint made its copy
  durable (Liria: in batches, moved folders after their files; the desktop:
  after each item), so a stick pulled out during a move never loses a file.
  When the drive fails, the sources stay;
* **deletions, the Trash and emptying the Trash** on such a drive (Liria)
  are written to it before the operation ends ("Writing the last data
  to …");
* **user-space and network file systems** (ntfs-3g and exFAT through FUSE
  keep the data in the cache of the block device; NFS, SMB, SSHFS) confirm
  a window with `fdatasync ()` instead;
* keeps the holes of sparse files, drops pieces the drive has taken from the
  page cache (a large copy does not push the programs' data out of memory),
  removes a cancelled or failed file, and refuses files of 4 GiB or more on
  FAT before it starts (alpha15).

Liria copies local regular files this way and keeps GIO for the rest:
symbolic links, folders and special files (`g_file_copy`), the metadata
(`g_file_copy_attributes`: permissions, times, extended attributes, SELinux
labels), the conflict and rename questions, and non-local URIs (`smb://`,
`sftp://`, MTP), whose copies are on the server when GIO says so and which
cannot be synced beyond that. The desktop keeps permissions, times and the
user's extended attributes itself.

### Which destinations

`sde_dest_probe ()` finds the mount of the destination in
`/proc/self/mountinfo` and its device in `/sys`:

* **removable and external drives**: USB, SD cards (not the computer's own
  eMMC), FireWire, memory stick readers, a disk with removable media, a
  device behind an external port (Thunderbolt and USB4 docks and
  enclosures), also under LUKS, LVM or RAID;
* **network shares**: NFS, SMB/CIFS, SSHFS, WebDAV, 9p, Ceph, ...;
* **the file systems of sticks and external disks** wherever they are: FAT,
  exFAT, NTFS, UDF, HFS+;

and the name for the messages: the drive's label ("SANDISK"), else the name
of the mount point.

**Internal disks keep the page cache** — the measurements below show the
safe writer makes "done" come later there for copies that fit in memory,
without a safety a user could notice (an internal disk is not pulled out).
sde.conf decides, and the environment overrides it:

```ini
[files]
safe_copy = auto     # auto (default) | always (also internal disks) | never (the page cache everywhere)
```

`SDE_SAFE_COPY=auto|always|never` (the older `SDE_LIRIA_SAFE_COPY` too).

### Errors

A full drive ("There is not enough space on SANDISK."), a read-only one
("SANDISK is read-only.") and a drive pulled out or failing ("Could not
write to SANDISK: the drive was removed or does not work.") stop the
operation with their message; the file being written is removed. When the
drive fails at the last checkpoint, the desktop removes the copies of that
operation (they are not reliably there) and Liria says that not everything
could be written; the sources of a move stay in both.

### FAT and the "dirty" bit

Linux marks a FAT file system as in use while it is mounted and clears the
mark when it is unmounted. A stick pulled out without Eject keeps the mark:
the files are complete — the safe writer made sure of that — but Windows
may offer to scan the drive. Eject (Liria's sidebar, the panel's drives)
unmounts and clears the mark; it also writes whatever other programs left
in the cache. exFAT clears its mark at every sync, so after a safe copy it
is clean.

### Measurements

`build/tests/sde-safe-copy-bench` copies a folder with the writer and reads
the drive's own count of written bytes (`/sys/block/<dev>/stat`) while it
copies; `tools/safe-copy-check.py` does the same for the desktop's file
operations and for Liria on Xvfb. The drive was a loop device with ext4,
its writes limited with the blkio cgroup (both tools need root and run only
on a test machine).

A drive at 20 MB/s (a USB 2 stick); "done" is when the copy said so,
"on the drive" when the data was really there (for the page cache: after a
`syncfs ()`), "lead" how far the progress was ahead of the drive:

| Data | Page cache: done | on the drive | lead | Safe: done = on the drive | lead | in flight at most |
|---|---|---|---|---|---|---|
| one file of 200 MiB | 1.4 s | 12.1 s | 210 MB | 10.5 s | 0 | 31 MB |
| 2,000 files of 32 KiB | 0.5 s | 3.8 s | 66 MB | 3.4 s | 0 | 1.2 MB |
| 340 files, 1 KiB to 8 MiB (165 MB) | 0.7 s | 8.9 s | 165 MB | 8.3 s | 0 | 21 MB |

The safe writer has the data on the drive *sooner* than the page cache,
because the drive starts at once instead of after the kernel's delay.
The same drive, one large and 400 small files (114 MB), as the user sees it:

| Copy | Says "done" after | On the drive then |
|---|---|---|
| desktop, page cache (0.2.0 Alpha 2) | 0.5 s | 0 % |
| desktop, safe writer | 5.7 s | 100 % |
| Liria, page cache (0.2.0 Alpha 2) | 0.6 s | 0 % |
| Liria, safe writer | 6.2 s | 100 % |

The last phase ("Writing the last data to …") took about one second when
the copy ended with a large file and less with small files.

Internal disks (the same loop device at the speed of a hard disk and of a
SATA SSD; "done" with the page cache, then on the disk, then with the safe
writer):

| Data | 120 MB/s | 500 MB/s |
|---|---|---|
| one file of 200 MiB | 1.2 s, 3.1 s; safe 1.9 s | 0.4 s, 1.0 s; safe 0.6 s |
| 2,000 files of 32 KiB | 0.1 s, 0.6 s; safe 0.6 s | 0.1 s, 0.2 s; safe 0.2 s |
| 340 files (165 MB) | 0.8 s, 2.3 s; safe 1.5 s | 0.1 s, 0.4 s; safe 0.3 s |

So on an internal disk the safe writer would say "done" 1.2 to 6 times
later for copies that fit in memory (it has the data on the disk about as
soon as the page cache does, mostly sooner); larger copies are limited by
the disk either way. That is why
`safe_copy = auto` keeps the page cache there.

### Not covered

* Renaming and new folders on a drive are written within the kernel's
  normal delay (about 30 s); Eject writes everything before it unmounts.
* Extracting an archive onto a drive (Taca) does not go through this writer.
* With the `flush` mount option (udisks2 mounts FAT with it), the kernel
  waits a tenth of a second after each written file is closed, so many
  small files are slow on FAT sticks in every file manager; not measured
  here (the test machine had no FAT driver).

### Tests

* `sde-safe-copy` (ctest, `tests/test_safe_copy.cpp`): the writer against a
  simulated slow drive — the progress never runs ahead of the drive, the
  data in flight stays within the window, done only after the drive has
  everything and the file system and folder were synced, the drive kept
  busy (large and small files, slow sources), FUSE windows, holes, errors
  and Cancel.
* `sde-desktop-model` (ctest, `tests/test_desktop.cpp`): the desktop's file
  operations with a simulated USB stick that is pulled out during a copy,
  after everything was written, and during a move to another file system.
* `tests/test_transfer_dialog.py` (ctest `sde-transfer-dialog`) and
  `tools/liria-progress-smoke.py --prefix <Liria>`: both dialogs on Xvfb —
  the moving and the still picture, the texts, Pause, Details, Cancel, the
  queue, the done and failed states; Liria also a complete copy byte for
  byte and a safe copy's finished state.
* `tools/safe-copy-check.py --i-use-a-test-machine` (root, a test machine
  only): the measurements above on a real, slowed-down drive.
