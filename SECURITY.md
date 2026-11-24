# Security policy

## Reporting a vulnerability

Please report a security problem **privately**, not in a public issue: use GitHub's
[private vulnerability reporting](https://github.com/DeLFuS77/maxpack-ps5/security/advisories/new)
for this repository. Reports in English or Spanish are welcome. Say which MaxPack version and console
firmware you used, and how to reproduce it. You will get an answer as soon as possible, and the fix
and its credit are published together once it is ready.

## What to know about MaxPack's exposure

- MaxPack's page and its API listen on the **local network, on port 8095** of the console, with no
  password: anyone who can reach the console's IP on that network can use it. Use it on a network you
  trust, and do not forward the port to the internet.
- The page answers only when it is opened at the console's IP address (or `localhost`), and refuses
  requests that come from another website (`Host` and `Origin` checks), so a web page you visit
  cannot drive it.
- MaxPack is a payload for a jailbroken PS5: it runs with the console's privileges. Only run
  `maxpack.elf` from this repository's [Releases](https://github.com/DeLFuS77/maxpack-ps5/releases)
  and check it against `SHA256SUMS`. The self-update downloads from this repository's releases only
  and checks the file against the release's `SHA256SUMS` before sending it. The ampr-emu-ng updater
  (off by default), whose `libSceAmpr.sprx` every game loads, downloads from DeLFuS77/ampr-emu-ng's
  releases only and installs the file only when it matches that release's `SHA256SUMS`. Both wait
  until the console has passed Settings > "Check secure connections".

## Out of scope

Problems in the console's firmware, in the jailbreak or in kstuff, and in the other projects MaxPack
works with (ShadowMountPlus, Kraken Mount, ampr-emu-ng upstream), belong to their own projects.
For problems in our ampr-emu-ng fork, see its
[security policy](https://github.com/DeLFuS77/ampr-emu-ng/blob/main/SECURITY.md).
