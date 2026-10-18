#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Validates the JSON that `osie remote` writes itself (remote_error,
# remote_status, remote_list) against docs/schemas, using the stand-in ssh
# from the build (osie_fake_ssh). Every remote_error code that can be
# provoked locally is produced at least once.
#
#   python3 tests/platform/test_remote_schemas.py BUILD_DIR
import json, os, shutil, subprocess, sys, tempfile

from jsonschema import Draft202012Validator

build = sys.argv[1]
root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
osie = os.path.join(build, "osie")
fake = os.path.abspath(os.path.join(build, "osie_fake_ssh"))
schema = {n: Draft202012Validator(json.load(open(os.path.join(root, "docs", "schemas", n + ".schema.json"))))
          for n in ("remote_error", "remote_status", "remote_list")}

work = tempfile.mkdtemp(prefix="osrs", dir="/tmp")
os.chmod(work, 0o700)
env = dict(os.environ, XDG_CONFIG_HOME=work + "/cfg", XDG_RUNTIME_DIR=work, OSIE_SSH=fake)
failures = 0


def run(args, **extra):
    e = dict(env, **extra)
    p = subprocess.run([osie, "remote", *args], capture_output=True, text=True, env=e, timeout=60)
    return p.returncode, p.stdout


def check(name, kind, rc, out, want_rc, want_code=None):
    global failures
    try:
        doc = json.loads(out)
        errors = list(schema[kind].iter_errors(doc))
        code = doc.get("remote_error", {}).get("code")
    except Exception as ex:
        errors, code = [str(ex)], None
    good = not errors and rc == want_rc and (want_code is None or code == want_code)
    failures += 0 if good else 1
    print("%s  %-28s exit %d %s%s" % ("PASS" if good else "FAIL", name, rc, code or "", "" if good else " " + str(errors[:2])))


try:
    assert run(["add", "t", "alice@example.test", "--socket", "/run/osie/agent.sock", "--timeout", "3"])[0] == 0
    assert run(["add", "w", "bob@[::1]", "--port", "2222", "--identity", "/k/id", "--known-hosts", "/k/kh"])[0] == 0
    check("list", "remote_list", *run(["list", "--json"]), 0)

    reply = os.path.join(work, "reply")
    body = '{"schema_version":"1.0","status":"ready"}'
    open(reply, "w").write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n%s" % (len(body), body))
    check("status", "remote_status", *run(["status", "t", "--json"], FAKE_SSH_MODE="reply_file", FAKE_SSH_REPLY=reply), 0)

    fixtures = {
        "host_key_unknown": "No ED25519 host key is known for h and you have requested strict checking.\nHost key verification failed.\n",
        "host_key_mismatch": "@ WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED! @\nHost key verification failed.\n",
        "authentication_failed": "alice@h: Permission denied (publickey).\n",
        "connection_failed": "ssh: connect to host h port 22: Connection refused\n",
        "ssh_unavailable": "command-line line 0: Bad configuration option: sessiontype\n",
    }
    for code, err in fixtures.items():
        check(code, "remote_error", *run(["snapshot", "t", "--json"], FAKE_SSH_MODE="exit", FAKE_SSH_STDERR=err), 3, code)
    check("remote_socket_unavailable", "remote_error", *run(["snapshot", "t", "--json"], FAKE_SSH_MODE="eof"), 3,
          "remote_socket_unavailable")
    check("timeout", "remote_error", *run(["snapshot", "t", "--json", "--timeout", "1"], FAKE_SSH_MODE="hang"), 3, "timeout")
    open(reply, "w").write("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n")
    check("protocol_error", "remote_error", *run(["snapshot", "t", "--json"], FAKE_SSH_MODE="reply_file", FAKE_SSH_REPLY=reply),
          3, "protocol_error")
    check("ssh_unavailable (missing)", "remote_error", *run(["snapshot", "t", "--json"], OSIE_SSH="/nonexistent/ssh"), 3,
          "ssh_unavailable")
    check("invalid_profile", "remote_error", *run(["snapshot", "nosuch", "--json"]), 2, "invalid_profile")
    # SSH stderr with terminal escapes stays printable ASCII.
    check("hostile stderr", "remote_error",
          *run(["snapshot", "t", "--json"], FAKE_SSH_MODE="exit", FAKE_SSH_STDERR="\x1b[2J\x1b]0;x\x07boom\n"), 3)
finally:
    shutil.rmtree(work, ignore_errors=True)

print("test_remote_schemas: %s" % ("ok" if failures == 0 else "%d failed" % failures))
sys.exit(1 if failures else 0)
