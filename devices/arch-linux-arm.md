# Arch Linux ARM with Perpetua

Perpetua supplies the kernel and the device tree. Arch Linux ARM (ALARM)
supplies an ordinary AArch64 Linux userspace. The phone's existing Android
boot chain loads a device-specific boot image; its initramfs mounts ALARM
from an existing partition and starts systemd. Denial can be installed after
this base system boots and SSH works.

This guide covers that shared procedure. Read the device's
[Phonewave](https://github.com/denialwm/phonewave) README for its supported
hardware variant, permitted partitions, boot image format, firmware, USB
configuration and recovery procedure. Boot image commands and partition
numbers cannot be shared between devices.

## Before installing

Have a working Perpetua RAM rescue with USB SSH, an accessible recovery,
verified backups of the important non-userdata partitions, and an unlocked
bootloader. Confirm that the rescue remains reachable after reconnecting USB.
Check storage, display/touch power coordination and temperatures before
installing a graphical session. Do not infer working charging from USB data.

Choose one existing partition whose contents you intend to erase. On phones
this is often Android `userdata`: formatting it destroys Android user data
and encryption metadata. Do not repartition the device or format a whole
MMC/UFS disk. Keep bootloaders, firmware, calibration and recovery intact.
On A/B devices follow the device's slot restrictions; never switch slots or
write a protected slot just because a generic installation guide does so.

The kernel must have its storage controller, partition parser, ext4 and
initramfs support built in, or include the required modules in the
initramfs. Its configuration also needs systemd's normal kernel interfaces,
including devtmpfs, procfs, sysfs, cgroups, namespaces and UNIX sockets.
USB networking additionally needs the device's controller/PHY/regulators,
configfs and a supported network gadget function such as ECM.

## Download and authenticate the root filesystem

Use the **generic AArch64** archive from the
[official installation page](https://archlinuxarm.org/platforms/armv8/generic)
and an HTTPS endpoint from the
[official mirror list](https://archlinuxarm.org/about/mirrors). Reuse one
local download; do not download a fresh archive for each boot attempt.
For example, with a selected mirror:

```sh
archive=ArchLinuxARM-aarch64-latest.tar.gz
mirror=https://de3.mirror.archlinuxarm.org
curl --fail --location --continue-at - --output "$archive" "$mirror/os/$archive"
curl --fail --location --output "$archive.sig" "$mirror/os/$archive.sig"
```

Do not disable TLS verification for a mirror with a certificate error.
The detached signature is the authenticity check; an MD5 file alone is not.
Verify the signing key against ALARM's
[package-signing page](https://archlinuxarm.org/about/package-signing).
At the time of this guide the build key's full fingerprint is
`68B3537F39A313B3E574D06777193F152BDBE6A6`:

```sh
keyhome=$(mktemp -d)
chmod 700 "$keyhome"
gpg --homedir "$keyhome" --keyserver hkps://keyserver.ubuntu.com \
    --recv-keys 68B3537F39A313B3E574D06777193F152BDBE6A6
gpg --homedir "$keyhome" --fingerprint 68B3537F39A313B3E574D06777193F152BDBE6A6
gpg --homedir "$keyhome" --verify "$archive.sig" "$archive"
sha256sum "$archive"
```

Require a good signature from that independently checked key. Record the
archive URL, SHA-256, signature result and date, since `latest` changes.

## Identify, format and mount the target

Perform writes as root on the device. Recheck its serial/product before
opening a remote shell. Resolve the partition by GPT **PARTUUID**, then
verify its label, size, parent disk identity and unmounted state. Android
recovery and mainline can assign different `mmcblkN` numbers to the same
storage. `/dev/disk/by-partuuid` is available with udev; in a small rescue,
find the matching `PARTUUID=` in `/sys/class/block/*/uevent` and read its
`DEVNAME=`. Stop on no match or multiple matches.

These placeholders must be replaced with the identities you verified:

```sh
rootdev=/dev/disk/by-partuuid/REPLACE_WITH_VERIFIED_GPT_PARTUUID
lsblk -o NAME,SIZE,FSTYPE,LABEL,PARTLABEL,PARTUUID,MOUNTPOINTS "$rootdev"
findmnt --source "$rootdev"       # must not already be mounted
```

Only after choosing to erase that partition:

```sh
mkfs.ext4 -L ALARM -O ^orphan_file \
    -E lazy_itable_init=0,lazy_journal_init=0,nodiscard "$rootdev"
mkdir -p /mnt/alarm
mount -o noatime "$rootdev" /mnt/alarm
blkid "$rootdev"
```

`^orphan_file` retains compatibility with older recovery kernels. Check
other ext4 feature compatibility against your recovery as well. `nodiscard`
avoids an initial whole-partition discard; it does not disable later TRIM.
Record both the filesystem UUID and GPT PARTUUID: formatting changes the
former, while the latter identifies the existing partition.

## Extract with ownership and metadata intact

Use **bsdtar as root**, preserving numeric owners, ACLs and extended
attributes, as required by ALARM. A rescue BusyBox tar is useful for moving
small bootstrap tools, but use native AArch64 bsdtar for the full rootfs.
If the recovery lacks bsdtar or e2fsprogs, prepare a temporary native tools
chroot in RAM from the authenticated archive. Include their ELF loader,
shared-library dependency closure, `mke2fs.conf` and a bind-mounted `/dev`.
An x86 host cannot directly chroot into an ARM64 filesystem without emulation.

The full archive can remain on the host. For example, when bsdtar is
available on the device:

```sh
ssh root@DEVICE 'bsdtar --numeric-owner --acls --xattrs -xzpf - -C /mnt/alarm' \
    < "$archive"
```

Check the pipeline's exit status and available space. Extract directly onto
the mounted filesystem, not into a RAM directory that only looks like it.

## Configure the installed system

On the device, bind `/dev`, mount procfs and bind sysfs below the installed
root before running native tools:

```sh
mount --rbind /dev /mnt/alarm/dev
mount --make-rslave /mnt/alarm/dev
mount -t proc proc /mnt/alarm/proc
mount --rbind /sys /mnt/alarm/sys
mount --make-rslave /mnt/alarm/sys
chroot /mnt/alarm /bin/bash
```

In that shell:

- Put `UUID=YOUR_EXT4_UUID / ext4 defaults,noatime 0 1` in `/etc/fstab`.
  Do not add unrelated Android partitions.
- Set the hostname, timezone and locale. Set a plausible current clock
  before using TLS or checking package signatures; not all phone RTCs work.
- Install your SSH public key in the chosen user's `~/.ssh/authorized_keys`,
  with directory mode 0700 and file mode 0600. Generate persistent SSH host
  keys with `ssh-keygen -A` and record their fingerprints. A rescue host key
  can instead be deliberately converted and retained. Never publish private
  host keys or put them in Phonewave.
- ALARM ships `root` and `alarm` accounts with default passwords. Change or
  lock both before exposing the installation. For key-only SSH, set
  `PasswordAuthentication no`, `KbdInteractiveAuthentication no` and
  `PermitRootLogin prohibit-password` in an sshd configuration drop-in.
  Run `sshd -t` after configuration.
- Run `pacman-key --init` and `pacman-key --populate archlinuxarm`. Retain
  package signature checking.
- Enable `sshd`, `systemd-networkd`, `systemd-resolved` and
  `systemd-timesyncd`. Use `/run/systemd/resolve/stub-resolv.conf` as the
  `/etc/resolv.conf` symlink when using resolved. Initially select
  `multi-user.target`, without automatically starting a graphical session.

## Install the Perpetua modules and device userspace

For the exact release whose Image you will boot, install
`modules-root/lib/modules/<release>/` into ALARM's
`/usr/lib/modules/<release>/`. Verify the release manifest and module hashes.
Keep releases in their own versioned module directory and run
`depmod <release>` when needed. Do not mix an Image with another build's
modules. The archive's generic kernel does not replace the Perpetua boot
image; never flash its Image or DTB using this guide.

Copy the firmware the device actually needs into `/usr/lib/firmware`,
respecting its redistribution terms. Firmware obtained from a private stock
backup stays outside public source control.

Install Phonewave's shared services and device overlay into the mounted
root, following its README (`make install-device DEVICE=... DESTDIR=...`).
Ensure the overlay enables the services it installs. Keep required temporary
bring-up workarounds documented in the device guide; they are not generic
ALARM requirements. Device-specific Mesa environment settings must reach
both the compositor and the apps it launches. Check the Phonewave session
configuration before assuming that an advertised GLES version proves image
uploads and filtering work.

## Make USB SSH independent of the rescue

Systemd must configure the USB gadget and network after switching to ALARM.
A typical Phonewave device overlay supplies a configfs gadget service and a
networkd `.network` file matched by a stable **device-side MAC address**.
The gadget must bind only after its actual UDC appears, and the service must
survive an SSH logout or cable reconnect.

Choose an unused private subnet. Give the phone and host different static
addresses, or run a small DHCP server on the phone. If using networkd's DHCP
server, explicitly disable router and DNS advertisements when this link is
only for SSH. This keeps the host's existing internet connection usable.
Mark a cable-only link `RequiredForOnline=no`. For an installation with no
required online interface, disable an enabled `systemd-networkd-wait-online`
service rather than making each boot wait for a nonexistent internet uplink.
Match the host interface by its gadget host MAC rather than a guessed
`enx...` name. An existing host NetworkManager profile can override DHCP;
check the actual address and route after enumeration.

USB SSH alone does not give the phone internet. Configure deliberate
routing/NAT, Wi-Fi, or an SSH proxy separately. Do not run two DHCP servers
on the same link. With supported OpenSSH clients, a reverse dynamic forward
can give the device a loopback SOCKS proxy without changing host routing:

```sh
ssh -N -R 127.0.0.1:1080 root@DEVICE
# In another device shell, for curl/libcurl-backed downloads:
ALL_PROXY=socks5h://127.0.0.1:1080 curl -I https://archlinuxarm.org/
```

Once networking and the clock work, run a full `pacman -Syu`. Keep ALARM's
userspace updates separate from the device-specific Perpetua boot packaging.

## Package the installed-root boot image

Use the device's existing Phonewave packager and the correct Perpetua release.
Keep its required Android header version, page size, ramdisk compression,
vendor DTBs and AVB layout. Some devices use `boot` plus `init_boot`; others
put kernel and ramdisk together. Do not copy another phone's boot image.

The initramfs should resolve the saved GPT PARTUUID, wait a bounded time for
storage discovery, mount the existing ext4 filesystem, verify `/sbin/init`,
move `/dev`, `/proc`, `/sys` and `/run` into it, then execute `switch_root`
to `/sbin/init`. It must never format anything during boot. Keeping RAM USB
rescue available when mounting the installed root fails helps diagnosis.
A failure after systemd starts still needs the preserved recovery.

Round-trip unpack the generated boot image and verify header fields,
components, hashes and partition size before flashing. Follow the device's
allowed-partition and read-back procedure; keep its recovery untouched.
Unmount installer bind mounts and the root cleanly, and sync before reboot.

## First boot checks

After boot, verify the expected `uname -r`, `/etc/os-release`, ext4 root UUID,
SSH host-key fingerprint, `systemctl --failed`, module/firmware loading, USB
speed and reconnect behavior. Inspect the journal for storage, regulator,
USB and DRM failures. Have the person holding the device check the display,
touch and unusual warmth. Charging and battery reporting are separate
bring-up checks.

Only then install Mesa and the dependencies for Denial, deploy matching ARM64
compositor, Flutter engine and AOT shell artifacts, and enable the session.
For the phone presentation, set `DENIAL_SHELL_PROFILE=mobile` in the session
environment. Set output scaling independently in the output configuration,
for example `scale=DSI-1,1.5`; choose a value for the actual panel.
A working ALARM base does not establish that every phone peripheral works.
