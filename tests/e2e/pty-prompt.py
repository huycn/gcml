#!/usr/bin/env python3
"""Drive `git-credential-manager get` through the terminal basic-credential prompt on a pseudo terminal.

    tests/e2e/pty-prompt.py <binary> [typed|ctrlc]

The Git request goes in on a stdin pipe (as Git does); the prompts appear on the pty, which is the child's
controlling terminal (/dev/tty). `typed` answers "alice" / "s3cret"; `ctrlc` types "ali" then Ctrl+C. Prints
what appeared on the terminal, stdout, stderr and the exit code, for comparison between the port and upstream.

Known difference in `ctrlc` (PLAN.md 1.4, main): upstream 3.0.2 on Linux uses the macOS termios flag values,
so its "raw" mode only clears ECHO -- the typed text is not echoed and Ctrl+C raises SIGINT (exit -2 here).
The port implements upstream's documented design: Ctrl+C is read as 0x03 and answered with quit=1.
"""
import fcntl, os, select, subprocess, sys, tempfile, termios, time

binary = sys.argv[1]
mode = sys.argv[2] if len(sys.argv) > 2 else "typed"
home = tempfile.mkdtemp()
master, slave = os.openpty()
slave_name = os.ttyname(slave)


def child_setup():
    os.setsid()
    fd = os.open(slave_name, os.O_RDWR)
    fcntl.ioctl(fd, termios.TIOCSCTTY, 0)


env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
env.update(HOME=home, GIT_CONFIG_GLOBAL=home + "/gitconfig", GIT_CONFIG_NOSYSTEM="1",
           GCM_CREDENTIAL_STORE="none", GCM_PROVIDER="generic")
p = subprocess.Popen([binary, "get"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                     env=env, preexec_fn=child_setup)
p.stdin.write(b"protocol=https\nhost=example.com\n\n")
p.stdin.close()
screen = b""


def pump(seconds):
    global screen
    end = time.time() + seconds
    while time.time() < end:
        r, _, _ = select.select([master], [], [], 0.05)
        if r:
            try:
                screen += os.read(master, 4096)
            except OSError:
                return


pump(2.0)
if mode == "ctrlc":
    os.write(master, b"ali"); pump(1.0); os.write(master, b"\x03"); pump(1.0)
else:
    os.write(master, b"alice\r"); pump(0.5); os.write(master, b"s3cret\r"); pump(1.0)
try:
    out, err = p.communicate(timeout=10)
except subprocess.TimeoutExpired:
    p.kill()
    out, err = p.communicate()
    print("HUNG: killed after 10 s")
print("TTY:", repr(screen.decode()))
print("STDOUT:", repr(out.decode()))
print("STDERR:", repr(err.decode()))
print("EXIT:", p.returncode)
