# XNU for sunstone

Built on GitHub's macOS runners (macos-14, Xcode 15) by `.github/workflows/xnu.yml`, using
[blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build) (MIT) at a pinned commit.

- `macos_version=13.5` -> xnu-8796 (the iOS 16 generation of the iPhone X userland), machine
  config `SWEET`, kernel config `DEVELOPMENT`.
- `patches/*.patch` are copied into the build script's patch dir and applied in order with
  `git apply`; `patches-topaz/` and `patches-unused/` are not built.
- Output: the `xnu-kernel` artifact with `kernel.development.sweet` (+ `SHA256SUMS`).

```sh
gh workflow run xnu.yml -R YOU/ios-sunstone --ref sunstone
gh run download RUN_ID -R YOU/ios-sunstone -n xnu-kernel -D xnu/out
```
