# $1 == 0: final removal (not an upgrade).
if [ "$1" -eq 0 ] && [ -d /run/systemd/system ]; then
  systemctl stop osie-agent.service >/dev/null 2>&1 || :
  systemctl disable osie-agent.service >/dev/null 2>&1 || :
fi
