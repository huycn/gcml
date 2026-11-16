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
# Lines that only upstream prints because of host providers this port does not have yet
# Scenarios that cannot match before the host providers are ported (upstream's providers force the HEAD probe)
EXPECTED_DIFF="autodetect"
UNPORTED='azure-repos  Commands for interacting|github       Commands for interacting|Azure Repos provider|https://dev\.azure\.com|useHttpPath = true'

trap 'rm -rf "$WORK"' EXIT
failures=0

# run_scenario <name> <function>: runs <function> for each binary with $GCM pointing to it, captures its output
run_scenario() {
    local name=$1 fn=$2
    for side in port upstream; do
        local gcm=$PORT
        [ "$side" = upstream ] && gcm=$UPSTREAM
        local dir="$WORK/$name/$side"
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
    if diff -u "$WORK/$name/upstream/out.txt" "$WORK/$name/port/out.txt" >"$WORK/$name/diff.txt"; then
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
    export GIT_TRACE2_EVENT="$DIR/events.json"
    printf 'protocol=https\nhost=example.com\nusername=alice\npassword=s3cret\n\n' |
        GCM_CREDENTIAL_STORE=plaintext GCM_PLAINTEXT_STORE_PATH="$DIR/store" GCM_PROVIDER=generic "$GCM" store
    echo "[exit $?]"
    # Event sequence of GCM's own process (sid without nesting), without timing and source locations
    python3 - "$DIR/events.json" <<'PY'
import json, sys
for line in open(sys.argv[1]):
    e = json.loads(line)
    keep = {k: e[k] for k in ("event", "thread", "depth", "category", "label", "key", "value", "name", "code", "child_class", "msg") if k in e}
    if "argv" in e:
        keep["argv"] = [a.rsplit("/", 1)[-1] for a in e["argv"]]
    print(json.dumps(keep, sort_keys=True))
PY
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

run_scenario capability scenario_capability
run_scenario help scenario_help
run_scenario autodetect scenario_autodetect
run_scenario version scenario_version
run_scenario plaintext scenario_plaintext
run_scenario cache scenario_cache
run_scenario no-store scenario_no_store
run_scenario none-store scenario_none_store
run_scenario direct-get scenario_direct_get
run_scenario configure scenario_configure
run_scenario trace2 scenario_trace2

echo
[ "$failures" -eq 0 ] && echo "All scenarios match." || echo "$failures scenario(s) differ."
exit $((failures > 0))
