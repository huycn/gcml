#!/usr/bin/env bash
# End-to-end comparison of this port with the .NET Git Credential Manager (PLAN.md section 6, step 3).
#
#   tests/e2e/compare-with-upstream.sh <port-binary> <upstream-binary>
#
# Each scenario runs once per binary in its own temporary HOME with an isolated global Git configuration, no system
# configuration, no display (so no GUI prompts) and no network (the generic provider is selected explicitly). The
# observable results -- stdout, stderr, exit codes and the files the stores leave behind -- are normalised (binary
# path, temp dir) and diffed. Exit status: 0 when all scenarios match.
set -u

PORT=$(realpath "$1")
UPSTREAM=$(realpath "$2")
WORK=$(mktemp -d)
# Scenarios that cannot match before the host providers are ported (upstream's providers force the HEAD probe)
EXPECTED_DIFF=""
# Lines that only upstream prints because of host providers this port does not have yet (all ported now)
UNPORTED=''

trap 'rm -rf "$WORK"' EXIT
failures=0

# run_scenario <name> <function>: runs <function> for each binary with $GCM pointing to it, captures its output
run_scenario() {
    local name=$1 fn=$2
    for side in port upstream; do
        local gcm=$PORT
        [ "$side" = upstream ] && gcm=$UPSTREAM
        # Same-length directory names on both sides, so messages that contain a path wrap at the same place
        local dir="$WORK/$name/${side:0:4}"
        mkdir -p "$dir/home"
        (
            export HOME="$dir/home" GIT_CONFIG_GLOBAL="$dir/home/.gitconfig" GIT_CONFIG_NOSYSTEM=1
            export GCM="$gcm" DIR="$dir" GIT_TERMINAL_PROMPT=0
            unset DISPLAY WAYLAND_DISPLAY GCM_TRACE GIT_TRACE2 GIT_TRACE2_EVENT GIT_TRACE2_PERF GCM_CREDENTIAL_STORE
            touch "$GIT_CONFIG_GLOBAL"
            cd "$dir"
            "$fn"
        ) >"$dir/out.txt" 2>&1
        # Normalise paths that differ between the two runs, and the commit suffix of the informational version
        # (only present when the binary was built from a Git checkout)
        sed -i -E -e "s#$dir#<DIR>#g" -e "s#$gcm#<GCM>#g" -e 's#^([0-9]+\.[0-9]+\.[0-9]+)\+[0-9a-f]{40}$#\1#' \
            -e 's#127\.0\.0\.1:[0-9]+#127.0.0.1:<PORT>#g' "$dir/out.txt"
        # Host providers not ported yet: drop their lines on both sides. Remove a pattern once its provider exists.
        if [ -n "$UNPORTED" ]; then
            sed -i -E "\\#$UNPORTED#d" "$dir/out.txt"
        fi
    done
    if diff -u "$WORK/$name/upst/out.txt" "$WORK/$name/port/out.txt" >"$WORK/$name/diff.txt"; then
        echo "PASS  $name"
    elif [[ " $EXPECTED_DIFF " == *" $name "* ]]; then
        echo "DIFF  $name (expected until the host providers are ported)"
        sed 's/^/      /' "$WORK/$name/diff.txt"
    else
        echo "FAIL  $name"
        cat "$WORK/$name/diff.txt"
        failures=$((failures + 1))
    fi
}

# git credential <op> through GCM only (the empty helper resets any other helper)
cred() {
    local op=$1
    shift
    git -c credential.helper= -c "credential.helper=$GCM" -c credential.provider=generic "$@" credential "$op"
    echo "[exit $?]"
}

show_files() {
    (cd "$1" 2>/dev/null && find . -type f | sort | while read -r f; do echo "== $f"; cat "$f"; done)
}

scenario_capability() {
    "$GCM" capability
    echo "[exit $?]"
}

scenario_version() {
    "$GCM" --version
    echo "[exit $?]"
}

scenario_plaintext() {
    local cfg=(-c credential.credentialStore=plaintext -c "credential.plaintextStorePath=$DIR/store")
    echo "--- approve"
    printf 'protocol=https\nhost=example.com\npath=org/repo.git\nusername=alice\npassword=s3cret\n\n' | cred approve "${cfg[@]}"
    echo "--- second account"
    printf 'protocol=https\nhost=example.com\npath=org/repo.git\nusername=bob\npassword=hunter2\n\n' | cred approve "${cfg[@]}"
    echo "--- files"
    show_files "$DIR/store"
    echo "--- fill (bob)"
    printf 'protocol=https\nhost=example.com\npath=org/repo.git\nusername=bob\n\n' | cred fill "${cfg[@]}"
    echo "--- reject alice"
    printf 'protocol=https\nhost=example.com\npath=org/repo.git\nusername=alice\n\n' | cred reject "${cfg[@]}"
    show_files "$DIR/store"
    echo "--- fill alice (gone; prompting disabled)"
    printf 'protocol=https\nhost=example.com\npath=org/repo.git\nusername=alice\n\n' | cred fill "${cfg[@]}"
    echo "--- useHttpPath off: no path"
    printf 'protocol=http\nhost=example.com:8080\nusername=carol\npassword=pw\n\n' | cred approve "${cfg[@]}"
    show_files "$DIR/store"
}

# gpg store (GNU pass layout): a passphrase-less key in a private GNUPGHOME; files are shown by name and decrypted
scenario_gpg() {
    export GNUPGHOME="$DIR/gnupg"
    mkdir -m 700 "$GNUPGHOME"
    gpg --batch --quiet --pinentry-mode loopback --passphrase '' --quick-gen-key gcm-test@example.com default default never 2>/dev/null
    local store="$DIR/pass"
    local cfg=(-c credential.credentialStore=gpg -c "credential.gpgPassStorePath=$store")
    local alice='protocol=https\nhost=example.com\npath=org/repo.git\nusername=alice\npassword=s3cret\n\n'
    echo "--- no GPG_TTY (no desktop session)"
    printf "$alice" | cred approve "${cfg[@]}"
    export GPG_TTY=/dev/null
    echo "--- store not initialised"
    printf "$alice" | cred approve "${cfg[@]}"
    mkdir -p "$store"
    echo gcm-test@example.com >"$store/.gpg-id"
    echo "--- approve two accounts"
    printf "$alice" | cred approve "${cfg[@]}"
    printf 'protocol=https\nhost=example.com\npath=org/repo.git\nusername=bob\npassword=hunter2\n\n' | cred approve "${cfg[@]}"
    (cd "$store" && find . -type f | sort | while read -r f; do
        echo "== $f"
        case $f in *.gpg) gpg --batch --quiet --decrypt "$f" 2>&1 ;; *) cat "$f" ;; esac
    done)
    echo "--- fill (bob)"
    printf 'protocol=https\nhost=example.com\npath=org/repo.git\nusername=bob\n\n' | cred fill "${cfg[@]}"
    echo "--- reject alice, fill alice (gone; prompting disabled)"
    printf 'protocol=https\nhost=example.com\npath=org/repo.git\nusername=alice\n\n' | cred reject "${cfg[@]}"
    printf 'protocol=https\nhost=example.com\npath=org/repo.git\nusername=alice\n\n' | cred fill "${cfg[@]}"
    (cd "$store" && find . -type f | sort)
    gpgconf --kill gpg-agent
}

scenario_cache() {
    local cfg=(-c credential.credentialStore=cache -c "credential.cacheOptions=--socket $DIR/sock --timeout 60")
    printf 'protocol=https\nhost=example.com\nusername=alice\npassword=s3cret\n\n' | cred approve "${cfg[@]}"
    printf 'protocol=https\nhost=example.com\n\n' | cred fill "${cfg[@]}"
    printf 'protocol=https\nhost=example.com\nusername=alice\n\n' | cred reject "${cfg[@]}"
    printf 'protocol=https\nhost=example.com\n\n' | cred fill "${cfg[@]}"
    git credential-cache --socket "$DIR/sock" exit
}

scenario_no_store() {
    printf 'protocol=https\nhost=example.com\nusername=alice\npassword=s3cret\n\n' | cred approve
    printf 'protocol=https\nhost=example.com\n\n' | cred fill -c credential.credentialStore=bogus
}

scenario_none_store() {
    printf 'protocol=https\nhost=example.com\nusername=alice\npassword=s3cret\n\n' | cred approve -c credential.credentialStore=none
    printf 'protocol=https\nhost=example.com\n\n' | cred fill -c credential.credentialStore=none
}

scenario_direct_get() {
    # The helper protocol without git: capabilities, state, missing arguments
    export GCM_CREDENTIAL_STORE=plaintext GCM_PLAINTEXT_STORE_PATH="$DIR/store" GCM_PROVIDER=generic
    printf 'protocol=https\nhost=example.com\nusername=alice\npassword=s3cret\n\n' | "$GCM" store
    echo "[exit $?]"
    printf 'capability[]=authtype\ncapability[]=state\ncapability[]=unknown\nprotocol=https\nhost=example.com\n\n' | "$GCM" get
    echo "[exit $?]"
    printf 'host=example.com\n\n' | "$GCM" get
    echo "[exit $?]"
    printf 'protocol=https\n\n' | "$GCM" erase
    echo "[exit $?]"
    "$GCM" bogus-command
    echo "[exit $?]"
}

scenario_configure() {
    git config --global credential.helper other-helper
    "$GCM" configure
    echo "[exit $?]"
    sed "s#$GCM#<GCM>#" "$GIT_CONFIG_GLOBAL"
    "$GCM" configure
    git config --global --get-all credential.helper | sed "s#$GCM#<GCM>#"
    "$GCM" unconfigure
    echo "[exit $?]"
    git config --global --get-all credential.helper
}

scenario_trace2() {
    # All three formats at once; the git children GCM starts write to the same targets (as with Git itself)
    export GIT_TRACE2_EVENT="$DIR/events.json" GIT_TRACE2="$DIR/normal.txt" GIT_TRACE2_PERF="$DIR/perf.txt"
    printf 'protocol=https\nhost=example.com\nusername=alice\npassword=s3cret\n\n' |
        GCM_CREDENTIAL_STORE=plaintext GCM_PLAINTEXT_STORE_PATH="$DIR/store" GCM_PROVIDER=generic "$GCM" store
    echo "[exit $?]"
    # Event sequence of GCM's own process (sid without nesting), without timing and source locations
    echo "--- event"
    python3 - "$DIR/events.json" <<'PY'
import json, sys
for line in open(sys.argv[1]):
    e = json.loads(line)
    keep = {k: e[k] for k in ("event", "thread", "depth", "category", "label", "key", "value", "name", "code", "child_class", "msg") if k in e}
    if "argv" in e:
        keep["argv"] = [a.rsplit("/", 1)[-1] for a in e["argv"]]
    if "pid" in e:
        keep["pid"] = "<PID>" if e["pid"] > 0 else e["pid"]
    print(json.dumps(keep, sort_keys=True))
PY
    # Normal and perf formats without wall clock, source location (file names differ by nature), times and pids. The
    # first git child (reading trace2.* settings) logs its argv[0], which QProcess always resolves to a full path.
    for format in normal perf; do
        echo "--- $format"
        python3 - "$format" "$DIR/$format.txt" "$(command -v git)" <<'PY'
import re, sys
fmt, path, git = sys.argv[1:]
for line in open(path):
    line = line.rstrip("\n")[16:]
    if fmt == "normal":
        s = line[34:]
    else:
        cols = [c.strip() for c in line.split("|")][1:]
        cols[4] = re.sub(r"[0-9.]+", "<T>", cols[4])
        cols[5] = re.sub(r"[0-9.]+", "<T>", cols[5])
        s = " | ".join(cols)
    s = re.sub(r"elapsed:[0-9.E-]+", "elapsed:<T>", s)
    s = re.sub(r"pid:[1-9][0-9]*", "pid:<PID>", s)
    s = re.sub(r"\b[0-9]+\.[0-9]+\.[0-9]+\+[0-9a-f]{40}\b", "<VERSION>", s)
    s = s.replace(git + " config -z --get-regexp", "git config -z --get-regexp")
    print(s)
PY
    done
}

scenario_help() {
    for args in "--help" "configure --help" "get --help" "diagnose --output" "configure --bogus" "--no-ui" ""; do
        echo "--- $args"
        # shellcheck disable=SC2086
        "$GCM" $args
        echo "[exit $?]"
    done
}

scenario_autodetect() {
    # No provider configured: one HEAD probe of the remote, then the result is remembered in the global config.
    python3 -u -m http.server 0 --bind 127.0.0.1 --directory "$DIR" >"$DIR/server.log" 2>&1 &
    local server=$!
    local port=""
    for _ in $(seq 50); do
        port=$(sed -nE 's/.*port ([0-9]+).*/\1/p' "$DIR/server.log" | head -1)
        [ -n "$port" ] && break
        sleep 0.1
    done
    local cfg=(-c credential.credentialStore=plaintext -c "credential.plaintextStorePath=$DIR/store" -c credential.allowUnsafeRemotes=true)
    printf 'protocol=http\nhost=127.0.0.1:%s\nusername=alice\npassword=s3cret\n\n' "$port" |
        git -c credential.helper= -c "credential.helper=$GCM" "${cfg[@]}" credential approve
    echo "[exit $?]"
    printf 'protocol=http\nhost=127.0.0.1:%s\n\n' "$port" |
        git -c credential.helper= -c "credential.helper=$GCM" "${cfg[@]}" credential fill
    echo "[exit $?]"
    kill "$server"
    cat "$GIT_CONFIG_GLOBAL"
    grep -c HEAD "$DIR/server.log" | sed 's/^/HEAD requests: /'
}

# diagnose: the console output (wrapped like Spectre.Console at 80 columns) and the log file. Run twice: with a store
# (every diagnostic passes) and without one (Credential storage fails with an exception). No D-Bus, so neither build
# touches the user's keyring; the Networking and GitHub API diagnostics reach the internet when it is available.
scenario_diagnose() {
    for store in plaintext ""; do
        echo "=== store: ${store:-<none>}"
        # A minimal environment: the Environment diagnostic logs every variable
        env -i HOME="$HOME" PATH="$PATH" LANG=C.UTF-8 GIT_CONFIG_GLOBAL="$GIT_CONFIG_GLOBAL" GIT_CONFIG_NOSYSTEM=1 \
            GCM_CREDENTIAL_STORE="$store" "$GCM" diagnose --output "$DIR/diag-$store" |
            # Spectre's status spinner prints the running diagnostic's name when it is slow enough (timing)
            grep -v -x -E 'Environment|File system|Networking|Git|Credential storage|Microsoft Entra authentication|GitHub API' |
            # Stack trace of upstream (wrapped, so dropped from its first line up to the blank line that ends it)
            awk '/^   at /{trace=1} trace&&/^$/{trace=0} !trace' |
            sed -E 's/^(GIT_TRACE2_PARENT_SID=).*/\1<SID>/'
        echo "[exit ${PIPESTATUS[0]}]"
        echo "--- log"
        # Log normalised: timestamp, binary/version header, temp file names, ports; environment variables sorted
        # (upstream prints them in hash order); stack traces (C++ has none) and the Entra token cache section
        # (our own cache instead of MSAL's, PLAN.md 1.4) removed.
        python3 - "$DIR/diag-$store/gcm-diagnose.log" <<'PY'
import re, sys
text = open(sys.argv[1], encoding="utf-8").read()
print("BOM" if text.startswith("﻿") else "no BOM")
out, env, section = [], [], None
for line in text.lstrip("﻿").split("\n"):
    if line.startswith("Diagnostic: "):
        section = line[len("Diagnostic: "):]
    if section is None and re.match(r"(Diagnose log at |AppPath: |InstallDir: |Version: )", line):
        continue
    if section == "Environment" and re.match(r"[A-Za-z_][A-Za-z0-9_]*=", line):
        env.append(re.sub(r"^(GIT_TRACE2_PARENT_SID=).*", r"\1<SID>", line))
        continue
    if env:
        out.extend(sorted(env))
        env = []
    if line.startswith("   at "):
        continue
    if section == "Microsoft Entra authentication" and line not in ("------------", "Log:") and not line.startswith(("Diagnostic:", "Outcome:")):
        continue
    line = re.sub(r"temporary file '/tmp/[0-9a-f]+'", "temporary file '/tmp/<TEMP>'", line)
    line = re.sub(r"(Got port |localhost:)[0-9]+", r"\1<PORT>", line)
    out.append(line)
print("\n".join(out))
PY
    done
}

# git credential <op> for Azure Repos (provider auto-detected from the host name; no network unless a token is needed)
azcred() {
    local op=$1
    shift
    git -c credential.helper= -c "credential.helper=$GCM" -c credential.credentialStore=plaintext \
        -c "credential.plaintextStorePath=$DIR/store" -c credential.https://dev.azure.com.useHttpPath=true "$@" credential "$op"
    echo "[exit $?]"
}

scenario_azrepos_commands() {
    for args in "azure-repos --help" "azure-repos list --help" "azure-repos bind" "azure-repos list" "azure-repos bind org1 alice@example.com" \
        "azure-repos bind ORG2 bob@example.com" "azure-repos bind --local org1 carol" "azure-repos list" "azure-repos list org1" \
        "azure-repos unbind org1" "azure-repos list" "azure-repos clear-cache"; do
        echo "--- $args"
        # shellcheck disable=SC2086
        "$GCM" $args
        echo "[exit $?]"
    done
    cat "$GIT_CONFIG_GLOBAL"
    echo "--- inside a repository"
    git init -q repo && cd repo || return
    git remote add origin https://org1@dev.azure.com/org1/proj/_git/repo
    git remote add other https://alice@org3.visualstudio.com/proj/_git/repo
    git remote set-url --push other https://dev.azure.com/org3/proj%20x/_git/repo
    git remote add gh https://github.com/foo/bar
    "$GCM" azure-repos bind --local org1 carol
    "$GCM" azure-repos bind --local org3 ""
    "$GCM" azure-repos bind org3 dave
    "$GCM" azure-repos list --show-remotes
    echo "[exit $?]"
    "$GCM" azure-repos list --show-remotes -v org3
    echo "[exit $?]"
    "$GCM" azure-repos unbind --local org1
    "$GCM" azure-repos list --show-remotes --verbose
    echo "[exit $?]"
    cat .git/config
}

scenario_azrepos_pat() {
    local cfg=(-c credential.azreposCredentialType=pat)
    echo "--- store (dev.azure.com: organization service name)"
    printf 'protocol=https
host=dev.azure.com
path=org/proj/_git/repo
username=org
password=pat1

' | azcred approve "${cfg[@]}"
    echo "--- store (visualstudio.com: full path)"
    printf 'protocol=https
host=org.visualstudio.com
path=proj/_git/repo
username=alice
password=pat2

' | azcred approve "${cfg[@]}"
    show_files "$DIR/store"
    echo "--- get"
    printf 'protocol=https
host=dev.azure.com
path=org/other/_git/repo
username=org

' | azcred fill "${cfg[@]}"
    printf 'protocol=https
host=org.visualstudio.com
path=proj/_git/repo
username=alice

' | azcred fill "${cfg[@]}"
    echo "--- erase"
    printf 'protocol=https
host=dev.azure.com
path=org/proj/_git/repo

' | azcred reject "${cfg[@]}"
    printf 'protocol=https
host=org.visualstudio.com
path=proj/_git/repo
username=bob

' | azcred reject "${cfg[@]}"
    show_files "$DIR/store"
    echo "--- unknown credential type"
    printf 'protocol=http
host=dev.azure.com
path=org/proj/_git/repo

' | azcred fill -c credential.azreposCredentialType=bogus
}

scenario_azrepos_oauth() {
    echo "--- store signs the user in, erase signs out"
    printf 'protocol=https
host=dev.azure.com
path=org/proj/_git/repo
username=alice@example.com
password=token

' | azcred approve
    cat "$GIT_CONFIG_GLOBAL"
    git config --global credential.azrepos:org/org.azureAuthority https://login.microsoftonline.com/tenant
    printf 'protocol=https
host=dev.azure.com
path=org/proj/_git/repo
username=alice@example.com

' | azcred reject
    cat "$GIT_CONFIG_GLOBAL"
    echo "--- unsafe remote"
    printf 'protocol=http
host=dev.azure.com
path=org/proj/_git/repo

' | azcred fill
    echo "--- identity configuration errors"
    printf 'protocol=http
host=dev.azure.com
path=org/proj/_git/repo

' | azcred fill -c credential.azreposServicePrincipal=tenant
    printf 'protocol=http
host=dev.azure.com
path=org/proj/_git/repo

' | azcred fill -c credential.azreposServicePrincipal=tenant/client \
        -c credential.azreposServicePrincipalCertificateThumbprint=ABCDEF
    printf 'protocol=http
host=dev.azure.com
path=org/proj/_git/repo

' | azcred fill -c credential.azreposManagedIdentity=bogus
    printf 'protocol=http
host=dev.azure.com
path=org/proj/_git/repo

' | azcred fill -c credential.azreposWorkloadFederation=generic
    printf 'protocol=http
host=dev.azure.com
path=org/proj/_git/repo

' | azcred fill -c credential.azreposWorkloadFederation=generic \
        -c credential.azreposWorkloadFederationClientId=c -c credential.azreposWorkloadFederationTenantId=t
    printf 'protocol=http
host=dev.azure.com
path=org/proj/_git/repo

' | azcred fill -c credential.azreposWorkloadFederation=generic \
        -c credential.azreposWorkloadFederationClientId=c -c credential.azreposWorkloadFederationTenantId=t \
        -c credential.azreposWorkloadFederationAssertion=file:///nonexistent/assertion
    printf 'protocol=http
host=dev.azure.com
path=org/proj/_git/repo

' | azcred fill -c credential.azreposWorkloadFederation=mi \
        -c credential.azreposWorkloadFederationClientId=c -c credential.azreposWorkloadFederationTenantId=t
    printf 'protocol=http
host=dev.azure.com
path=org/proj/_git/repo

' | azcred fill -c credential.azreposWorkloadFederation=github \
        -c credential.azreposWorkloadFederationClientId=c -c credential.azreposWorkloadFederationTenantId=t
    echo "--- store/erase with an identity configured"
    printf 'protocol=https
host=dev.azure.com
path=org/proj/_git/repo
username=x
password=y

' | azcred approve -c credential.azreposManagedIdentity=system
    printf 'protocol=https
host=dev.azure.com
path=org/proj/_git/repo
username=x

' | azcred reject -c credential.azreposServicePrincipal=t/c
    echo "--- configure"
    "$GCM" configure
    git config --global --get credential.https://dev.azure.com.useHttpPath
    "$GCM" unconfigure
    git config --global --get credential.https://dev.azure.com.useHttpPath
    echo "[exit $?]"
}

run_scenario capability scenario_capability
run_scenario help scenario_help
run_scenario autodetect scenario_autodetect
run_scenario version scenario_version
run_scenario plaintext scenario_plaintext
run_scenario cache scenario_cache
run_scenario gpg scenario_gpg
run_scenario no-store scenario_no_store
run_scenario none-store scenario_none_store
run_scenario direct-get scenario_direct_get
run_scenario configure scenario_configure
run_scenario trace2 scenario_trace2
run_scenario diagnose scenario_diagnose
run_scenario azrepos-commands scenario_azrepos_commands
run_scenario azrepos-pat scenario_azrepos_pat
run_scenario azrepos-oauth scenario_azrepos_oauth

echo
[ "$failures" -eq 0 ] && echo "All scenarios match." || echo "$failures scenario(s) differ."
exit $((failures > 0))
