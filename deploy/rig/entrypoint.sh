#!/bin/sh
# deploy/rig/entrypoint.sh — make the container behave like a real rig before
# sshd starts, then exec sshd (passing through the per-rig -p <port>).
#
# A host-network container doesn't get its hostname in /etc/hosts, so `sudo`
# (which colony uses to provision) fails with "unable to resolve host <name>:
# Temporary failure in name resolution". Add the mapping so sudo + the colony
# set-identity / provision path work exactly as on a physical board.
set -e
hn="$(hostname)"
if ! grep -q "[[:space:]]$hn\$" /etc/hosts 2>/dev/null; then
    echo "127.0.1.1 $hn" >> /etc/hosts
fi
exec /usr/sbin/sshd -D -e "$@"
