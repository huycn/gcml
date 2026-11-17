# Git credential manager for Linux

This is a less-bloated Linux-only C++/Qt rewrite of [Git Credential Manager](https://github.com/git-ecosystem/git-credential-manager).

It is a port of Git Credential Manager 3.0.2 and a drop-in replacement for it on Linux: the same executable name,
commands, settings, environment variables and credential store formats, so credentials stored by the .NET version
stay readable. It needs no .NET runtime: a stripped release build is about 3 MB (2 MB with GitHub only) plus 1.7 MB for the dialog
helper, using the system's Qt.

## Features

- All commands: `get`, `store`, `erase`, `capability`, `configure`, `unconfigure`, `diagnose`, `github
  login|logout|list`, `azure-repos list|bind|unbind|clear-cache`.
- Host providers: GitHub (incl. Enterprise), GitLab, Bitbucket (Cloud and Data Center), Azure Repos (OAuth with
  Microsoft Entra ID, PATs, managed identity, service principals, workload identity federation), and a generic
  provider (basic authentication and generic OAuth) for every other host.
- Credential stores: `secretservice` (GNOME Keyring, KWallet, KeePassXC, ...), `gpg` (GNU `pass` layout), `cache`
  (Git's credential cache), `plaintext`, `none`.
- Graphical sign-in dialogs (Qt Widgets, light and dark) with a terminal fallback; tracing with `GCM_TRACE` and Git's
  TRACE2.

## Build

Requirements: a C++20 compiler, CMake >= 3.21, Ninja (optional), Qt 6 >= 6.5 (Core, Network, Widgets, Svg; Test for
the tests), libsecret, OpenSSL (Azure Repos only), and `git` at run time.

On Arch Linux: `pacman -S cmake ninja qt6-base qt6-svg libsecret openssl`.

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
ctest --test-dir build-release          # optional
```

Build options:

| Option | Default | Meaning |
| --- | --- | --- |
| `GCM_PROVIDERS` | `github;gitlab;bitbucket;azrepos` | host providers to build; the generic provider is always included |
| `GCM_BUILD_UI` | `ON` | build the graphical helper `git-credential-manager-ui` |
| `GCM_BUILD_TESTS` | `ON` | build the unit tests |

For example, GitHub only, without tests:

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DGCM_PROVIDERS=github -DGCM_BUILD_TESTS=OFF
```

## Install

```sh
cmake --install build-release --prefix ~/.local --strip      # or /usr/local with sudo
```

This installs `git-credential-manager` and `git-credential-manager-ui`; keep them in the same directory (the dialogs
run in the second one). Then make it Git's credential helper and choose a store:

```sh
git-credential-manager configure                          # sets credential.helper to its full path (global config)
git config --global credential.credentialStore secretservice
```

With `credential.helper = manager`, Git runs the first `git-credential-manager` it finds in `$(git --exec-path)` and
then on `PATH`, so an install in `~/.local/bin` or `/usr/local/bin` takes precedence over a distribution package in
`/usr/bin`. To go back, remove the two files or run `git-credential-manager unconfigure`.

Settings and environment variables are those of upstream; see its
[configuration](https://github.com/git-ecosystem/git-credential-manager/blob/main/docs/configuration.md),
[environment](https://github.com/git-ecosystem/git-credential-manager/blob/main/docs/environment.md) and
[credential store](https://github.com/git-ecosystem/git-credential-manager/blob/main/docs/credstores.md)
documentation.

## Differences from the .NET version

- The dialogs run in a separate executable, `git-credential-manager-ui`, so Git operations never open a display
  connection unless a dialog is shown. Without it (or without a desktop session) prompts use the terminal.
- Azure Repos: Microsoft Entra sign-in uses its own token cache instead of MSAL's, so you sign in once more after
  switching; the Microsoft identity broker is not supported.
- Ctrl+C at a terminal prompt cancels cleanly (the .NET version on Linux kills the process).
- Error messages show no .NET stack traces.

The full list, with the reasons, is in [docs/DESIGN.md](docs/DESIGN.md).

## Status

All upstream features that apply to Linux are ported, together with about 97 % of the upstream unit tests; the
behaviour is checked against the .NET build by end-to-end comparison scripts. Sign-in against the real GitHub, GitLab,
Bitbucket and Azure DevOps services has not been verified yet.

## License

MIT, like the original; see [LICENSE](LICENSE).
