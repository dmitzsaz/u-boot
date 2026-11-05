.. SPDX-License-Identifier: GPL-2.0+

Boot selections from the boot loader
====================================

The Aurora boot loader (based on m1n1) can tell U-Boot how a machine is to
boot by properties in ``/chosen`` of the device tree it passes. They are
independent of the SoC and the machine. Without them U-Boot runs its standard
boot scan (``bootflow scan``) over ``boot_targets``.

Each selection only sets defaults: every variable below may be given another
value in a saved environment or by ``preboot``, and a ``bootcmd`` other than
the built-in default is never replaced.

Encrypted /boot
---------------

``aurora,luks-partition`` (string)
    Device and partition of a LUKS2 volume that holds ``/boot``, on the
    interface in ``aurora_luks_if`` (default ``nvme``), e.g. ``"0:3"``.

``aurora,luks-bootargs`` (string, optional)
    Appended to the boot loader's ``/chosen/bootargs`` for the kernel, e.g.
    the arguments that let the initramfs find and unlock the root file
    system.

U-Boot then boots with ``run aurora_luks_boot``:

#. ``run aurora_luks_input`` makes the built-in keyboard available (default
   ``mtpkbd start``; add ``usb start`` for a USB keyboard).
#. ``luks unlock`` asks for the passphrase without echoing it, up to three
   times, and maps the volume read-only as blkmap
   ``luks-<interface>-<dev:part>``. Argon2 and PBKDF2 keyslots are supported,
   with AES-XTS and 512- to 4096-byte sectors.
#. ``bootargs`` becomes ``${aurora_bootargs} ${aurora_luks_bootargs}``.
#. If the volume holds ``${aurora_luks_extlinux}`` (default
   ``/boot/extlinux/extlinux.conf``), it is booted with ``sysboot``. An entry
   without an ``fdt`` line uses the boot loader's device tree; an ``append``
   line replaces ``bootargs``, so include ``${aurora_bootargs}`` and
   ``${aurora_luks_bootargs}`` in it.
#. Otherwise ``${aurora_luks_kernel}`` and ``${aurora_luks_initrd}`` are
   loaded from the volume, the volume is closed and the kernel is started with
   ``booti`` and the boot loader's device tree.

The defaults of ``aurora_luks_kernel`` and ``aurora_luks_initrd`` are
``/boot/Image-j704`` and ``/boot/initramfs-j704-crypt.img``, the names used
by existing installations.

The master key is wiped when the volume is closed, and the blkmap is torn
down on the way to any OS, so that the key is not left in memory the OS
takes over. Nothing is ever written to the volume.

On T8142 (M5) the built-in keyboard runs on the MTP coprocessor, which
cannot be restarted once stopped. ``mtpkbd start`` starts it (with
``CONFIG_APPLE_MTP_KEYB_T8142_HANDOFF``) and U-Boot hands the running
session to the OS (``apple,rtkit-handoff = "mtp-live-v1"``), which has to
adopt it to keep the keyboard and trackpad.

SD card
-------

``aurora,sd-extlinux-conf`` (string)
    Path of an extlinux.conf on the SD card, in partition ``aurora_sd_part``
    (default ``0:1``) of ``mmc``.

U-Boot boots it with ``run aurora_sd_boot`` (``sysboot``), with ``bootargs``
set to ``${aurora_bootargs}`` for entries without an ``append`` line. No other
boot source is tried: the boot loader chose this configuration.

Without the property, the standard boot scan also covers the SD card
(``boot_targets`` contains ``mmc``) and finds an extlinux.conf there.

Encrypted /boot takes precedence when both properties are present.

USB storage
-----------

USB is not started before the boot scan on machines with a built-in
keyboard, since running a Type-C port as a USB 2.0 host can upset a dock
that is in DisplayPort alternate mode. The boot scan reaches USB when it
gets to ``usb`` in ``boot_targets``, after the internal storage. To prefer a
USB drive, set ``boot_targets`` to e.g. ``usb nvme mmc``, or run
``bootflow scan usb`` at the prompt.
