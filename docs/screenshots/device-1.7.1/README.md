# DeskOS 1.7.1 physical D1L captures

These 480x480 framebuffer captures came from the attached SenseCAP Indicator
D1L after installing the exact GitHub Actions production package.

- Firmware version: `1.7.1`
- Source commit: `e300f1faa88ddd7941ad1141cf92e7064d2458ec`
- GitHub Actions run: `31755945841`
- Release profile: `full_feature`
- SD history mode: `conditional`
- Capture screens: Home and Contacts

The post-flash read-only check also confirmed board, UI, NVS, identity, and
radio readiness, retained repeater and room entries, and no new crash-like
records. The read-only acceptance check issued no RF, settings, or storage
mutations.

The captures contain public node labels visible on the device. They do not
contain messages, passwords, private keys, account credentials, Wi-Fi names,
or device serial numbers.
