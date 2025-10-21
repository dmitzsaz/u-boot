.. SPDX-License-Identifier: GPL-2.0+

LUKS (Linux Unified Key Setup)
===============================

Overview
--------

U-Boot provides support for accessing LUKS-encrypted partitions through the
``luks`` command and the blkmap device infrastructure. This allows U-Boot to
unlock and read data from LUKS1-encrypted block devices.

LUKS is a standard for disk encryption that stores encryption metadata in a
header on the encrypted device. It supports multiple key slots, allowing
different passphrases to unlock the same encrypted data.

Currently, U-Boot supports:

* LUKS version 1 (LUKS1)
* AES-256-CBC encryption with ESSIV (Encrypted Salt-Sector IV) mode
* Passphrase-based unlocking via PBKDF2 key derivation
* Reading ext4 and other filesystems from unlocked devices

Supported Cipher Modes
-----------------------

The following LUKS1 cipher configurations are supported:

* **Cipher**: aes
* **Mode**: cbc-essiv:sha256
* **Key sizes**: 128-bit, 192-bit, 256-bit
* **Hash**: sha256 (for PBKDF2)

Commands
--------

See the :doc:`cmd/luks` for full details.

luks detect
~~~~~~~~~~~

Detect whether a partition is LUKS-encrypted::

    luks detect <interface> <dev[:part]>

Example::

    => luks detect mmc 0:2
    LUKS1 encrypted partition detected

This command checks for the LUKS magic bytes and validates the header structure.

luks info
~~~~~~~~~

Display information about a LUKS partition::

    luks info <interface> <dev[:part]>

Example::

    => luks info mmc 0:2
    Version:        1
    Cipher name:    aes
    Cipher mode:    cbc-essiv:sha256
    Hash spec:      sha256
    Payload offset: 4096
    Key bytes:      32

This shows the LUKS header metadata including encryption parameters and the
offset to the encrypted data payload.

luks unlock
~~~~~~~~~~~

Unlock a LUKS partition with a passphrase::

    luks unlock <interface> <dev[:part]> <passphrase>

Example::

    => luks unlock mmc 0:2 mypassword
    Trying to unlock LUKS partition...
    Key size: 32 bytes
    Trying key slot 0...
    Successfully unlocked with key slot 0!
    Unlocked LUKS partition as blkmap device 'luks-mmc-0:2'
    Access decrypted data via: blkmap 0

This command:

1. Reads the LUKS header from the specified partition
2. Derives the encryption key from the passphrase using PBKDF2
3. Attempts to unlock each active key slot
4. Verifies the unlocked key against the master key digest
5. Creates a blkmap device that provides on-the-fly decryption

Accessing Unlocked Data
------------------------

After unlocking, the decrypted data is accessible through a blkmap device.
The device number is shown in the unlock output (typically ``blkmap 0``).

Listing Files
~~~~~~~~~~~~~

Use standard filesystem commands to access the unlocked device::

    => ls blkmap 0 /
                ./
                ../
                lost+found/
          221   README.md
           17   hello.txt
                subdir/
           20   test.txt

    3 file(s), 4 dir(s)

Reading Files
~~~~~~~~~~~~~

Read file contents::

    => cat blkmap 0 /hello.txt
    Hello from LUKS!

Loading Files to Memory
~~~~~~~~~~~~~~~~~~~~~~~~

Load files for further processing::

    => ext4load blkmap 0 ${loadaddr} /boot/vmlinuz
    5242880 bytes read in 123 ms (40.6 MiB/s)

Using with Boot Flows
~~~~~~~~~~~~~~~~~~~~~

LUKS-encrypted partitions can be integrated into boot flows::

    => bootflow scan
    => bootflow list

The bootflow will detect filesystems on unlocked blkmap devices.

Implementation Details
----------------------

Key Derivation
~~~~~~~~~~~~~~

LUKS uses PBKDF2 (Password-Based Key Derivation Function 2) to derive
encryption keys from passphrases. The process:

1. Passphrase + salt → PBKDF2 → derived key
2. Derived key decrypts the AF-split key material
3. AF-merge combines the split key material → master key
4. Master key digest is verified against stored value

Anti-Forensic (AF) Splitter
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

LUKS uses an anti-forensic information splitter to protect key material:

* The master key is split into 4000 stripes
* Each stripe is XORed and hashed with a counter
* All stripes must be recovered to reconstruct the key
* This makes recovery from partial key material extremely difficult

ESSIV Mode
~~~~~~~~~~

ESSIV (Encrypted Salt-Sector IV) generates unique initialization vectors
for each encrypted sector:

1. ESSIV key = SHA256(master_key)
2. For each sector: IV = AES_encrypt(sector_number, ESSIV_key)
3. Data is encrypted: ciphertext = AES-CBC-encrypt(plaintext, master_key, IV)

This prevents watermarking attacks where identical plaintext blocks would
produce identical ciphertext blocks.

Blkmap Device Mapping
~~~~~~~~~~~~~~~~~~~~~

When a LUKS partition is unlocked, U-Boot creates a blkmap device that:

* Maps to the underlying encrypted device
* Performs on-the-fly AES-CBC decryption with ESSIV
* Presents decrypted data as a standard block device
* Supports whole-disk filesystems (no partition table required)

The blkmap device number can be used with any U-Boot command that accepts
block device specifications.

Creating LUKS Partitions for Testing
-------------------------------------

Using cryptsetup on Linux::

    # Create a 60MB file
    $ dd if=/dev/zero of=test.img bs=1M count=60

    # Format as LUKS1
    $ cryptsetup luksFormat --type luks1 \
        --cipher aes-cbc-essiv:sha256 \
        --key-size 256 \
        --hash sha256 \
        test.img

    # Open the LUKS device
    $ cryptsetup open test.img testluks

    # Create filesystem
    $ mkfs.ext4 /dev/mapper/testluks

    # Mount and add files
    $ mount /dev/mapper/testluks /mnt
    $ echo "Hello from LUKS!" > /mnt/hello.txt
    $ umount /mnt

    # Close the LUKS device
    $ cryptsetup close testluks

Using Python with U-Boot Test Infrastructure
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

See ``test/py/tests/fs_helper.py`` for the ``FsHelper`` class::

    from fs_helper import FsHelper, DiskHelper

    # Create encrypted filesystem
    with FsHelper(config, 'ext4', 30, 'test',
                  part_mb=60,
                  encrypt_passphrase='mypassword') as fsh:
        # Add files to fsh.srcdir
        with open(os.path.join(fsh.srcdir, 'hello.txt'), 'w') as f:
            f.write('Hello from LUKS!\n')

        # Create encrypted filesystem
        fsh.mk_fs()

Configuration Options
---------------------

CONFIG_CMD_LUKS
    Enable the ``luks`` command

CONFIG_BLKMAP
    Enable blkmap device support (required for LUKS unlock)

CONFIG_AES
    Enable AES encryption support

CONFIG_SHA256
    Enable SHA-256 hashing support

CONFIG_PBKDF2
    Enable PBKDF2 key derivation

Limitations
-----------

* Only LUKS1 is currently supported (LUKS2 not yet implemented)
* Only AES-CBC-ESSIV cipher mode is supported
* Only passphrase-based unlocking is supported (no key files)
* Unlocked devices are read-only (write support not yet implemented)
* Master key remains in memory after unlocking (not securely erased on exit)

Security Considerations
-----------------------

Passphrase Handling
~~~~~~~~~~~~~~~~~~~

* Passphrases are passed as command-line arguments
* They may be visible in command history
* Consider using environment variables or scripts to minimize exposure

Memory Security
~~~~~~~~~~~~~~~

* Master keys are wiped from stack after use where possible
* Keys remain in blkmap device structure while device is active
* No secure memory protection or key erasure on warm reboot

Intended Use Cases
~~~~~~~~~~~~~~~~~~

U-Boot's LUKS support is intended for:

* Boot-time access to encrypted root filesystems
* Reading configuration from encrypted partitions
* Testing and development of encrypted disk support

It is **not** a replacement for full disk encryption solutions in production
operating systems.

Example Use Cases
-----------------

Encrypted Root Filesystem
~~~~~~~~~~~~~~~~~~~~~~~~~

Boot from an encrypted root partition::

    => luks unlock mmc 0:2 ${rootfs_password}
    => setenv bootargs root=/dev/blkmap0 rootfstype=ext4
    => ext4load blkmap 0 ${kernel_addr_r} /boot/vmlinuz
    => ext4load blkmap 0 ${ramdisk_addr_r} /boot/initrd.img
    => bootz ${kernel_addr_r} ${ramdisk_addr_r} ${fdt_addr_r}

Encrypted Configuration Storage
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Read configuration from encrypted partition::

    => luks unlock mmc 0:3 ${config_password}
    => ext4load blkmap 0 ${loadaddr} /config/boot.conf
    => env import -t ${loadaddr} ${filesize}

Testing
-------

See ``test/boot/luks.c`` for tests:

* LUKS detection on encrypted and non-encrypted partitions
* LUKS header information parsing
* Successful unlocking with correct passphrase
* Rejection of incorrect passphrases
* Blkmap device creation and filesystem access

See Also
--------

* :doc:`cmd/luks` - LUKS command reference
* :doc:`cmd/blkmap` - Blkmap command reference
* :doc:`blkmap` - Blkmap device documentation
* ``test/py/tests/fs_helper.py`` - Filesystem helper for creating test images
* Linux ``cryptsetup`` documentation for LUKS disk format specification
