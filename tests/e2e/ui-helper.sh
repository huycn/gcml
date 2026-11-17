#!/usr/bin/env bash
# End-to-end check of the graphical prompts (PLAN.md section 6, step 4): git-credential-manager runs the UI helper
# as a child process, exactly as in a desktop session, but the helper is git-credential-manager-ui-scripted (the
# real dialogs on the offscreen platform with a scripted user, see tests/uiscripted/main.cpp).
#
#   tests/e2e/ui-helper.sh <git-credential-manager> <git-credential-manager-ui-scripted>
#
# Scenarios: basic prompt (fill, then the stored credential is used without a prompt), cancelled basic prompt
# (git is told to quit), generic OAuth with the mode prompt and the device code dialog against a local fake OAuth
# server, the GitHub provider (token, password + two-factor code, device code, account selection) against a
# fake GitHub Enterprise server, and the GitLab provider (token; browser OAuth with token storage, reuse, expiry
# and refresh) against a fake GitLab server, and the Bitbucket provider (Data Center: password with validation of the
# stored password; browser OAuth with refresh token, expiry and refresh) against a fake Bitbucket Data Center. A fake `xdg-open` first on PATH plays the browser: it fetches the
# authorize URL and follows the redirect to GCM's loopback listener, so no real browser is ever opened.
# Exit status: 0 when all scenarios pass.
set -u

GCM=$(realpath "$1")
HELPER=$(realpath "$2")
WORK=$(mktemp -d)
SERVER_PID=
cleanup() {
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null
    [ -n "${GH_PID:-}" ] && kill "$GH_PID" 2>/dev/null
    [ -n "${GL_PID:-}" ] && kill "$GL_PID" 2>/dev/null
    [ -n "${BB_PID:-}" ] && kill "$BB_PID" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT
failures=0

export HOME="$WORK/home" GIT_CONFIG_GLOBAL="$WORK/home/.gitconfig" GIT_CONFIG_NOSYSTEM=1
# A desktop session as far as GCM can tell; the scripted helper itself renders offscreen
export DISPLAY=:gcm-e2e GCM_UI_HELPER="$HELPER" GIT_TERMINAL_PROMPT=0
export GCM_UI_SCRIPT_LOG="$WORK/ui.log"
unset WAYLAND_DISPLAY GCM_TRACE GIT_TRACE2 GIT_TRACE2_EVENT GIT_TRACE2_PERF GCM_CREDENTIAL_STORE GCM_INTERACTIVE
mkdir -p "$HOME" "$WORK/bin"
touch "$GIT_CONFIG_GLOBAL"
cat >"$WORK/bin/xdg-open" <<'SH'
#!/bin/sh
echo "$1" >>"$(dirname "$0")/../browser.log"
curl -s -L -o /dev/null "$1" &
SH
chmod +x "$WORK/bin/xdg-open"
export PATH="$WORK/bin:$PATH"
STORE=(-c credential.credentialStore=plaintext -c "credential.plaintextStorePath=$WORK/store")

cred() {
    local op=$1
    shift
    git -c credential.helper= -c "credential.helper=$GCM" -c credential.provider=generic "${STORE[@]}" "$@" credential "$op" 2>&1
    echo "[exit $?]"
}

check() {
    local name=$1 expected=$2 actual=$3
    if [ "$expected" = "$actual" ]; then
        echo "PASS  $name"
    else
        echo "FAIL  $name"
        diff -u <(echo "$expected") <(echo "$actual") | sed 's/^/      /'
        failures=$((failures + 1))
    fi
}

# --- basic ------------------------------------------------------------------------------------
: >"$GCM_UI_SCRIPT_LOG"
out=$(printf 'protocol=https\nhost=example.com\n\n' |
    GCM_UI_SCRIPT='type:userName=alice;type:password=s3cret pw;click:signIn' cred fill)
check "basic: fill through the dialog" "protocol=https
host=example.com
username=alice
password=s3cret pw
[exit 0]" "$out"
check "basic: one dialog shown" "Git Credential Manager|" "$(cat "$GCM_UI_SCRIPT_LOG")"

out=$(printf 'protocol=https\nhost=example.com\nusername=alice\npassword=s3cret pw\n\n' | cred approve)
check "basic: approve" "[exit 0]" "$out"
: >"$GCM_UI_SCRIPT_LOG"
out=$(printf 'protocol=https\nhost=example.com\n\n' | GCM_UI_SCRIPT='cancel' cred fill)
check "basic: stored credential, no dialog" "protocol=https
host=example.com
username=alice
password=s3cret pw
[exit 0]" "$out"
check "basic: no dialog shown" "" "$(cat "$GCM_UI_SCRIPT_LOG")"

# --- cancelled --------------------------------------------------------------------------------
out=$(printf 'protocol=https\nhost=other.example.com\n\n' | GCM_UI_SCRIPT='cancel' cred fill)
check "cancel: git is told to quit" "info: user cancelled the credential request.
fatal: credential helper '$GCM' told us to quit
[exit 128]" "$out"

# --- generic OAuth: mode prompt + device code -------------------------------------------------
cat >"$WORK/server.py" <<'PY'
import json, sys
from http.server import BaseHTTPRequestHandler, HTTPServer
polls = {"n": 0}
class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def reply(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def do_POST(self):
        self.rfile.read(int(self.headers.get("Content-Length", 0)))
        port = self.server.server_address[1]
        if self.path == "/device":
            self.reply(200, {"device_code": "dc-1", "user_code": "WXYZ-1234", "interval": 1, "expires_in": 300,
                             "verification_uri": f"http://127.0.0.1:{port}/verify"})
        elif self.path == "/token":
            polls["n"] += 1
            if polls["n"] < 2:
                self.reply(400, {"error": "authorization_pending"})
            else:
                self.reply(200, {"access_token": "at-42", "token_type": "Bearer", "expires_in": 3600})
        else:
            self.reply(404, {})
srv = HTTPServer(("127.0.0.1", 0), H)
print(srv.server_address[1], flush=True)
srv.serve_forever()
PY
coproc SERVER { exec python3 -I "$WORK/server.py" 2>/dev/null; }
SERVER_PID=$SERVER_PID
read -r PORT <&"${SERVER[0]}"
base="http://127.0.0.1:$PORT"
OAUTH=(-c "credential.$base.oauthClientId=client" -c "credential.$base.oauthAuthorizeEndpoint=/authorize"
       -c "credential.$base.oauthTokenEndpoint=/token" -c "credential.$base.oauthDeviceEndpoint=/device"
       -c "credential.$base.oauthDefaultUserName=oauthuser")
: >"$GCM_UI_SCRIPT_LOG"
out=$(printf 'protocol=http\nhost=127.0.0.1:%s\n\n' "$PORT" |
    GCM_UI_SCRIPT='click:signInDeviceCode;wait' cred fill "${OAUTH[@]}" | rg -v '^warning: |unsaferemotes')
check "oauth: device code flow through the dialogs" "protocol=http
host=127.0.0.1:$PORT
username=oauthuser
password=at-42
[exit 0]" "$out"
check "oauth: mode prompt, then device code dialog" "Git Credential Manager|
Device code authentication|" "$(cat "$GCM_UI_SCRIPT_LOG")"
if pgrep -f "$HELPER" >/dev/null; then
    echo "FAIL  oauth: device code dialog still running"
    failures=$((failures + 1))
else
    echo "PASS  oauth: device code dialog closed by GCM"
fi

# --- GitHub (Enterprise on a fake server) -----------------------------------------------------
cat >"$WORK/github.py" <<'PY'
import base64, json
from http.server import BaseHTTPRequestHandler, HTTPServer
state = {"polls": 0}
class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def reply(self, code, obj, headers=()):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        for k, v in headers:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)
    def do_GET(self):
        auth = self.headers.get("Authorization", "")
        if self.path == "/api/v3/meta":
            self.reply(200, {"installed_version": "3.12.0", "verifiable_password_authentication": True},
                       [("X-GitHub-Request-Id", "1")])
        elif self.path == "/api/v3/user":
            logins = {"Bearer ghp_typed": "token-user", "Bearer ghp_from_basic": "basic-user", "Bearer gho_device": "device-user"}
            if auth in logins:
                self.reply(200, {"login": logins[auth]})
            else:
                self.reply(401, {"message": "Bad credentials"})
        else:
            self.reply(404, {})
    def do_POST(self):
        self.rfile.read(int(self.headers.get("Content-Length", 0)))
        port = self.server.server_address[1]
        if self.path == "/api/v3/authorizations":
            expected = "Basic " + base64.b64encode(b"octo:pass word").decode()
            if self.headers.get("Authorization") != expected:
                self.reply(401, {"message": "Bad credentials"})
            elif self.headers.get("X-GitHub-OTP") != "135790":
                self.reply(401, {"message": "Must specify two-factor authentication OTP code."}, [("X-GitHub-OTP", "required; app")])
            else:
                self.reply(201, {"token": "ghp_from_basic"})
        elif self.path == "/login/device/code":
            self.reply(200, {"device_code": "dc", "user_code": "GH12-3456", "interval": 1, "expires_in": 300,
                             "verification_uri": f"http://127.0.0.1:{port}/login/device"})
        elif self.path == "/login/oauth/access_token":
            state["polls"] += 1
            if state["polls"] < 2:
                self.reply(200, {"error": "authorization_pending"})
            else:
                self.reply(200, {"access_token": "gho_device", "token_type": "bearer", "scope": "repo,gist,workflow"})
        else:
            self.reply(404, {})
srv = HTTPServer(("127.0.0.1", 0), H)
print(srv.server_address[1], flush=True)
srv.serve_forever()
PY
coproc GHSERVER { exec python3 -I "$WORK/github.py" 2>/dev/null; }
GH_PID=$GHSERVER_PID
read -r GHPORT <&"${GHSERVER[0]}"
GH=(-c credential.provider=github -c credential.allowUnsafeRemotes=true)
ghfill() {
    # Each GitHub scenario starts without stored accounts unless it adds them itself
    [ -n "${KEEP_STORE:-}" ] || rm -rf "$WORK/store"
    printf 'protocol=http\nhost=127.0.0.1:%s\n%s\n' "$GHPORT" "${1:-}" | cred fill "${GH[@]}" | rg -v '^warning: |unsaferemotes'
}

: >"$GCM_UI_SCRIPT_LOG"
out=$(GCM_UI_SCRIPT='tab:1;type:token=ghp_typed;click:signInToken' ghfill)
check "github: token tab, login resolved" "protocol=http
host=127.0.0.1:$GHPORT
username=token-user
password=ghp_typed
[exit 0]" "$out"
check "github: Enterprise offers every mode" "Connect to GitHub|Browser/Device,Token,Password" "$(cat "$GCM_UI_SCRIPT_LOG")"

: >"$GCM_UI_SCRIPT_LOG"
out=$(GCM_UI_SCRIPT='tab:2;type:userName=octo;type:password=pass word;click:signInBasic;type:digit1=1;type:digit2=3;type:digit3=5;type:digit4=7;type:digit5=9;type:digit6=0;click:verify' ghfill 2>&1)
check "github: password + two-factor code creates a token" "protocol=http
host=127.0.0.1:$GHPORT
username=basic-user
password=ghp_from_basic
[exit 0]" "$(echo "$out" | rg -v 'scripted helper')"
check "github: prompt, then two-factor dialog" "Connect to GitHub|Browser/Device,Token,Password
Two-factor authentication required|" "$(cat "$GCM_UI_SCRIPT_LOG")"

: >"$GCM_UI_SCRIPT_LOG"
out=$(GCM_UI_SCRIPT='click:signInDevice;wait' ghfill 2>&1 | rg -v 'scripted helper')
check "github: device code flow" "protocol=http
host=127.0.0.1:$GHPORT
username=device-user
password=gho_device
[exit 0]" "$out"
check "github: prompt, then device code dialog" "Connect to GitHub|Browser/Device,Token,Password
Device code authentication|" "$(cat "$GCM_UI_SCRIPT_LOG")"

# Two stored accounts: the account dialog picks one, no sign-in prompt
rm -rf "$WORK/store"
for acct in token-user:ghp_typed device-user:gho_device; do
    printf 'protocol=http\nhost=127.0.0.1:%s\nusername=%s\npassword=%s\n\n' "$GHPORT" "${acct%%:*}" "${acct#*:}" | cred approve "${GH[@]}" >/dev/null
done
: >"$GCM_UI_SCRIPT_LOG"
out=$(GCM_UI_SCRIPT='select:1;click:continue' KEEP_STORE=1 ghfill)
# The store lists accounts in file system order: check that the second account came back with its own token
case "$out" in
*"username=token-user"$'\n'"password=ghp_typed"$'\n[exit 0]'|*"username=device-user"$'\n'"password=gho_device"$'\n[exit 0]')
    echo "PASS  github: account selection" ;;
*)
    echo "FAIL  github: account selection"; echo "$out" | sed 's/^/      /'; failures=$((failures + 1)) ;;
esac
check "github: one account dialog" "Select an account|" "$(cat "$GCM_UI_SCRIPT_LOG")"
kill "$GH_PID" 2>/dev/null

# --- GitLab (self-managed, on a fake server) ---------------------------------------------------
cat >"$WORK/gitlab.py" <<'PY'
import json, urllib.parse, os, sys
from http.server import BaseHTTPRequestHandler, HTTPServer
expired_flag = sys.argv[1]
class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def reply(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        q = dict(urllib.parse.parse_qsl(url.query))
        if url.path == "/oauth/authorize":
            ok = q.get("client_id") == "test-client" and q.get("scope") == "write_repository read_repository"
            target = q["redirect_uri"] + ("?code=gl-code&state=" if ok else "?error=bad_request&state=") + urllib.parse.quote(q.get("state", ""))
            self.send_response(302)
            self.send_header("Location", target)
            self.send_header("Content-Length", "0")
            self.end_headers()
        elif url.path == "/oauth/token/info":
            expired = os.path.exists(expired_flag) and self.headers.get("Authorization") == "Bearer glo-1"
            self.reply(401 if expired else 200, {})
        else:
            self.reply(404, {})
    def do_POST(self):
        form = dict(urllib.parse.parse_qsl(self.rfile.read(int(self.headers.get("Content-Length", 0))).decode()))
        if self.path == "/oauth/token" and form.get("grant_type") == "authorization_code" and form.get("code") == "gl-code":
            self.reply(200, {"access_token": "glo-1", "refresh_token": "glr-1", "token_type": "bearer", "expires_in": 7200})
        elif self.path == "/oauth/token" and form.get("grant_type") == "refresh_token" and form.get("refresh_token") == "glr-1":
            self.reply(200, {"access_token": "glo-2", "refresh_token": "glr-2", "token_type": "bearer", "expires_in": 7200})
        else:
            self.reply(400, {"error": "invalid_grant"})
srv = HTTPServer(("127.0.0.1", 0), H)
print(srv.server_address[1], flush=True)
srv.serve_forever()
PY
coproc GLSERVER { exec python3 -I "$WORK/gitlab.py" "$WORK/gitlab-expired" 2>/dev/null; }
GL_PID=$GLSERVER_PID
read -r GLPORT <&"${GLSERVER[0]}"
GL=(-c credential.provider=gitlab -c credential.allowUnsafeRemotes=true -c credential.gitLabDevClientId=test-client)
glfill() {
    printf 'protocol=http\nhost=127.0.0.1:%s\n\n' "$GLPORT" | cred fill "${GL[@]}" | rg -v '^warning: |unsaferemotes'
}
glstore() {
    (cd "$WORK/store" 2>/dev/null && find . -type f | sort | while read -r f; do echo "$f: $(head -n1 "$f")"; done)
}

rm -rf "$WORK/store"
: >"$GCM_UI_SCRIPT_LOG"
out=$(GCM_UI_SCRIPT='tab:1;type:tokenUserName=jo;type:token=glpat-x;click:signInToken' glfill)
check "gitlab: token tab with user name" "protocol=http
host=127.0.0.1:$GLPORT
username=jo
password=glpat-x
[exit 0]" "$out"
check "gitlab: OAuth configured, every mode offered" "Connect to GitLab|Browser,Token,Password" "$(cat "$GCM_UI_SCRIPT_LOG")"

rm -rf "$WORK/store" "$WORK/browser.log"
out=$(GCM_UI_SCRIPT='click:signInBrowser' glfill 2>&1 | rg -v '^info: please complete')
check "gitlab: browser OAuth" "protocol=http
host=127.0.0.1:$GLPORT
username=oauth2
password=glo-1
[exit 0]" "$out"
check "gitlab: browser opened at the authorize endpoint" "1" "$(rg -c "^http://127\.0\.0\.1:$GLPORT/oauth/authorize\?" "$WORK/browser.log")"
check "gitlab: access and refresh tokens stored" "./git/http/127.0.0.1:$GLPORT/oauth2.credential: glo-1
./git/http/oauth-refresh-token.127.0.0.1:$GLPORT/oauth2.credential: glr-1" "$(glstore)"

: >"$GCM_UI_SCRIPT_LOG"
out=$(GCM_UI_SCRIPT='cancel' glfill)
check "gitlab: valid stored token reused without a dialog" "protocol=http
host=127.0.0.1:$GLPORT
username=oauth2
password=glo-1
[exit 0]|" "$out|$(cat "$GCM_UI_SCRIPT_LOG")"

touch "$WORK/gitlab-expired"
out=$(GCM_UI_SCRIPT='cancel' glfill)
check "gitlab: expired token refreshed without a dialog" "protocol=http
host=127.0.0.1:$GLPORT
username=oauth2
password=glo-2
[exit 0]|" "$out|$(cat "$GCM_UI_SCRIPT_LOG")"
check "gitlab: refreshed tokens stored" "./git/http/127.0.0.1:$GLPORT/oauth2.credential: glo-2
./git/http/oauth-refresh-token.127.0.0.1:$GLPORT/oauth2.credential: glr-2" "$(glstore)"

out=$(printf 'protocol=http\nhost=127.0.0.1:%s\nusername=oauth2\n\n' "$GLPORT" | cred reject "${GL[@]}")
check "gitlab: erase removes both tokens" "[exit 0]|" "$out|$(glstore)"
kill "$GL_PID" 2>/dev/null

# --- Bitbucket Data Center (on a fake server) --------------------------------------------------
cat >"$WORK/bitbucket.py" <<'PY'
import base64, json, urllib.parse, os, sys
from http.server import BaseHTTPRequestHandler, HTTPServer
expired_flag = sys.argv[1]
class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def reply(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("X-AREQUESTID", "1")
        self.end_headers()
        self.wfile.write(body)
    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        q = dict(urllib.parse.parse_qsl(url.query))
        auth = self.headers.get("Authorization", "")
        if url.path == "/rest/authconfig/1.0/login-options":
            self.reply(200, {"results": [{"type": "LOGIN_FORM", "id": 1}, {"type": "IDP", "id": 2}]})
        elif url.path == "/rest/oauth2/1.0/client":
            self.reply(401, {})
        elif url.path == "/rest/api/1.0/users":
            valid = {"Basic " + base64.b64encode(b"jo:pass word").decode(), "Bearer bbat-2"}
            if not os.path.exists(expired_flag):
                valid.add("Bearer bbat-1")
            self.reply(200 if auth in valid else 401, {"values": []})
        elif url.path == "/rest/oauth2/latest/authorize":
            ok = q.get("client_id") == "dc-client" and q.get("scope") == "PUBLIC_REPOS REPO_READ REPO_WRITE"
            target = q["redirect_uri"] + ("?code=bb-code&state=" if ok else "?error=bad_request&state=") + urllib.parse.quote(q.get("state", ""))
            self.send_response(302)
            self.send_header("Location", target)
            self.send_header("Content-Length", "0")
            self.end_headers()
        else:
            self.reply(404, {})
    def do_POST(self):
        form = dict(urllib.parse.parse_qsl(self.rfile.read(int(self.headers.get("Content-Length", 0))).decode()))
        if self.path == "/rest/oauth2/latest/token" and form.get("client_secret") == "dc-secret":
            if form.get("grant_type") == "authorization_code" and form.get("code") == "bb-code":
                return self.reply(200, {"access_token": "bbat-1", "refresh_token": "bbrt-1", "token_type": "bearer", "scopes": "REPO_WRITE"})
            if form.get("grant_type") == "refresh_token" and form.get("refresh_token") == "bbrt-1":
                return self.reply(200, {"access_token": "bbat-2", "refresh_token": "bbrt-2", "token_type": "bearer"})
        self.reply(400, {"error": "invalid_grant"})
srv = HTTPServer(("127.0.0.1", 0), H)
print(srv.server_address[1], flush=True)
srv.serve_forever()
PY
coproc BBSERVER { exec python3 -I "$WORK/bitbucket.py" "$WORK/bitbucket-expired" 2>/dev/null; }
BB_PID=$BBSERVER_PID
read -r BBPORT <&"${BBSERVER[0]}"
BB=(-c credential.provider=bitbucket -c credential.bitbucketDataCenterOAuthClientId=dc-client -c credential.bitbucketDataCenterOAuthClientSecret=dc-secret -c credential.bitbucketDataCenterOauthRedirectUri=http://127.0.0.1/)
bbcred() {
    local op=$1
    shift
    printf 'protocol=http\nhost=127.0.0.1:%s\n%s\n' "$BBPORT" "$*" | cred "$op" "${BB[@]}" | rg -v '^info: please complete'
}
bbstore() {
    (cd "$WORK/store" 2>/dev/null && find . -type f | sort | while read -r f; do echo "$f: $(head -n1 "$f")"; done)
}

rm -rf "$WORK/store"
: >"$GCM_UI_SCRIPT_LOG"
out=$(GCM_UI_SCRIPT='tab:1;type:userName=jo;type:password=pass word;click:signInBasic' bbcred fill)
check "bitbucket: password tab" "protocol=http
host=127.0.0.1:$BBPORT
username=jo
password=pass word
[exit 0]" "$out"
check "bitbucket: Data Center offers browser and password" "Connect to Bitbucket|Browser,Password/Token" "$(cat "$GCM_UI_SCRIPT_LOG")"
bbcred approve "username=jo
password=pass word" >/dev/null
: >"$GCM_UI_SCRIPT_LOG"
out=$(GCM_UI_SCRIPT='cancel' bbcred fill)
check "bitbucket: stored password validated, no dialog" "protocol=http
host=127.0.0.1:$BBPORT
username=jo
password=pass word
[exit 0]|" "$out|$(cat "$GCM_UI_SCRIPT_LOG")"

rm -rf "$WORK/store" "$WORK/browser.log"
out=$(GCM_UI_SCRIPT='click:signInBrowser' bbcred fill)
check "bitbucket: browser OAuth" "protocol=http
host=127.0.0.1:$BBPORT
username=OAUTH_USERNAME
password=bbat-1
[exit 0]" "$out"
check "bitbucket: refresh token stored" "./git/http/127.0.0.1:$BBPORT/refresh_token/OAUTH_USERNAME.credential: bbrt-1" "$(bbstore)"
bbcred approve "username=OAUTH_USERNAME
password=bbat-1" >/dev/null
touch "$WORK/bitbucket-expired"
: >"$GCM_UI_SCRIPT_LOG"
out=$(GCM_UI_SCRIPT='cancel' bbcred fill)
check "bitbucket: expired token refreshed without a dialog" "protocol=http
host=127.0.0.1:$BBPORT
username=OAUTH_USERNAME
password=bbat-2
[exit 0]|" "$out|$(cat "$GCM_UI_SCRIPT_LOG")"
check "bitbucket: rotated refresh token stored" "./git/http/127.0.0.1:$BBPORT/OAUTH_USERNAME.credential: bbat-1
./git/http/127.0.0.1:$BBPORT/refresh_token/OAUTH_USERNAME.credential: bbrt-2" "$(bbstore)"
kill "$BB_PID" 2>/dev/null

[ "$failures" -eq 0 ]
