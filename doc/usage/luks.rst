.. SPDX-License-Identifier: GPL-2.0+

LUKS (Linux Unified Key Setup)
==============================

Overview
--------

U-Boot provides support for accessing LUKS-encrypted partitions through the
``luks`` command and the blkmap device infrastructure. This allows U-Boot to
unlock and read data from LUKS1 and LUKS2-encrypted block devices.

LUKS is a standard for disk encryption that stores encryption metadata in a
header on the encrypted device. It supports multiple key slots, allowing
different passphrases to unlock the same encrypted data.

Currently, U-Boot supports:

* LUKS version 1 (LUKS1)
* LUKS version 2 (LUKS2) with PBKDF2 and Argon2 key derivation
* AES-256-CBC encryption with ESSIV (Encrypted Salt-Sector IV) mode
* AES-256-XTS encryption (modern default for LUKS2)
* Passphrase-based unlocking via PBKDF2 or Argon2id key derivation
* Reading ext4 and other filesystems from unlocked devices

Supported Cipher Modes
----------------------

The following cipher configurations are supported:

**LUKS1:**

* **Cipher**: aes
* **Mode**: cbc-essiv:sha256
* **Key sizes**: 128-bit, 192-bit, 256-bit
* **KDF**: PBKDF2 with SHA256

**LUKS2:**

* **Cipher**: aes
* **Mode**: cbc-essiv:sha256 or xts-plain64
* **Key sizes**: 128-bit, 192-bit, 256-bit (CBC); 256-bit, 512-bit (XTS)
* **KDF**: PBKDF2 with SHA256, or Argon2id
* **Hash**: sha256, sha512 (for PBKDF2 and digests)

XTS mode is the modern default for LUKS2 and provides better security properties
than CBC-ESSIV for disk encryption.

Commands
--------

See the :doc:`cmd/luks` for full details.

luks detect
~~~~~~~~~~~

Detect whether a partition is LUKS-encrypted:

::

    luks detect <interface> <dev[:part]>

Example:

::

    => luks detect mmc 0:2
    LUKS1 encrypted partition detected

This command checks for the LUKS magic bytes and validates the header structure.

luks info
~~~~~~~~~

Display information about a LUKS partition:

::

    luks info <interface> <dev[:part]>

Example:

::

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

Unlock a LUKS partition with a passphrase:

::

    luks unlock <interface> <dev[:part]> <passphrase>

Example:

::

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
-----------------------

After unlocking, the decrypted data is accessible through a blkmap device. The
device number is shown in the unlock output (typically ``blkmap 0``).

Listing Files
~~~~~~~~~~~~~

Use standard filesystem commands to access the unlocked device:

::

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

Read file contents:

::

    => cat blkmap 0 /hello.txt
    Hello from LUKS!

Loading Files to Memory
~~~~~~~~~~~~~~~~~~~~~~~

Load files for further processing:

::

    => ext4load blkmap 0 ${loadaddr} /boot/vmlinuz
    5242880 bytes read in 123 ms (40.6 MiB/s)

Using with Boot Flows
~~~~~~~~~~~~~~~~~~~~~

LUKS-encrypted partitions can be integrated into boot flows:

::

    => bootflow scan
    => bootflow list

The bootflow will detect filesystems on unlocked blkmap devices.

Implementation Details
----------------------

Key Derivation
~~~~~~~~~~~~~~

LUKS supports two key derivation functions:

**PBKDF2 (Password-Based Key Derivation Function 2)**

Used by LUKS1 and LUKS2. The process:

1. Passphrase + salt → PBKDF2 → derived key
2. Derived key decrypts the AF-split key material
3. AF-merge combines the split key material → master key
4. Master key digest is verified against stored value

PBKDF2 applies HMAC-SHA256 iteratively (typically 100,000+ iterations) to make
brute-force attacks computationally expensive.

**Argon2id (Modern KDF for LUKS2)**

Argon2id is the winner of the Password Hashing Competition and provides better
resistance to GPU-based cracking attacks. The process:

1. Passphrase + salt → Argon2id(time, memory, parallelism) → derived key
2. Derived key decrypts the AF-split key material
3. AF-merge combines the split key material → master key
4. Master key digest is verified against stored value

Argon2id parameters:

* **time**: Number of iterations through memory
* **memory**: Amount of memory used in KiB (e.g., 1048576 = 1GB)
* **parallelism**: Number of parallel threads/lanes

Argon2id is memory-hard, making it resistant to ASIC and GPU attacks while
remaining fast on CPUs.

Anti-Forensic (AF) Splitter
~~~~~~~~~~~~~~~~~~~~~~~~~~~

LUKS uses an anti-forensic information splitter to protect key material:

* The master key is split into 4000 stripes
* Each stripe is XORed and hashed with a counter
* All stripes must be recovered to reconstruct the key
* This makes recovery from partial key material extremely difficult

ESSIV Mode
~~~~~~~~~~

ESSIV (Encrypted Salt-Sector IV) generates unique initialization vectors for
each encrypted sector:

1. ESSIV key = SHA256(master_key)
2. For each sector: IV = AES_encrypt(sector_number, ESSIV_key)
3. Data is encrypted: ciphertext = AES-CBC-encrypt(plaintext, master_key, IV)

This prevents watermarking attacks where identical plaintext blocks would
produce identical ciphertext blocks.

XTS Mode
~~~~~~~~

XTS (XEX-based tweaked-codebook mode with ciphertext stealing) is the modern
standard for disk encryption and the default for LUKS2. It provides several
advantages over CBC-ESSIV:

1. **Tweakable block cipher**: Each 512-byte sector uses the sector number as a
   tweak
2. **No IV dependency**: Eliminates the need for ESSIV key derivation
3. **Parallel encryption**: Sectors can be encrypted/decrypted independently
4. **Better security**: Resistant to manipulation attacks on ciphertext

XTS operation:

1. Uses two AES keys (concatenated): 256-bit key → two 128-bit keys, or 512-bit
   key → two 256-bit keys
2. For each 512-byte sector:

   * Data unit (tweak) = sector number in little-endian format
   * Plaintext is encrypted using AES-XTS with the data unit

3. Each 16-byte block within the sector uses a different tweak value derived
   from the sector number

XTS is specified in IEEE P1619 and is widely used in modern disk encryption.

LUKS2 JSON Metadata Parsing
~~~~~~~~~~~~~~~~~~~~~~~~~~~

LUKS2 stores its metadata in JSON format within the header. This is handled as
follows:

1. **JSON to FDT Conversion**: The JSON metadata is converted to a Flattened
   Device Tree (FDT) format using ``json_to_fdt()``
2. **ofnode Navigation**: ofnode API is used to access the tree structure
3. **Type-Safe Property Reading**: Properties are read using
   ``ofnode_read_string()`` and ``ofnode_read_u32()`` for type safety

This approach provides several advantages:

* **Robust Parsing**: Uses U-Boot's well-tested FDT/ofnode infrastructure
* **Type Safety**: Explicit type checking for all properties
* **Maintainability**: Clear code structure with proper node navigation
* **No String Searching**: Eliminates fragile string-based parsing

Example navigation path for reading a keyslot's KDF type:

::

    root → keyslots → "0" → kdf → type

The implementation properly handles LUKS2's mixed data types:

* JSON numbers (iterations, time, memory) are stored as 32-bit integers
* JSON string-encoded numbers (offset, size) are parsed from strings
* JSON strings (type, hash, salt) are read directly

Blkmap Device Mapping
~~~~~~~~~~~~~~~~~~~~~

When a LUKS partition is unlocked, U-Boot creates a blkmap device that:

* Maps to the underlying encrypted device
* Performs on-the-fly decryption (AES-CBC-ESSIV for LUKS1, AES-XTS for LUKS2)
* Presents decrypted data as a standard block device
* Supports whole-disk filesystems (no partition table required)

The blkmap device number can be used with any U-Boot command that accepts block
device specifications.

Creating LUKS Partitions for Testing
------------------------------------

Using cryptsetup on Linux
~~~~~~~~~~~~~~~~~~~~~~~~~

**LUKS1 with CBC-ESSIV (legacy):**

::

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

**LUKS2 with XTS and PBKDF2 (modern default):**

::

    # Create a 60MB file
    $ dd if=/dev/zero of=test.img bs=1M count=60

    # Format as LUKS2 with XTS
    $ cryptsetup luksFormat --type luks2 \
        --cipher aes-xts-plain64 \
        --key-size 512 \
        --hash sha256 \
        --pbkdf pbkdf2 \
        test.img

    # Open, create filesystem, and add files (same as LUKS1)
    $ cryptsetup open test.img testluks
    $ mkfs.ext4 /dev/mapper/testluks
    $ mount /dev/mapper/testluks /mnt
    $ echo "Hello from LUKS2!" > /mnt/hello.txt
    $ umount /mnt
    $ cryptsetup close testluks

**LUKS2 with XTS and Argon2id (maximum security):**

::

    # Create a 60MB file
    $ dd if=/dev/zero of=test.img bs=1M count=60

    # Format as LUKS2 with XTS and Argon2id
    $ cryptsetup luksFormat --type luks2 \
        --cipher aes-xts-plain64 \
        --key-size 512 \
        --hash sha256 \
        --pbkdf argon2id \
        --pbkdf-memory 65536 \
        --pbkdf-parallel 4 \
        --iter-time 2000 \
        test.img

    # Open, create filesystem, and add files (same as above)
    $ cryptsetup open test.img testluks
    $ mkfs.ext4 /dev/mapper/testluks
    $ mount /dev/mapper/testluks /mnt
    $ echo "Hello from LUKS2 with Argon2!" > /mnt/hello.txt
    $ umount /mnt
    $ cryptsetup close testluks

Note: Argon2 parameters:

* ``--pbkdf-memory``: Memory in KiB (65536 = 64MB)
* ``--pbkdf-parallel``: Number of parallel threads
* ``--iter-time``: Target time in milliseconds for KDF

Using Python with U-Boot Test Infrastructure
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

See ``test/py/tests/fs_helper.py`` for the ``FsHelper`` class:

::

    from fs_helper import FsHelper, DiskHelper

    # Create LUKS2 encrypted filesystem with PBKDF2 (default)
    with FsHelper(config, 'ext4', 30, 'test',
                  part_mb=60,
                  encrypt_passphrase='mypassword') as fsh:
        # Add files to fsh.srcdir
        with open(os.path.join(fsh.srcdir, 'hello.txt'), 'w') as f:
            f.write('Hello from LUKS!\n')

        # Create encrypted filesystem
        fsh.mk_fs()

    # Create LUKS2 encrypted filesystem with Argon2id
    with FsHelper(config, 'ext4', 30, 'test',
                  part_mb=60,
                  encrypt_passphrase='mypassword',
                  luks_kdf='argon2id') as fsh:
        # Add files to fsh.srcdir
        with open(os.path.join(fsh.srcdir, 'hello.txt'), 'w') as f:
            f.write('Hello from LUKS with Argon2!\n')

        # Create encrypted filesystem
        fsh.mk_fs()

    # Create LUKS1 encrypted filesystem
    with FsHelper(config, 'ext4', 30, 'test',
                  part_mb=60,
                  encrypt_passphrase='mypassword',
                  luks_version=1) as fsh:
        # Add files to fsh.srcdir
        with open(os.path.join(fsh.srcdir, 'hello.txt'), 'w') as f:
            f.write('Hello from LUKS1!\n')

        # Create encrypted filesystem
        fsh.mk_fs()

Configuration Options
---------------------

CONFIG_CMD_LUKS
    Enable the ``luks`` command

CONFIG_BLK_LUKS
    Enable LUKS block device support

CONFIG_BLKMAP
    Enable blkmap device support (required for LUKS unlock)

CONFIG_JSON
    Enable JSON parsing support (automatically selected by CONFIG_BLK_LUKS for
    LUKS2 metadata parsing)

CONFIG_MBEDTLS_LIB
    Enable mbedTLS library for AES-XTS and key derivation

CONFIG_SHA256
    Enable SHA-256 hashing support

CONFIG_SHA512
    Enable SHA-512 hashing support (optional, for LUKS2)

CONFIG_ARGON2
    Enable Argon2 key derivation support (optional, for modern LUKS2)

Limitations
-----------

* Only passphrase-based unlocking is supported (no key files)
* Unlocked devices are read-only (write support not yet implemented)
* Master key remains in memory after unlocking (not securely erased on exit)
* Argon2 support requires CONFIG_ARGON2 to be enabled (adds ~50KB to binary)
* Large Argon2 memory requirements may not be suitable for all embedded systems

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

Boot from an encrypted root partition:

::

    => luks unlock mmc 0:2 ${rootfs_password}
    => setenv bootargs root=/dev/blkmap0 rootfstype=ext4
    => ext4load blkmap 0 ${kernel_addr_r} /boot/vmlinuz
    => ext4load blkmap 0 ${ramdisk_addr_r} /boot/initrd.img
    => bootz ${kernel_addr_r} ${ramdisk_addr_r} ${fdt_addr_r}

Encrypted Configuration Storage
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Read configuration from encrypted partition:

::

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
