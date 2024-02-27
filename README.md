# rk-misc-tools
This repository contains miscellaneous boot-related tools for the
RK356x/RK3588 platforms.

Derived from:
 * [tegra-boot-tools](https://github.com/OE4T/tegra-boot-tools)
 * [tegra-eeprom-tool](https://github.com/OE4T/tegra-eeprom-tools)
 * [tegra-fuse-tool](https://github.com/madisongh/tegra-fuse-tool)

## rk-bootinfo
The `rk-bootinfo` tool provides storage, similar to U-Boot environment
variables, for information that should persist across reboots. The variables
are stored (with redundancy) outside of any Linux filesystem.

## rk-otp-tool
The `rk-otp-tool` tool stores a UUID as a 32-character hex digit
string in the non-protected OEM zone of the one-time-programmable
memory on the RK356x/RK3588.  It can be used to assign a unique identifier
to a target device, which doubles as a consistent machine ID for systemd
use on targets that implement an read-only root filesystem. U-Boot can be
patched to retrieve the value from the OTP and include it as part of the
kernel command line.

## rk-update-bootloader
This tool can be used to update the idblock and U-Boot bootloaders, including
redundant copies of each.

## rkvendor-tool
The `rkvendor-tool` tool provides access to the Rockchip-specific
vendor storage data, for getting or setting MAC addresses and the
device serial number.  Uses the ioctl interface provided by the Rockchip
driver.

# Builds
This package uses CMake for building.

## Dependencies
This package depends on systemd, libz, libedit, the UAPI headers from the
Rockchip kernel, and the Rockchip OP-TEE client library and headers.

# License
Distributed under license. See the [LICENSE](LICENSE) file for details.
