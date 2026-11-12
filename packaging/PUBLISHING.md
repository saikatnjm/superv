# Publishing SuperV

How to put SuperV into the package systems of the big Linux families, so users can install
**and update** it with their normal tools. Do them in this order; each takes 15–45 minutes the first time.

| | Users install with | Updates arrive via |
|---|---|---|
| 1. AUR | `yay -S superv` | `yay -Syu` |
| 2. Fedora COPR | `sudo dnf copr enable saikatnjm/superv` → `sudo dnf install superv` | `sudo dnf upgrade`, GNOME Software |
| 3. Launchpad PPA | `sudo add-apt-repository ppa:saikatnjm/superv` → `sudo apt install superv` | `sudo apt upgrade`, Software Updater, Mint Update Manager |

> Replace `saikatnjm` with the usernames you actually get on each site.
> Never commit private keys. Everything below keeps them in `~/.ssh` and `~/.gnupg`.

---

## 1. AUR (Arch, Manjaro, EndeavourOS)

The files are in `packaging/arch/` (`PKGBUILD`, `.SRCINFO`, `superv.install`). The name `superv` is free.
Before the first push, point them at the latest release: `packaging/arch/bump.sh 1.2.3`.

1. **Create an account** at <https://aur.archlinux.org/register>.
2. **Make an SSH key for the AUR:**
   ```bash
   ssh-keygen -t ed25519 -f ~/.ssh/aur -C "aur"
   cat ~/.ssh/aur.pub
   ```
   Paste the printed line into **My Account → SSH Public Key** on the AUR site, then save.
3. **Tell SSH to use it.** Add this to `~/.ssh/config`:
   ```
   Host aur.archlinux.org
     IdentityFile ~/.ssh/aur
     User aur
   ```
4. **Create the package by pushing it** (the "empty repository" warning is normal):
   ```bash
   git clone ssh://aur@aur.archlinux.org/superv.git ~/aur-superv
   cp packaging/arch/PKGBUILD packaging/arch/.SRCINFO packaging/arch/superv.install ~/aur-superv/
   cd ~/aur-superv
   git add PKGBUILD .SRCINFO superv.install
   git commit -m "superv 1.2.3"
   git push origin master
   ```
5. Check <https://aur.archlinux.org/packages/superv>.

**Each new release:** after the GitHub release `vX.Y.Z` exists,
```bash
packaging/arch/bump.sh X.Y.Z
cp packaging/arch/PKGBUILD packaging/arch/.SRCINFO ~/aur-superv/
cd ~/aur-superv && git commit -am "superv X.Y.Z" && git push
```

---

## 2. Fedora COPR (Fedora, also openSUSE Tumbleweed)

COPR builds straight from this GitHub repo using `packaging/rpm/superv.spec`. Nothing to upload.

1. **Create a Fedora account** at <https://accounts.fedoraproject.org/>.
2. **Log in** at <https://copr.fedorainfracloud.org/> with it.
3. **New Project:**
   - Name: `superv`
   - Chroots: tick the current `fedora-NN-x86_64` releases (optionally `opensuse-tumbleweed-x86_64`)
   - Description: *Win+V clipboard history for Linux*
   - Instructions: `sudo dnf copr enable <you>/superv && sudo dnf install superv`
4. **Packages → New package → SCM:**
   - Package name: `superv`
   - Clone URL: `https://github.com/saikatnjm/superv.git`
   - Committish: `v1.2.3` (the release tag)
   - Subdirectory: *(empty)*
   - Spec file: `packaging/rpm/superv.spec`
   - Type: `git`, SRPM build method: `rpkg`
5. **Rebuild** the package and wait for the builds to go green (5–10 minutes).
6. Test on Fedora: `sudo dnf copr enable <you>/superv && sudo dnf install superv`.

**Each new release:** edit the package, change **Committish** to the new tag (`vX.Y.Z`), then **Rebuild**.

---

## 3. Launchpad PPA (Ubuntu and Linux Mint)

The Debian source packaging is in `packaging/ppa/debian/`, and `packaging/ppa/upload.sh` builds,
signs and uploads it. Default targets: `jammy` (Ubuntu 22.04, Mint 21), `noble` (Ubuntu 24.04, Mint 22), `resolute` (26.04) and `stonking` (26.10).

1. **Create a Launchpad account** at <https://launchpad.net/> (pick your username, e.g. `saikatnjm`).
2. **Install the tools:**
   ```bash
   sudo apt install devscripts dput debhelper
   ```
3. **Make a GPG signing key** (use your real name and the email you'll use for Launchpad):
   ```bash
   gpg --full-generate-key          # choose RSA and RSA, 4096 bits, does not expire (or 2y)
   gpg --list-secret-keys --keyid-format=long   # copy the long fingerprint
   gpg --keyserver keyserver.ubuntu.com --send-keys <FINGERPRINT>
   ```
   Back it up somewhere safe (not in the repo): `gpg --export-secret-keys --armor <FINGERPRINT> > superv-signing-key.asc`
4. **Register the key on Launchpad:** your profile → **OpenPGP keys** → paste the fingerprint → **Import Key**.
   Launchpad emails you an encrypted message: save the text to a file, run `gpg --decrypt thatfile.txt`
   and open the link it contains. (Wait a few minutes after step 3 so the keyserver has your key.)
   If Launchpad asks you to sign the Ubuntu Code of Conduct, do that on the same page.
5. **Create the PPA:** your profile → **Create a new PPA** → URL: `superv`, Display name: `SuperV`.
6. **Upload** (from the repo folder):
   ```bash
   export DEBFULLNAME="Your Name" DEBEMAIL="you@example.com"   # exactly as on your GPG key
   packaging/ppa/upload.sh <launchpad-username>
   ```
   Enter your GPG passphrase when asked. Launchpad emails *Accepted*, then builds for 10–30 minutes.
7. **Watch** `https://launchpad.net/~<you>/+archive/ubuntu/superv/+packages`, then test on Ubuntu or Mint:
   ```bash
   sudo add-apt-repository ppa:<you>/superv && sudo apt install superv
   ```

**Each new release:** `packaging/ppa/upload.sh <launchpad-username>` (same version again for a fix? `PPA_REV=2 packaging/ppa/upload.sh …`).
**Newer Ubuntu releases:** add their series names: `packaging/ppa/upload.sh <you> jammy noble <newer-series>`.

---

## Release checklist

1. Bump the version in `superv.h` (`#define SV_VERSION`), `packaging/rpm/superv.spec` (+ `%changelog`),
   `data/io.github.superv.SuperV.metainfo.xml` (`<releases>`) and `docs/index.html`.
   Add the version to `CHANGELOG.md`. `docs/versions.html` shows only the stable version of each major line:
   update the current line's row and download links (file names as the release workflow attaches them);
   a new major version gets its own row and a column in the comparison table.
2. Commit, then `git tag vX.Y.Z && git push origin main vX.Y.Z`: GitHub builds the .deb/.rpm/Arch packages.
3. AUR: `packaging/arch/bump.sh X.Y.Z`, copy, push.
4. COPR: set Committish to `vX.Y.Z`, Rebuild.
5. PPA: `packaging/ppa/upload.sh <you>`.
