# Design notes

Port of Git Credential Manager 3.0.2 (.NET) to C++20 / Qt 6, Linux only. This file records how the port is built,
where it deliberately differs from upstream, and how it is verified. Read it before fixing a bug: something that looks
wrong may be a documented difference, and most fixes are checked by comparing with the .NET build.

## 1. Upstream reference

- Upstream checkout: `~/devel/custom/git-credential-manager`, version 3.0.2, commit `f0ee0c77`. Source files of the
  port name the upstream files they port in their first comment line.
- Reference build for the comparison scripts (needs the .NET 10 SDK):

  ```sh
  cd ~/devel/custom/git-credential-manager
  dotnet publish src/git-credential-manager/git-credential-manager.csproj -c Release -o ~/opt/gcm-upstream
  ~/opt/gcm-upstream/git-credential-manager --version    # 3.0.2+f0ee0c77...
  ```

### 1.1 Fidelity rule

Same behaviour, protocol output, user-visible messages and trace strings as upstream; message texts are copied
verbatim. On-disk formats (plaintext store, `pass` layout, Secret Service schema and attributes) are identical, so
credentials stored by the .NET build stay readable and the other way round. Every exception to this rule is listed in
this file.

### 1.2 Scope

In scope: the `git-credential-manager` executable with all commands (`get`, `store`, `erase`, `capability`,
`configure`, `unconfigure`, `diagnose`, `github login|logout|list`, `azure-repos list|bind|unbind|clear-cache`); the
host providers Azure Repos, Bitbucket (Cloud and Data Center), GitHub, GitLab and Generic (basic and generic OAuth);
the stores `secretservice`, `gpg`, `cache`, `plaintext`, `none`; all settings, environment variables and Git config
keys with upstream's precedence (env var, URL-scoped config, unscoped config, `/etc/git-credential-manager/config.d`);
proxy, TLS options, CA bundle, cookie file, provider auto-detection; `GCM_TRACE` and TRACE2; graphical and terminal
prompts.

Out of scope (Windows/macOS only upstream): DPAPI, Windows Credential Manager, macOS Keychain, the WAM broker,
registry settings, Schannel, legacy UI helpers and `dotnet exec` helper lookup, Windows Integrated Authentication as an
authentication method (NTLM/Negotiate challenges are still detected, as upstream on Linux), the main-thread
`Dispatcher`. No distribution packages yet.

## 2. Architecture

### 2.1 Toolchain

C++20, Qt 6 (>= 6.5, developed with 6.12), CMake + Ninja, libsecret-1, OpenSSL `libcrypto` (Azure Repos only),
namespace `gcm`. Libraries use QtCore + QtNetwork only; QtWidgets and QtSvg are linked only into the UI helper.

### 2.2 Synchronous code, no dispatcher

Upstream is async/await throughout, with a dispatcher that keeps the main thread free for the UI toolkit. The port is
synchronous: `Task<T> FooAsync()` becomes `T foo()`. Waiting on the network, a child process or a timer is a nested
`QEventLoop` or the blocking `QProcess` API. No threads. Cancellation uses `CancellationToken` (`core/common.h`).

### 2.3 Two executables

- `git-credential-manager`: `QCoreApplication`, no GUI code. Starts fast and cannot fail on a broken display.
- `git-credential-manager-ui`: `QApplication` and all dialogs, run as a child process through
  `AuthenticationBase::invokeHelper` with upstream's helper protocol: options on the command line, optional payload
  on stdin, a `key=value` dictionary ended by a blank line on stdout.
  - Generic prompts at top level: `basic`, `oauth`, `device`, `default-account` (options and result keys as upstream
    `src/Core/UI/Commands/*.cs`).
  - Provider prompts under the provider's name: `github prompt|2fa|device|select-account`, `gitlab prompt`,
    `bitbucket prompt` (as upstream `src/<Provider>/UI/Commands/*.cs`).
  - Failure: non-zero exit and `error=<message>`. Addition to upstream: when the user dismisses a dialog the helper
    also writes `cancelled=1`, which the caller turns into `OperationCanceledException`, so `get` answers `quit=1`.
  - Lookup: `GCM_UI_HELPER` / `credential.uiHelper` (empty disables it), else `git-credential-manager-ui` next to the
    executable, then on `PATH`. Without GUI prompts, a desktop session or the helper, prompts use the terminal.

Reason for the split: a Qt GUI application object cannot be created lazily after a `QCoreApplication`, and creating
one on every `get` would cost a display connection per Git operation and abort the process on an unusable display.
Consequence: the two binaries must be installed side by side.

### 2.4 Type mapping

| .NET | Port |
| --- | --- |
| `string` that may be `null` where the difference is observable | `gcm::OptString` (`std::optional<QString>`) |
| `Uri` | `QUrl`, rendered with `gcm::absoluteUri()` to match `System.Uri` normalisation |
| `IDictionary<string,string>` (+ `StringComparer`) | `gcm::StringDictionary`, insertion ordered, case sensitivity chosen at construction |
| Exceptions | `gcm::Exception` hierarchy (`core/common.h`) with the same kinds and messages; `typeName()` gives the short .NET name |
| `HttpClient` / `HttpMessageHandler` / `HttpResponseMessage` | `HttpClient` / `IHttpMessageHandler` / `HttpResponse` over `QNetworkAccessManager` |
| `System.CommandLine` | own sub-command parser, `core/cmdline.h` |
| Spectre.Console prompts | `IConsoleService::promptText / promptSecret / promptSelection` on `/dev/tty` with termios |
| `System.Text.Json` | `QJsonDocument` |
| MSAL (Microsoft.Identity.Client) | own Entra ID client, `src/azrepos/entra.h` |
| xUnit + Moq | Qt Test + hand-written fakes in `tests/infra` |

## 3. Layout

```
src/core/*.h            shared interface headers
src/core/auth/*.h       authentication interfaces (AuthenticationBase, basic/OAuth/WIA, OAuth2)
src/core/base/          strings, streams, trace, file system, environment, processes, session, INI
src/core/gitcfg/        git process, git configuration, settings, configuration service
src/core/app/           request/response, provider registry, command line, commands, diagnostics, application,
                        command context, generic host provider
src/core/net/           HTTP client + factory, cookies, OAuth2 client/browser, basic/OAuth/WIA authentication
src/core/stores/        credential stores, console and TTY prompts, TRACE2
src/github/ src/gitlab/ src/bitbucket/ src/azrepos/     host providers (azrepos also holds the Entra client)
src/app/main.cpp        git-credential-manager
src/ui/                 git-credential-manager-ui (library gcmui + main.cpp)
tests/infra/            fakes (port of src/TestInfrastructure) and the multi-class test runner
tests/core/<part>/  tests/<provider>/  tests/ui/        one test executable per directory
tests/uiscripted/       git-credential-manager-ui-scripted (real dialogs, scripted user; for tests/e2e)
tests/e2e/              end-to-end scripts
```

- Each `src/core/<part>` is an OBJECT library (`gcmcore_<part>`); they merge into the static library `gcmcore`.
  Sources are globbed with `CONFIGURE_DEPENDS`; a module (provider, `src/app`, `src/ui`) is only added once it has
  sources.
- `GCM_PROVIDERS` (default `github;gitlab;bitbucket;azrepos`) selects the providers to build. `src/app/main.cpp`
  links and registers a provider when its library target exists (`GCM_HAVE_<PROVIDER>`), Azure Repos, Bitbucket,
  GitHub, GitLab at normal priority in upstream's order, Generic at low priority. Tests of a provider that is not
  built are skipped. Only Azure Repos links OpenSSL directly.
- Informational version: `<VERSION>+<git commit>` when built from a git checkout, else `<VERSION>`
  (`gcmInformationalVersion()`, used by `--version`, TRACE2 `version` and `diagnose`).

## 4. Components

Each entry: where the code is, interface changes relative to upstream, and deliberate behaviour differences.

### 4.1 base

- `EnsureArgument`, `CancellationToken` (`common.h`); string helpers, URI helpers, `quoteCmdArg`,
  `splitCommandLine`, base64url and `wrapText` (Spectre.Console-style word wrapping) in `strings.h`; dictionary wire
  format in `streams.h`; `ChildProcess` / `ProcessManager` over `QProcess` with TRACE2 child events in `platform.h`.
- `ChildProcess::id()` keeps the pid after the process exits (QProcess forgets it), so TRACE2 `child_exit` reports
  it. `ChildProcess::startInfo()` has a mutable overload (GitProcess redirects stderr before starting).
- Text writers always write LF line endings.

### 4.2 gitcfg

- `GitVersion::originalString()` returns `toString()` for versions built from components; `KeyNotFoundException`
  for `IGitConfiguration::get`; `Settings::tryGetExternalDefault` is public (`protected internal` upstream);
  `GitProcess::getConfiguration()` returns a new configuration object per call, kept alive by the `GitProcess`.
- `LinuxSettings` sorts the files of `config.d` before concatenating them (upstream's comment says alphabetical
  order but relies on enumeration order). The defaults parser accepts only keys of `[a-zA-Z0-9.-]`, as upstream, so
  URL-scoped defaults cannot be expressed there.

### 4.3 net

- `QtHttpMessageHandler` (the real handler) with `HttpHandlerOptions` and `HttpProxySettings` (`core/http.h`);
  `HttpClientFactory::tryCreateProxy` fills an `HttpProxySettings`; `HttpClient::send(method, url, headers, ...)`
  takes a `MultiDictionary`; `OAuth2Json` parse functions in `core/auth/oauth2.h`.
- A device-code response without `interval` polls every 5 s (RFC 8628); upstream polls without delay then.
- A missing default UI helper silently falls back to the terminal; a configured one that is missing warns, as
  upstream. Without the helper, prompts that upstream shows in-process (basic, OAuth mode, device code, NTLM) use the
  terminal.
- The loopback listener answers paths outside the redirect path with 404 (HttpListener prefix behaviour) and serves
  HTML with `Content-Type: text/html; charset=utf-8`.
- As upstream, WIA is never probed on Linux. An unparsable `credential.oauthAuthModes` leaves no mode at all
  (upstream's `Enum.TryParse` default).

### 4.4 app (commands, providers registry, diagnostics)

- `core/commands.h` (all commands and `ProviderCommand`), `core/diagnostics.h` (standard diagnostics),
  `core/generic.h` (`GenericHostProvider`, `GenericOAuthConfig`). Diagnostic enums use upstream's names (`Success`,
  `Warning`, `Error`, `Skipped`). `HostProviderRegistry` keeps priorities in first-registration order, like
  upstream's dictionary.
- Errors print no stack trace (C++ has none). `GCM_DEBUG` waits for a debugger by polling `TracerPid`. Exit codes
  -1 (error) and 130 (interrupted) as upstream.
- `diagnose` prints plain text with ANSI colours when stdout is a terminal (no Spectre markup, no spinner), wrapped
  like Spectre (terminal width, 80 columns when redirected). The log file starts with a UTF-8 BOM like upstream's
  `StreamWriter`. Exceptions are rendered like .NET's `Exception.ToString()` without the stack trace
  (`exceptionToString`: namespace-qualified type name such as `System.IO.IOException`, inner exceptions after
  ` ---> `).
- The Entra diagnostic is registered by the Azure Repos provider (`IDiagnosticProvider`), not by
  `addStandardDiagnostics`.

### 4.5 command line (`core/cmdline.h`)

Own parser imitating System.CommandLine's help and errors (checked against the .NET build):
- an `Argument` added to several commands is `multiParented` (optional in the usage line); sub-command rows print
  arguments as `<name>` whatever their arity;
- `Command::addValidator` reports its message like a parse error;
- help sorts aliases by prefix then name, and the help of a command with arguments ends with one more blank line.

### 4.6 stores, console, TRACE2

- `core/stores.h` (plaintext, `pass`/`gpg`, credential-cache, none, Secret Service), `core/tty.h` (key parser,
  `Select::waitReadable`, raw-mode terminal input, `/dev/tty` output, prompt rendering), `core/trace2message.h`
  (TRACE2 messages and their three formats). `Trace2::writeChildStart/Exit` take a child id from
  `Trace2::nextChildId()`, as upstream. `ConsoleService` has a test-seam constructor. `IGpg` in `core/credential.h`.
- Prompts are drawn by our own code instead of Spectre.Console: `<label>: ` text prompts, secrets without echo (or
  with a mask), a selection menu with arrow/number keys, Escape cancels.
- **Deliberate difference: Ctrl+C at a terminal prompt.** Upstream 3.0.2 builds its Linux raw mode from the macOS
  `LocalFlags` values (`Interop/Posix/Native/Termios.cs`), so on Linux only `ECHO` is cleared: input stays
  line-buffered and Ctrl+C raises SIGINT, which kills the process after its handler restores the terminal (and
  occasionally hangs). The port uses real raw mode: keys echoed as typed, Ctrl+C read as 0x03, terminal restored,
  `InterruptedException` thrown; `get` answers `quit=1` like upstream's `GetCommand`, elsewhere the exit code is 130.
- TRACE2 additionally supports a directory target (one file per process, as Git) and `af_unix:` sockets through
  plain POSIX sockets (stream, falling back to datagram). The JSON event format mimics System.Text.Json's escaping and
  number formatting.
- **Known TRACE2 difference:** Git's own `start` line for the `git config -z --get-regexp trace2\..*` child shows the
  full path of `git` (QProcess passes the resolved path as argv[0]; .NET passes `git`).

### 4.7 ui (`src/ui`)

- Library `gcmui` (dialogs, `HelperApplication`) plus `main.cpp`. Qt Widgets with the system style; when the
  platform reports a dark colour scheme that the style's palette does not follow, a dark Fusion palette is installed
  (`GCM_UI_THEME=light|dark` forces one). Images are SVGs converted from the Avalonia geometries; monochrome icons
  take the theme's icon colour.
- Dismissing a dialog (close button or Escape) writes `error=User cancelled dialog.` and `cancelled=1`, exit 255.
  `--help`/`--version` work without a display. The parent window id is only traced (`ui/parent`), as upstream
  outside Windows.
- Deliberate fix: `SixDigitInput` paste uses the first six digits and leaves missing boxes empty (upstream pads with
  spaces, so a short paste enabled Verify, and throws on more than six digits).
- Provider checks the commands need (`IsGitHubDotCom`, `IsGitLabDotCom`, Bitbucket help URLs) are local to `src/ui`,
  so the helper does not link the provider libraries.
- Not ported: the About window (macOS native menu only) and the debug controls of `DialogWindow`.

### 4.8 github (`src/github`)

- `github.h` holds the whole package (namespace `gcm::github`); `githubresources.h` (browser result pages) is
  generated verbatim from upstream's resx.
- Graphical prompts: a GitHub.UI-protocol helper when `GCM_GITHUB_HELPER` / `credential.gitHubHelper` (default
  `GitHub.UI`) finds one, else `git-credential-manager-ui github ...` (upstream: in-process views), else the terminal.
- The device-code dialog logic is shared with the generic OAuth flow
  (`AuthenticationBase::getTokenByDeviceCodeWithDialog`); closing it throws `Exception` rather than upstream GitHub's
  `InvalidOperationException` (same message; only the TRACE2 type name differs).
- `AuthenticationModes` print like .NET flags (`Pat, OAuth`) and parse like `Enum.TryParse` (names or numbers,
  comma-separated); the helpers `formatFlagsEnum` / `tryParseFlagsEnum` live in `core/strings.h`. The PAT note uses the
  C locale date (`dd-MMM-yyyy HH:mm`) and the host name. A `--url` that is not an absolute URI is rejected by the
  handler (upstream: a parse error of `Option<Uri>`).

### 4.9 gitlab (`src/gitlab`)

- `gitlab.{h,cpp}` (namespace `gcm::gitlab`). Prompts: `GCM_GITLAB_HELPER` / `credential.gitLabHelper` (default
  `GitLab.UI`), else `git-credential-manager-ui gitlab prompt`, else the terminal.
- A C++ `Credential` cannot carry upstream's `OAuthCredential` type: `generate()` returns the credential with an
  OAuth flag and the refresh token. A missing refresh token is stored as an empty secret (upstream stores null).

### 4.10 bitbucket (`src/bitbucket`)

- `bitbucket.{h,cpp}` and `bitbucketapi.cpp` (namespaces `gcm::bitbucket`, `::cloud`, `::datacenter`);
  `bitbucketresources.h` generated from upstream's resx. `IRegistry<T>` returns `shared_ptr`s;
  `RestApiResult<IUserInfo>` is `UserInfoResult` (status code + user name).
- The OAuth client configuration is read by one function per flavour so a missing setting reports the same error as
  upstream (C++ leaves the evaluation order of constructor arguments unspecified).
- Prompts: `GCM_BITBUCKET_HELPER` / `credential.bitbucketHelper` (default `Atlassian.Bitbucket.UI`), else
  `git-credential-manager-ui bitbucket prompt`, else the terminal.
- `httpStatusCodeName` follows the .NET enum names for duplicate values (`Moved`, `Redirect`, `Ambiguous`, ...),
  visible only in "Failed to resolve username. HTTP: ..." messages.

### 4.11 azrepos and the Entra ID client (`src/azrepos`)

- `azrepos.h` (namespace `gcm::azrepos`: constants, `UriHelpers`, REST API, authority cache, binding manager,
  provider, `EntraAuthenticationDiagnostic`) and `entra.h` (namespace `gcm::entra`: the Entra ID client replacing
  MSAL). MSAL's exception types are kept as `Entra*Exception` with MSAL's type names (TRACE2 and the legacy client hint
  depend on them).
- The Entra client: authorization code + PKCE through the system browser, device code, silent renewal, account
  enumeration and removal, MSA passthrough; client secret and RS256 certificate assertions (OpenSSL), PKCS#12 store
  lookup; managed identity (IMDS, App Service, Cloud Shell, Azure Arc); workload identity federation including the
  GitHub Actions OIDC token. Tested against a fake identity platform; real Microsoft endpoints are a manual check.
- Token cache: our own, in the Secret Service with a plaintext file fallback (as upstream allows). MSAL's cache is not
  read, so users sign in once more after switching; the shared Microsoft developer cache setting is only traced.
- The broker (WAM / Linux identity broker) needs MSAL's native runtime: reported as `unavailable`, and the
  broker-only default-account prompt does not exist. No embedded web view (as upstream on Linux).
  `getInteractionMode` (no caller upstream) asks a `select-interaction-mode` helper command that no UI helper
  implements, as upstream.
- The certificate store is .NET's on Linux (`~/.dotnet/corefx/cryptography/x509stores/my/*.pfx`); a thumbprint not
  found there throws .NET's "Unix LocalMachine X509Store is limited to ..." exception, as upstream on Linux.
- The Entra diagnostic reports our cache storages instead of MSAL's cache properties. It checks the Secret Service
  cache and, when that is unavailable, the fallback file (as authentication does); only a failing fallback is a
  warning (upstream's diagnostic only checks the fallback file).
- `azure-repos list ORG --show-remotes` lists the remotes of every organization, as upstream.

## 5. Tests

- One executable per directory (`test_core_base`, `test_core_gitcfg`, `test_core_app`, `test_core_net`,
  `test_core_stores`, `test_github`, `test_gitlab`, `test_bitbucket`, `test_azrepos`, `test_ui`), each holding several
  Qt Test classes registered with `GCM_REGISTER_TEST`. A test function ports the upstream method of the same name
  (class prefix dropped, `Async` dropped, lower camel case); the first comment of each test file says which upstream
  files it ports and what it leaves out. No network, no real credential store, no real user Git config.
- Tests that run `git` use `IsolatedGitConfig` (temporary `GIT_CONFIG_GLOBAL`, `GIT_CONFIG_NOSYSTEM=1`; upstream
  writes to the user's real global config). `createRepository` removes its repositories when the test process exits.
- The Secret Service tests that need a provider run only with `GCM_TEST_SECRETSERVICE=1` (they write to the keyring
  of the current session).
- Fakes differ from upstream's: `TestConsoleService` queues answers (`pushText`, `pushSelection`) instead of driving
  a Spectre `TestConsole`; `TestGitConfiguration::enumerate` stops at the first `false` from the callback;
  `TestDiagnosticReporter` keeps the exception's `what()`; `TestOAuth2Server::registerApplication` takes a
  `shared_ptr`; the `assert*` helpers record Qt Test failures. `TestApplicationFactory` lets a test executable use a
  `QApplication`.
- UI: `DialogWindow::setShowHook` is the seam. `test_ui` and `git-credential-manager-ui-scripted` (actions from
  `GCM_UI_SCRIPT`) drive the real dialogs offscreen; `GCM_UI_SCREENSHOT_DIR=<dir>` makes `test_ui` save every dialog
  in light and dark.
- Upstream tests not ported (every `[Fact]`/`[Theory]` name was matched against the port): null-argument cases
  (`QString` and references cannot be null); `Application_ContainsInterrupt` with `AggregateException` (no aggregate
  type in synchronous code; the inner-exception chain is tested); the CRLF `TextWriter` variants of
  `StreamExtensionsTests`; `GitConfigurationKeyComparer_Compare` (only equality is needed);
  `EnumerableExtensionsTests`; async `RunAsync`/`ReadKeyAsync` cancellation cases; `WslUtilsTests` and every
  `[WindowsFact]`, `Interop/Windows` and `Interop/MacOS` test; `U8StringConverterTests` (P/Invoke marshalling);
  `DispatcherTests`; `MsalParentWindowAdapterTests`; `Settings_TlsBackend` (Schannel).

### 5.1 End-to-end scripts (`tests/e2e`)

- `compare-with-upstream.sh <port> <upstream>`: 16 scenarios, each run with both binaries in an isolated HOME (no
  display, no system config, prompting disabled), output normalised and diffed. Covers capability, help and parse
  errors, autodetect, version, plaintext / cache / gpg stores, missing / unknown / none store, the helper protocol
  without git, configure / unconfigure, TRACE2 (event, normal and perf formats), diagnose (console output and log,
  with and without a store) and Azure Repos (commands, PAT store, OAuth sign-in/out, identity configuration errors).
  Normalised away: paths, ports, timings, pids, the `+<commit>` version suffix, upstream's stack traces, the Entra cache
  section of the diagnose log, and the TRACE2 argv[0] difference (4.6). diagnose runs under `env -i` because the
  Environment diagnostic logs every variable. Both sides use same-length directory names so messages containing paths
  wrap at the same column.
- `ui-helper.sh <git-credential-manager> <git-credential-manager-ui-scripted>`: 33 checks of the graphical prompts
  through `git credential fill`: basic, cancel, generic OAuth (mode prompt, device code), GitHub (token, password +
  two-factor, device code, account selection), GitLab (token, browser OAuth, reuse, expiry + refresh, erase), Bitbucket
  Data Center (password + validation, browser OAuth, refresh). Local fake servers; a fake `xdg-open` plays the browser.
- `pty-prompt.py <binary> [typed|ctrlc]`: the terminal prompt on a pseudo-terminal.

## 6. Open items

- Manual checks with real accounts: GitHub, GitLab, Bitbucket and Azure Repos sign-in (Entra against Microsoft's real
  endpoints); the Secret Service store on a real desktop session (end to end it is only covered by the gated unit
  tests); the `gpg` store with a real key.
- Packaging (distribution packages); `cmake --install` installs both executables to `bin/`.
- Qt version floor is set to 6.5 but only 6.12 has been used.
