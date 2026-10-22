# Working on ios-sunstone

- Read `SUNSTONE.md` (status, patch list, TODO, open items) and the top of `NOTES.md` (the active
  handoff) before starting. The rest of `NOTES.md` is ios-sweet's history.
- The user and agents work in parallel: the GPU code (`SweetGPU*`, `shim/gpu.c`) is the user's;
  modem, WLAN, ADSP and sensors are the agent's. Review and report in the other's area, do not take
  it over. Take the next free patch number (look at untracked files too) and commit only your own
  paths.
- Update the handoff at the top of `NOTES.md` after each milestone and before stopping: commits, CI
  run IDs, boot numbers, exact results, unfinished work. Distinguish observations from guesses.
- Kernel builds run on GitHub Actions (`xnu.yml`, dispatched with `gh`); the build is
  `-Weverything -Werror`. Test code on the host first where possible (see `tests/`).
- sunstone's ABL has no `fastboot boot`: test images are flashed to `boot_a` (slot a); the Android ROM
  and its recovery stay in slot b. The phone has no USB while XNU runs; logs come from the slot-b
  recovery (`RECOVERY.md`). Keep bootloaders, recovery, modem, persist and the backups intact, and
  confirm each destructive step with the user before running it.
- Keep device dumps, serial numbers, personal paths and host names, vendor firmware/configs and
  downloaded Apple artifacts out of git.
