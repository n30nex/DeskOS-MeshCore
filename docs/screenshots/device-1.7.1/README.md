# DeskOS 1.7.1 physical D1L captures

These 480x480 framebuffer captures came from the attached SenseCAP Indicator
D1L after installing the exact GitHub Actions production package.

- Firmware version: `1.7.1`
- Source commit: `00cecf95459faa1dd7ab3513ccd577d634e4e0ab`
- GitHub Actions run: `31752151745`
- Release profile: `full_feature`
- SD history mode: `conditional`
- Capture screens: Home and Contacts

The post-flash read-only check also confirmed board, UI, NVS, identity, and
radio readiness, retained repeater and room entries, and zero crash-like
records. It transmitted no public RF traffic and changed no settings or
storage.

The captures contain public node labels visible on the device. They do not
contain messages, passwords, private keys, account credentials, Wi-Fi names,
or device serial numbers.
