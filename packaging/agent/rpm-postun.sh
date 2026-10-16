if [ -d /run/systemd/system ]; then systemctl daemon-reload >/dev/null 2>&1 || :; fi
# On upgrade, restart the agent only if it was running.
if [ "$1" -ge 1 ] && [ -d /run/systemd/system ]; then systemctl try-restart osie-agent.service >/dev/null 2>&1 || :; fi
