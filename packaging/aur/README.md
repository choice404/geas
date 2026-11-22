# AUR packaging

Geas goes onto the AUR as three packages, and each directory here carries the
PKGBUILD and the generated .SRCINFO its AUR repository is cut from, since an
AUR repository holds those two files and nothing else.

| Package | Builds from | For |
|---|---|---|
| `geas-lang` | the tagged source tarball, `archive/refs/tags/vX.Y.Z.tar.gz` | the default, a release built on your own machine |
| `geas-lang-bin` | the release tarball, `geas-lang-X.Y.Z-x86_64-linux.tar.gz` | a release with nothing compiled, the binary the release workflow built and gated |
| `geas-lang-git` | the head of `main` | following the tree between releases |

So the three install exactly the same files, the compiler, the runtime and its
symlink, the headers, and the std modules, laid out by the one `make install`
target, and they conflict with each other and with the older `geas` package,
which `geas-lang` replaces. The source packages run `make quick` in their
check(), after holding the built binary to the version it is packaged as, and
`geas-lang-bin` checks the version and the runtime symlink of the tarball it
was handed. `geas-lang-git` reads its version from `git describe`, and before
the history carries any tag it falls back to `r<commits>.<hash>`.

## Layout the packages install

| Path | What |
|---|---|
| `/usr/bin/geas` | the compiler |
| `/usr/lib/libgeasrt.so.0` | the runtime, under the SONAME every module names |
| `/usr/lib/libgeasrt.so` | a symlink to `libgeasrt.so.0` for `-lgeasrt` |
| `/usr/include/geas/*.h` | the C ABI headers the emitted C compiles against |
| `/usr/share/geas/std/*.geas` | the std modules the loader's system probe finds |

The installed toolchain needs no environment variables: the compiler probes
for a repository checkout first and falls back to the system paths above.
`GEAS_ROOT` and `GEAS_HOME` still override both, in that order of specificity,
for out of tree development.

## Cutting a release

Now the order matters, since `geas-lang` needs the tag on GitHub and
`geas-lang-bin` needs the release tarball the tag's workflow attaches.

1. Bump `versionString` in `compiler/src/main.cpp`, the `pkgver` of
   `geas-lang` and `geas-lang-bin`, and add the CHANGELOG entry, all in the
   release commit, since check() refuses a binary whose version disagrees with
   the package.
2. Tag the release and push the tag, `git tag vX.Y.Z && git push origin vX.Y.Z`.
   The release workflow in `.github/workflows/release.yml` runs
   `packaging/release.sh` on that tag and attaches
   `geas-lang-X.Y.Z-x86_64-linux.tar.gz` and its `.sha256` to the release, so
   wait for it to go green before the `-bin` step.
3. For each package, clone its AUR repository and copy the files in:

   ```sh
   git clone ssh://aur@aur.archlinux.org/geas-lang.git
   cp packaging/aur/geas-lang/PKGBUILD geas-lang/
   cd geas-lang
   ```

   and the same for `geas-lang-bin` and `geas-lang-git`.
4. The PKGBUILDs in this repository carry `SKIP` for the tarball hashes, since
   the hash only exists once the tag does, so pin the real ones in the AUR
   copies of `geas-lang` and `geas-lang-bin` and regenerate the metadata. The
   hash `updpkgsums` writes for `-bin` must match the `.sha256` on the release.

   ```sh
   updpkgsums
   makepkg --printsrcinfo > .SRCINFO
   ```

   `geas-lang-git` keeps `SKIP`, a git source has no fixed hash, and needs only
   the .SRCINFO regenerated when its PKGBUILD changes.
5. Build each once from scratch to prove it, and lint it:

   ```sh
   makepkg -sfc
   namcap PKGBUILD *.pkg.tar.zst
   ```

6. Commit PKGBUILD and .SRCINFO to each AUR repository and push,
   `git add PKGBUILD .SRCINFO && git commit -m "vX.Y.Z" && git push`.

## Moving off the old geas package

The package was `geas` through 1.0.0, and `geas-lang` takes it over with
`replaces=('geas')`, so pacman offers the swap on the next upgrade. So once
`geas-lang` is up on the AUR, file a merge request for `geas` into it, which
carries the votes, the comments, and the notifications across:

1. Open <https://aur.archlinux.org/packages/geas>, and under Package Actions
   choose Submit Request.
2. Pick Merge as the request type, set the merge into field to `geas-lang`, and
   say in the comment that the package was renamed upstream to sit beside
   `geas-lang-bin` and `geas-lang-git`.
3. A package maintainer closes the request and the `geas` page goes away, while
   AUR helpers already follow `replaces` for anyone with `geas` installed.

Until the request is closed, leave `geas` where it is rather than pushing a
new version to it.

## Version bump checklist

- `versionString` in `compiler/src/main.cpp`
- `pkgver` in `geas-lang/PKGBUILD` and `geas-lang-bin/PKGBUILD`, with `pkgrel=1`
- the placeholder `pkgver` in `geas-lang-git/PKGBUILD`, which `pkgver()`
  rewrites at build time anyway
- the `.SRCINFO` of every directory, `makepkg --printsrcinfo > .SRCINFO`
- the CHANGELOG entry
- `pkgrel` alone climbs when the packaging changes and upstream does not
