.. SPDX-License-Identifier: GPL-2.0+:

.. index::
   single: luks (command)

luks command
============

Synopsis
--------

::

    luks detect <interface> <dev[:part]>
    luks info <interface> <dev[:part]>

Description
-----------

The *luks* command provides an interface to detect and inspect LUKS
(Linux Unified Key Setup) encrypted partitions. LUKS is a disk encryption
specification used for full disk encryption on Linux systems.

This command supports:

* Detection of LUKS encrypted partitions (LUKS1 and LUKS2)
* Display of LUKS header information
* Access to decrypted data via blkmap devices

The LUKS format uses a distinctive header containing:

* Magic bytes: "LUKS" followed by 0xBA 0xBE
* Version information (16-bit big-endian)
* Encryption metadata (cipher, mode, hash specification)

luks detect
~~~~~~~~~~~

Detect whether a specified partition is LUKS encrypted and report its version.

interface
    The storage interface type (e.g., mmc, usb, scsi)

dev[:part]
    The device number and optional partition number. If partition is omitted,
    defaults to the whole device.

luks info
~~~~~~~~~

Display detailed header information for a LUKS encrypted partition. This
subcommand reads the LUKS header and displays format-specific metadata.

For LUKS1 partitions, the following information is displayed:

* Version number
* Cipher name (encryption algorithm)
* Cipher mode (e.g., xts-plain64)
* Hash specification (e.g., sha256)
* Payload offset (in sectors)
* Key bytes (key size)

For LUKS2 partitions, the following information is displayed:

* Version number
* Header size (in bytes)
* Sequence ID (metadata update counter)
* UUID (partition identifier)
* Label (optional partition label)
* Checksum algorithm
* Full JSON metadata containing:

  - keyslots: Encryption key slot information including KDF parameters
  - tokens: Optional token metadata
  - segments: Encrypted data segment descriptions
  - digests: Master key digest information
  - config: Configuration parameters

interface
    The storage interface type (e.g., mmc, usb, scsi)

dev[:part]
    The device number and optional partition number. If partition is omitted,
    defaults to the whole device.

Examples
--------

Check if MMC device 0 partition 2 is LUKS encrypted::

    => luks detect mmc 0:2
    LUKS2 encrypted partition detected

Check a USB device partition::

    => luks detect usb 0:1
    Not a LUKS partition (error -2: No such file or directory)

Display LUKS header information for a LUKS2 partition::

    => luks info mmc 0:2
    Version:        2
    Header size:    16384 bytes
    Sequence ID:    3
    UUID:           7640da3a-d0a2-4238-9813-4714e7f62203
    Label:
    Checksum alg:   sha256

Display LUKS header information for a LUKS1 partition::

    => luks info mmc 1:1
    Version:        1
    Cipher name:    aes
    Cipher mode:    cbc-essiv:sha256
    Hash spec:      sha256
    Payload offset: 4096 sectors
    Key bytes:      32

Configuration
-------------

The luks command is available when CONFIG_CMD_LUKS is enabled.

For LUKS detection and info commands::

    CONFIG_BLK_LUKS=y
    CONFIG_CMD_LUKS=y

Return value
------------

For *detect* and *info*: The return value $? is 0 (true) on success, 1 (false)
on failure.

See also
--------

* cryptsetup project: https://gitlab.com/cryptsetup/cryptsetup
* LUKS on-disk format specifications: https://gitlab.com/cryptsetup/cryptsetup/-/wikis/home
