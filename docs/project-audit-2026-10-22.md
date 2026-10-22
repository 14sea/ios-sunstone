# Project audit, 2026-10-22

Audit baseline: project `92d3681`, XNU checkout `ee82c2a`. Both working trees were
clean. The project repository is `fork/` in the surrounding workspace; `work/xnu`
is the patched source checkout. This review covered repository tools and build
configuration, all active patches for reconstruction, current documentation,
the GPU regression suite and existing local boot evidence. It was not a line-by-line
audit of upstream XNU, vendor firmware or Apple's userland.
Tool fixes and repeatable checks: project commit `7309401`.

## Confirmed issues fixed

| Issue | Resulting behavior | Verification |
| --- | --- | --- |
| `lpextract.py` loops forever when a linear extent runs past EOF | Stops with a nonzero status and identifies the truncated input | Truncated synthetic super image reproduced the original hang; valid extraction and failure now tested |
| `mkdrop.py` checks character count, then packs into a 128-byte path field | Enforces a NUL-free absolute path of at most 127 UTF-8 bytes, rejecting duplicate destinations too | Original 130-byte name silently became 128 bytes; tests cover both the valid 127-byte boundary and rejection |
| `mkpayload.py` silently truncates boot arguments to 607 bytes | Rejects oversized or embedded-NUL arguments before building | Byte boundary, multibyte input, embedded NUL, valid payload and RAMDisk mapping tested |
| `sweet-log.py` accepts an incomplete ring as a decoded log | Rejects short headers/rings and unsupported layouts | Wrapped log round-trip and truncated reads tested |
| `kbuild.sh` loses the packer's exit status through `tail` | Propagates the packer failure to the caller | Complete shell-script test with mocked Git/GitHub/sleep; failing packer returns status 23 |

The workspace's untracked `tools-host/lpextract.py` received the same EOF fix as
the repository copy. The workspace README now explains that its legacy `mkboot.sh`
defaults to one CPU, read-only root and no persistent log; `fork/tools/mkboot.sh`
has the everyday eight-core/read-write defaults. The chain script explicitly
supplies its own boot arguments.

Documentation corrections:

- SM4375 is Snapdragon 4 Gen 1, as identified by the
  [Qualcomm product brief](https://www.qualcomm.com/content/dam/qcomm-martech/dm-assets/documents/Snapdragon-4-Gen-1-Product-Brief.pdf).
- GPU notes now distinguish boot-48's missing result from boot-49/50's confirmed
  400/600 MHz PASS. Earlier failed/pending observations remain in the historical handoff.
- CPU suspend/resume and hotplug are explicitly unsupported: `SweetCPU::quiesceCPU`,
  `haltCPU` and `initCPU(false)` panic. Eight-core initial startup and panel off/on
  are separate, verified capabilities.
- The README documents repeatable local checks. Markdown file targets resolve.

## Checks performed

| Check | Result |
| --- | --- |
| Full active patch replay | 132 patches, 169 affected files, all byte-identical to `work/xnu` |
| Base source | XNU `1b191cb58250d0705d8a51287127505aa4bc0789` (`xnu-8796.141.3`) |
| Script/plist syntax | 22 repository Python files, 12 shell scripts, 2 plists; 7 additional workspace shell scripts passed |
| Documentation | All 9 repository Markdown documents checked for existing local link targets |
| Workflow configuration | Both YAML workflows parsed; kernel workflow uses the active patch directory, excluding parked 0137 |
| Tool regression tests | 10 tests passed, entirely offline and without a device |
| GPU host suite | Passed with UBSan; all 32 CP_PROTECT entries match the local Linux catalog |
| Shim consistency | Shared workspace/repository build inputs byte-identical |
| Isolated shim build | Successful ARM64 compile/link; Android boot-image v3 and v4 produced |
| Existing boot images 49, 50, 53 | Valid v3 image boundaries, ARM Image size and aligned SWEETXNU payload; expected GPU boot arguments |
| Payload compatibility | Updated mkpayload reproduces boot-53's 17,431,600-byte payload byte-for-byte using that image's embedded ADT and its CI kernel |

The full replay uses the three already-staged libpthread/BearSSL/HFS providers
from the local checkout, reversing their project patches before reconstruction.
It verifies patch/source agreement; it does not independently re-fetch and verify
those upstream providers. Historical imported patches still emit whitespace
warnings. The shim retains its known unused `g2a`/`g2b`/`g2c` warnings.
The workspace's current `out/sunstone.adt` differs from boot-53's embedded ADT;
use the embedded tree when reproducing an existing image, rather than a mutable
output file. ADT generation normally supplies a fresh random seed.

## Existing device evidence

Boot-49 and boot-50 both use CI 39748355088, kernel SHA-256 prefix
`2aa6629f4cc98a94`. Logs show six RPM ACKs, passing clock selftests, page flips on,
three attempt-0 recoveries and kernel `PASS`, flags 3, canary `0xc0ffee45`, at
400 and 600 MHz respectively. These are prior device runs, not new boots from
this audit. Local boot-image SHA-256 values match the prefixes in their chain logs:

| Boot | SHA-256 |
| --- | --- |
| 49 | `3953a7d9c1373b296633bf70268b2268907ef72c8d7710b79e8828eac81471dc` |
| 50 | `48a3a64d6af2e7c0bd8ac90bf83e4aaed678d6f80a88085ca021f5d26855a109` |
| 53 | `cfa0465e6779262f1eb0429523ed8fedc4b69768408c9ca32f15656ffdaf1bbd` |

Boot-53's staged image uses CI 39751361470, active patches through 0136 and the
default 300 MHz, with 0137 reverted. Its chain log records flashing and reboot;
there is no boot-53 kernel-log pull in the local evidence reviewed here. Boot-52's
backboardd abort and the deferred ALS HID work remain documented in
[SUNSTONE.md](../SUNSTONE.md).

## Repeat the checks

From the repository root:

```sh
python3 tests/check-project.py --source-root ../work/xnu
python3 -m unittest discover -s tests -p 'test_*.py' -v
python3 tests/check-gpu.py --source-root ../work/xnu --catalog ../research/linux-msm/a6xx_catalog.c
```

Without the local source/reference checkouts, omit `--source-root` and `--catalog`.
No new full XNU link, CI dispatch, remote-host command, phone write or flash was
performed in this audit. The tool fixes still need their next normal build/use.
Remaining product work is the iOS WLAN interface, deferred auto-brightness, GPU
rendering, GPU clock/thermal/bandwidth follow-ups, and the other missing hardware
services listed in [SUNSTONE.md](../SUNSTONE.md#open-items).
