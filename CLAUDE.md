# gcml

C++20 / Qt 6 port of Git Credential Manager 3.0.2 for Linux. Design, deliberate differences from upstream and the
verification setup are in `docs/DESIGN.md`; read the section of the component you touch before changing it.

## Upstream

- Reference source: `~/devel/custom/git-credential-manager` (3.0.2, commit `f0ee0c77`). The first comment of every
  source and test file names the upstream files it ports; compare with them when fixing a bug.
- Reference build for comparisons: see `docs/DESIGN.md` section 1 (`dotnet publish ... -o ~/opt/gcm-upstream`).

## Build and test

```sh
cmake -S . -B build -G Ninja                       # RelWithDebInfo by default
cmake --build build
ctest --test-dir build                             # 10 test executables
build/tests/test_core_app --class DiagnoseCommandTests [testFunction]   # one class / function
```

- Options: `-DGCM_PROVIDERS="github;gitlab;bitbucket;azrepos"` (subset allowed), `-DGCM_BUILD_UI=OFF`,
  `-DGCM_BUILD_TESTS=OFF`.
- End to end (after a build): `tests/e2e/compare-with-upstream.sh build/src/app/git-credential-manager
  ~/opt/gcm-upstream/git-credential-manager` (all scenarios must print PASS),
  `tests/e2e/ui-helper.sh build/src/app/git-credential-manager build/tests/git-credential-manager-ui-scripted`,
  `tests/e2e/pty-prompt.py build/src/app/git-credential-manager typed`.
- The build must stay warning-free with GCC and clang (`-Wall -Wextra -Wpedantic`).
- If a new test file's `.moc` include is reported missing, reconfigure and delete
  `build/tests/<test>_autogen/timestamp` (automoc keeps a stale timestamp).

## Rules

- Fidelity: same behaviour, protocol output, messages and trace strings as upstream; copy message texts verbatim; keep
  on-disk store formats identical. A deliberate difference must be documented in `docs/DESIGN.md`.
- Behaviour that differs from upstream on purpose is listed in `docs/DESIGN.md`; do not "fix" it back.
- Synchronous code: upstream's `Task<T> FooAsync()` is `T foo()`; no threads.
- Message statements (`QStringLiteral(...).arg(...)`) stay on one line when below 250 characters.
- Test files: one Qt Test class per upstream test class, functions named after the upstream method (class prefix and
  `Async` dropped, lower camel case), registered with `GCM_REGISTER_TEST`; every test file carries
  `// clazy:excludeall=non-pod-global-static`. Tests never touch the network, a real keyring or the user's Git config
  (use the fakes in `tests/infra` and `IsolatedGitConfig`).
- Bug fixes come with a test that fails without the fix; when the output is observable, check it against the .NET
  build with the e2e scripts.
