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
    luks unlock <interface> <dev[:part]> <passphrase>

Description
-----------

The *luks* command provides an interface to detect, inspect, and unlock LUKS
(Linux Unified Key Setup) encrypted partitions. LUKS is a disk encryption
specification used for full disk encryption on Linux systems.

This command supports:

* Detection of LUKS encrypted partitions (LUKS1 and LUKS2)
* Display of LUKS header information
* Unlocking LUKS1 partitions with passphrase-based authentication
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

luks unlock
~~~~~~~~~~~

Unlock a LUKS1 encrypted partition using a passphrase. This command:

1. Verifies the partition is LUKS1 encrypted
2. Derives the encryption key using PBKDF2 with the provided passphrase
3. Attempts to unlock each active key slot
4. Verifies the master key against the stored digest
5. Creates a blkmap device providing on-the-fly decryption

After successful unlock, the decrypted data is accessible through a blkmap
device (typically ``blkmap 0``). Standard U-Boot filesystem commands can then
be used to access files on the unlocked partition.

**Currently only LUKS1 is supported for unlocking. LUKS2 unlock is not yet
implemented.**

Supported cipher modes:

* aes-cbc-essiv:sha256 (AES in CBC mode with ESSIV)

interface
    The storage interface type (e.g., mmc, usb, scsi)

dev[:part]
    The device number and optional partition number

passphrase
    The passphrase to unlock the LUKS partition. Note that the passphrase is
    passed as a command-line argument and may be visible in command history.
    Consider using environment variables to minimize exposure.

The unlocked data remains accessible until U-Boot exits or the blkmap device
is explicitly destroyed.

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

    JSON metadata (12288 bytes):
    {
      "keyslots": {
        "0": {
          "type": "luks2",
          "key_size": 64,
          "kdf": {
            "type": "argon2id",
            "time": 6,
            "memory": 1048576,
            "cpus": 4,
            ...
          },
          ...
        }
      },
      "tokens": {},
      "segments": {
        "0": {
          "type": "crypt",
          "offset": "16777216",
          "encryption": "aes-xts-plain64",
          ...
        }
      },
      "digests": { ... },
      "config": { ... }
    }

Display LUKS header information for a LUKS1 partition::

    => luks info mmc 1:1
    Version:        1
    Cipher name:    aes
    Cipher mode:    cbc-essiv:sha256
    Hash spec:      sha256
    Payload offset: 4096 sectors
    Key bytes:      32

Unlock a LUKS1 partition and access files::

    => luks unlock mmc 0:2 mypassword
    Trying to unlock LUKS partition...
    Key size: 32 bytes
    Trying key slot 0...
    Successfully unlocked with key slot 0!
    Unlocked LUKS partition as blkmap device 'luks-mmc-0:2'
    Access decrypted data via: blkmap 0

    => ls blkmap 0 /
                ./
                ../
                lost+found/
          221   README.md
           17   hello.txt
                subdir/
           20   test.txt

    3 file(s), 4 dir(s)

    => cat blkmap 0 /hello.txt
    Hello from LUKS!

Unlock and load a kernel from encrypted partition::

    => luks unlock mmc 0:2 ${rootfs_password}
    Successfully unlocked with key slot 0!

    => ext4load blkmap 0 ${kernel_addr_r} /boot/vmlinuz
    5242880 bytes read in 123 ms (40.6 MiB/s)

    => bootz ${kernel_addr_r} - ${fdt_addr_r}

Configuration
-------------

The luks command is available when CONFIG_CMD_LUKS is enabled.

For LUKS detection and info commands::

    CONFIG_BLK_LUKS=y
    CONFIG_CMD_LUKS=y

For LUKS unlock functionality, additional options are required::

    CONFIG_BLK_LUKS=y
    CONFIG_CMD_LUKS=y
    CONFIG_BLKMAP=y      # For blkmap device support
    CONFIG_AES=y         # For AES encryption
    CONFIG_SHA256=y      # For SHA-256 hashing
    CONFIG_PBKDF2=y      # For PBKDF2 key derivation

Return value
------------

For *detect* and *info*: The return value $? is 0 (true) on success, 1 (false)
on failure.

For *unlock*: The return value $? is 0 (true) if unlock succeeded, 1 (false)
if unlock failed (wrong passphrase, unsupported format, etc.).

See also
--------

* :doc:`blkmap` - Blkmap device documentation
* cryptsetup project: https://gitlab.com/cryptsetup/cryptsetup
* LUKS on-disk format specifications: https://gitlab.com/cryptsetup/cryptsetup/-/wikis/home
