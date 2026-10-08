# aemu_postoffice client

The client half of https://github.com/Kethen/aemu_postoffice (branch `ppsspp`,
commit 9e53363), as the core builds it. GPLv3, see LICENSE.txt.

Changed from upstream:

- Nothing sleeps or yields. Connects wait for the socket once with their
  timeout, blocked sends and receives wait for the socket, and a close stops
  new operations, wakes blocked ones with `shutdown()`, and parks on a
  libretro-common eventcount until those in flight are done.
- `ptp_connect_v4`/`_v6` take a `cancel_fd`: a socket that becomes readable
  to give the connect up, so a close doesn't wait one out.
- The PSP, stdc and test builds, and the server, are left out.
