# SPDX-License-Identifier: GPL-2.0+
#
# (C) Copyright 2025 Canonical Ltd
#
# Argon2 password hashing library
# Based on PHC winner: https://github.com/P-H-C/phc-winner-argon2

obj-$(CONFIG_ARGON2) += argon2.o

argon2-y := argon2_wrapper.o core.o ref.o blake2/blake2b.o
