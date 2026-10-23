# atlifm packaging

packaging for atlifm 1.0.1 for the mainstream standalone package managers, plus local build scripts that produce real artifacts

everything builds from the source tree in this repository: the fixes below are applied in place, no archives are stored and no patch files are used. toolchains that want an archive get one from `scripts/make-src-tarball.sh`, which recreates `atlifm-1.0.1-fixed.tar.gz` (top-level directory `atlifm-1.0.1/`, project files only) in the repository's `dist/`:

```sh
sh scripts/make-src-tarball.sh
```

the tree is the upstream release
(`https://github.com/1nfys/atlifm/archive/refs/tags/v1.0.0/atlifm-1.0.0.tar.gz`,
sha256 `23bfea39fe33c4a94d91ba2b2ea61827fea41dea0ddd262771d016b0eb6f7fc4`,
verified byte-for-byte against the upstream tag) with the fixes applied

## contents

| directory | package manager | file | publish to |
|-----------|-----------------|------|------------|
| `debian/` | Debian, Ubuntu, derivatives (dpkg/apt) | full `debian/` tree | Debian mentors, Ubuntu PPA |
| `rpm/` | Fedora, RHEL, openSUSE (dnf/zypper) | `atlifm.spec` | Fedora COPR, openSUSE OBS |
| `arch/` | Arch Linux, Manjaro (pacman) | `PKGBUILD` | AUR (optional, account required) |
| `alpine/` | Alpine Linux, postmarketOS (apk) | `APKBUILD` | Alpine aports |
| `appimage/` | distribution-independent | built by `scripts/build-appimage.sh` | GitHub Releases |
| `scripts/` | local artifact builders | `make-src-tarball.sh`, `build-deb.sh`, `build-rpm.sh`, `build-appimage.sh` | local only |
| `common/` | shared assets: `.desktop`, AppStream metainfo, man page | | |

recipes that reference a local archive need it generated first: run `sh scripts/make-src-tarball.sh . arch` (or `... . alpine`) to drop `atlifm-1.0.1-fixed.tar.gz` next to the recipe. the archive checksum is `SKIP` in the `PKGBUILD`; for the `APKBUILD` refresh it once with `abuild checksum`. the CI workflow does all of this automatically.

## fixes applied to the source

two upstream issues are fixed directly in the source (no patch files anywhere in the tree):

1. **DESTDIR-aware install**: the `install(CODE)` block in `CMakeLists.txt` now writes the D-Bus service file into `$ENV{DESTDIR}` as well as `${CMAKE_INSTALL_PREFIX}`. every package manager that stages files (dpkg, rpmbuild, abuild, makepkg) relies on `DESTDIR`; without the fix the file lands in the real `/usr` of the *build machine* and is missing from the package payload.
2. **single desktop main category**: `desktop-file-validate` reports "more than one main category" for the upstream `Categories=System;FileTools;FileManager;Utility;Core;`; the source keeps `Categories=System;FileManager;FileTools;`.

## building locally

all scripts accept a source directory or tarball as the first argument and an output directory as the second (defaults: the repository root and `dist/`); directory sources are packed on the fly with `scripts/make-src-tarball.sh`, and sources without the two fixes are refused.

```sh
sh scripts/build-deb.sh       # -> dist/atlifm_1.0.1-1_<arch>.deb
sh scripts/build-rpm.sh       # -> dist/atlifm-1.0.1-1.<arch>.rpm (+ .src.rpm)
sh scripts/build-appimage.sh  # -> dist/atlifm-1.0.1-<arch>.AppImage
```

`build-deb.sh` works **without debhelper** (plain `dpkg-deb`); `build-rpm.sh` works without a matching RPM dependency database (it retries with `--nodeps`) and without the `%cmake` macros (portable fallback in the spec).

installing what you built:

```sh
sudo apt install ./dist/atlifm_1.0.1-1_<arch>.deb         # Debian/Ubuntu
sudo dnf install ./dist/atlifm-1.0.1-1.<arch>.rpm         # Fedora/RHEL, or zypper in
sudo pacman -U dist/atlifm-1.0.1-1-<arch>.pkg.tar.zst     # Arch (from makepkg)
sudo apk add --allow-untrusted dist/atlifm-1.0.1-r0.apk   # Alpine (from abuild)
chmod +x dist/atlifm-1.0.1-<arch>.AppImage && dist/atlifm-1.0.1-<arch>.AppImage
```

for the canonical, distro-native flows:

```sh
# Debian/Ubuntu (<srcdir> = extracted dist/atlifm-1.0.1-fixed.tar.gz)
cp -r packaging/debian <srcdir>/debian
cd <srcdir> && dpkg-buildpackage -b -us -uc

# Fedora / openSUSE
sh scripts/make-src-tarball.sh . ~/rpmbuild/SOURCES
cp packaging/common/atlifm.metainfo.xml packaging/common/atlifm.1 \
   ~/rpmbuild/SOURCES/
cp packaging/rpm/atlifm.spec ~/rpmbuild/SPECS/
rpmbuild -ba ~/rpmbuild/SPECS/atlifm.spec

# Arch
sh scripts/make-src-tarball.sh . packaging/arch
cd packaging/arch && makepkg -si

# Alpine (in your aports clone)
cp -r packaging/alpine/* aports/testing/atlifm/
sh scripts/make-src-tarball.sh . aports/testing/atlifm
cd aports/testing/atlifm && abuild checksum && abuild -r

```

## publishing to user repositories

the fastest route to real installable packages is the user-repository service of each ecosystem: every service is independent, so the uploads can be done in parallel, and each needs only a one-time account (plus a GPG key and an ssh key for the Ubuntu PPA). the uploads are automated by `.github/workflows/publish-packages.yml`, which takes every credential from repository secrets: no key lands in the repository, in the git history or in the logs (see “publishing from ci” below).

| service | ecosystem | upload |
|---------|-----------|--------|
| Fedora COPR | Fedora, RHEL/EPEL | `copr-cli build atlifm <src.rpm>` (SRPM from CI) |
| openSUSE OBS | openSUSE, Fedora, Debian, Ubuntu | add `atlifm.spec` + sources to `home:1nfys`, `osc commit` |
| Ubuntu PPA | Ubuntu | `dput ppa:1nfys/atlifm <source.changes>` (GPG-signed) |
| Debian mentors | Debian (for sponsorship) | `dput mentors <source.changes>` |
| GitHub Releases | AppImage and every CI artifact | push a `v*` tag |

users then install with `dnf copr enable 1nfys/atlifm`, `add-apt-repository ppa:1nfys/atlifm` or the OBS repository URL

the `PKGBUILD` (with `.SRCINFO`) is AUR-ready; publishing there needs an AUR account and an SSH key, and is optional, since arch users can also build the `PKGBUILD` straight from this repository or from a self-hosted pacman repo

## publishing from ci (github secrets)

`publish-packages.yml` runs automatically after a `v*` tag build publishes its GitHub Release, so pushing a tag is enough to get the packages into every enabled service. it can also be started by hand: **Actions → “Publish packages” → “Run workflow”**, pick the release (any `v*` tag that already has a GitHub Release, empty = the latest release) and the targets (`all`, `copr`, `obs` or `ppa`). each target job downloads the source packages from that release (no repository checkout, no local build) and hands them to the matching service: COPR gets the `*.src.rpm` and compiles every enabled chroot, OBS gets the `*.src.rpm` sources plus the Debian `*.dsc` set and builds both rpm and deb repositories, the PPA gets the signed source package.

create these repository secrets once, in **Settings → Secrets and variables → Actions → Secrets**: GitHub stores them encrypted, masks them in the logs and hands them to this workflow alone. the workflow writes each value straight into the config file of the tool, so the values are never printed:

| secret | used by | value |
|--------|---------|-------|
| `COPR_CONFIG` | `copr` | the block that https://copr.fedorainfracloud.org/api/ shows you, the same content as `~/.config/copr` |
| `OBS_OSCRC` | `obs` | your OBS login in `~/.config/osc/oscrc` format (OBS has no general api tokens for source uploads, so this is the user and password) |
| `GPG_PRIVATE_KEY` | `ppa` | the secret key registered in your Launchpad profile, exported with `gpg --armor --export-secret-keys <key id>` |
| `LAUNCHPAD_SSH_KEY` | `ppa` | the private half of an `ed25519` key registered in your Launchpad profile, used by the sftp upload |

the two login files look like this (both secrets are just the file, pasted once):

```ini
[copr-cli]
username = 1nfys
login = <api login>
token = <api token>
copr_url = https://copr.fedorainfracloud.org
```

```ini
[general]
apiurl = https://api.opensuse.org

[https://api.opensuse.org]
user = 1nfys
pass = <password>
```

optional repository variables (**Settings → Secrets and variables → Actions → Variables**) override the defaults:

| variable | default |
|----------|---------|
| `COPR_PROJECT` | `atlifm` |
| `OBS_PROJECT` | `home:1nfys` |
| `OBS_PACKAGE` | `atlifm` |
| `LP_LOGIN` | `1nfys` |
| `LP_PPA` | `atlifm` |
| `LP_SERIES` | `resolute` |

one-time setup per service:

* **COPR**: log in, create the project (`atlifm` by default) and enable the chroots you want (`fedora-*`, `epel-*`); the job submits the `*.src.rpm` from the release and follows the build to the end;
* **OBS**: log in once (this creates `home:1nfys`), add the target distributions to the project (openSUSE, Fedora, Debian, Ubuntu) and put your login into `OBS_OSCRC`. the job creates the package itself, extracts the sources from the release `*.src.rpm`, takes the spec from `packaging/rpm/atlifm.spec`, adds the Debian `*.dsc` set and commits, so one package feeds both, and repeating a run on a newer release replaces the old tarballs;
* **PPA**: create the PPA (`atlifm` by default), add your GPG key and an `ed25519` ssh key to the Launchpad profile, and use a dedicated ci signing key without a passphrase, since `debsign` cannot ask for one. the upload targets `resolute` by default (it has Qt >= 6.5); set the `LP_SERIES` variable or pass `ppa_series` to use another enabled series. the job uploads into `~1nfys/ubuntu/atlifm/<series>/`, which overrides the changelog suite.

## cloud builds (github actions)

`.github/workflows/build-packages.yml` builds every format on
GitHub-hosted runners, x86_64 **and** aarch64 for each job below except
`arch` (x86_64-only, since there is no arm64 container image), so no local
toolchain or powerful PC is needed:

| job | produces | where it runs |
|-----|----------|---------------|
| `deb` | `.deb` + Debian source package | `debian:13` container, canonical `dpkg-buildpackage` |
| `rpm` | `.rpm` + `.src.rpm` | fedora container, `rpmbuild` with canonical `%cmake` |
| `arch` | `.pkg.tar.zst` | archlinux container, `makepkg` |
| `alpine` | `.apk` | alpine container via docker, `abuild` |
| `appimage` | `.AppImage` | `ubuntu:25.10` container |

how to use it:

1. push this tree to a GitHub repository (public or private);
2. **Actions → “Build packages” → “Run workflow”**: all jobs run in
   parallel and every artifact is attached to the workflow run;
3. or push a tag such as `v1.0.1`, and the same run additionally creates a
   GitHub Release with every artifact attached.

notes:

* every job builds from the checked-out source; jobs that need an archive
  generate it in-job with `scripts/make-src-tarball.sh` (nothing is stored
  in the repository);
* four of the five build jobs run as a matrix over `ubuntu-latest`
  (x86_64) and `ubuntu-24.04-arm` (aarch64); `arch` is x86_64-only; arm64
  runners are free for public repositories;
* artifacts are attached to each workflow run (and to a GitHub Release on
  version tags); nothing pre-built is stored in the repository;
* uploading to the user repositories is handled by
  `.github/workflows/publish-packages.yml`, which starts automatically
  after the release and reads every credential from repository secrets
  (see “publishing from ci” above).

## build requirements (all distributions)

* cmake >= 3.28, C++23 compiler (GCC 13+ / Clang 17+), ninja (or make)
* Qt >= 6.5: Core, Gui, Widgets, Concurrent, Svg, SvgWidgets, DBus
* runtime deps are computed automatically by each packaging system
  (the binary links Qt6 Widgets/Gui/DBus/Core/Svg + libstdc++).

**Ubuntu 24.04 note:** the distro Qt is 6.4.2, which is older than the
required 6.5. use the official Qt online installer / `aqtinstall`, a PPA,
or Ubuntu 25.10+/Debian 13+ which ship Qt >= 6.8.

## verification status

* Debian 13 (aarch64): full builds from the fixed source tree with
  `cmake + ninja`, repeated; the artifacts (`.deb`, `.rpm` + `.src.rpm`,
  Arch `.pkg.tar.zst`, AppImage, Debian source package) were produced and
  smoke-tested (payloads extracted, binary run).
* container checks on the local device (Ubuntu 25.10, Alpine 3.22) were
  started but stopped early to keep the phone usable under memory
  pressure; the same builds now run on GitHub runners via
  `.github/workflows/build-packages.yml` (see “cloud builds” above).
