# The group whose members may use the local diagnostic API (api_group = osie).
# No members until the administrator adds some; kept on removal.
getent group osie >/dev/null 2>&1 || groupadd --system osie >/dev/null 2>&1 || :
if [ -d /run/systemd/system ]; then systemctl daemon-reload >/dev/null 2>&1 || :; fi
