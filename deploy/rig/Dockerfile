# deploy/rig/Dockerfile — a BARE RIG container colony provisions over SSH.
#
# This replaces the old self-bootstrapping (puppet + run-supervisor.sh) image.
# The container is treated EXACTLY like a real board: it boots empty, runs an
# sshd with colony-api's pubkey in root's authorized_keys, and waits. An
# operator drives `colony provision` + `colony orchestrate` (ansible over SSH)
# from the Ground Station UI, which lays down the runtime (from S3) + the app
# overlay (Mender) — same path as a physical rig. Nothing Theia-specific is
# baked here; colony's ansible installs it all.
#
# Build (from deploy/):
#   docker build -f rig/Dockerfile -t theia-rig:latest .
#
# The two rig containers (central, compute) are IDENTICAL images — their role is
# assigned at provision time (which manifest slice colony pushes), not baked in.

FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
ENV LANG=C.UTF-8 LC_ALL=C.UTF-8

# Rig runtime prerequisites — what colony's ansible target needs + what the
# packaged FCs link at runtime:
#   openssh-server  — colony connects over SSH (the provisioning transport).
#   python3         — ansible's managed-node interpreter.
#   sudo            — colony runs privileged tasks (setcap, systemd, tipc).
#   curl, ca-certs, awscli — pull the runtime/app artifacts from the S3 planes.
#   dpkg, libcap2-bin — install the per-machine bundle + setcap the binaries.
#   iproute2        — `tipc` / `ip` (tipc-bearer.yml configures the TIPC link).
#   the lib* set    — per's etcd client + the grpc/protobuf runtime the FCs link
#                     (kept in lockstep with Dockerfile.base).
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        openssh-server \
        python3 \
        sudo \
        curl \
        ca-certificates \
        awscli \
        dpkg \
        libcap2-bin \
        iproute2 \
        libcpprest2.10 \
        libssl3 \
        libgrpc++1 \
        libgrpc10 \
        libprotobuf23 \
        libboost-system1.74.0 \
        libboost-random1.74.0 \
        libnftables1 \
        gnupg \
        dbus \
        && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

# ── Mender 4.x client (mender-update + mender-auth) — the ONE persistent OTA
#    agent, the "Mender precedes Theia" model. The container checks into the
#    dalek Mender server, so a preauthorized device auto-accepts + receives
#    Distribution app deployments. Mirrors colony's install-mender.yml +
#    ground-station/mender/server/enroll-rig.sh (the real-rig path). ───────────
RUN curl -fsSL https://downloads.mender.io/repos/debian/gpg \
        -o /etc/apt/trusted.gpg.d/mender.asc && \
    echo "deb [arch=amd64] https://downloads.mender.io/repos/debian ubuntu/jammy/stable main" \
        > /etc/apt/sources.list.d/mender.list && \
    apt-get update && \
    apt-get install -y --no-install-recommends mender-update mender-auth && \
    apt-get clean && rm -rf /var/lib/apt/lists/*

# Mender server config + CA. The cert CN is docker.mender.io (not the IP), so the
# entrypoint maps docker.mender.io → the server IP in /etc/hosts. ServerURL points
# at it; ServerCertificate trusts the dalek server CA.
COPY rig/mender.conf /etc/mender/mender.conf
COPY rig/mender-server-ca.crt /etc/mender/server.crt
# The PUBLIC artifact-verify key (mender.conf's ArtifactVerifyKey). mender-update
# REFUSES any .mender not signed by the matching private key — so an unsigned or
# forged SWP is rejected at the client, upstream of the theia-swp module. The
# private key stays with the release operator (deploy/signing/, gitignored).
COPY rig/artifact-verify-key.pem /etc/mender/artifact-verify-key.pem
RUN cp /etc/mender/server.crt /usr/local/share/ca-certificates/mender-server.crt && \
    update-ca-certificates >/dev/null 2>&1 || true

# Device identity + inventory. The identity script reads /etc/mender/device-id (a
# per-container override the entrypoint seeds from the hostname) — host-net
# containers all see the host's NICs, so the MAC is identical; the board name
# keeps central/compute DISTINCT (the enroll-rig.sh device-id mechanism). The
# UUID written by colony set-identity also lands here as device-id.
RUN install -d -m0755 /var/lib/mender /etc/mender /usr/share/mender/identity \
        /usr/share/mender/inventory && \
    printf 'device_type=theia-rig\n' > /var/lib/mender/device_type && \
    printf 'artifact_name=unprovisioned\n' > /etc/mender/artifact_info && \
    printf '#!/bin/sh\ndid="$(cat /etc/mender/device-id 2>/dev/null)"\n[ -n "$did" ] || did="$(cat /sys/class/net/*/address 2>/dev/null | head -1)"\necho "device_id=$did"\n' \
        > /usr/share/mender/identity/mender-device-identity && \
    chmod 0755 /usr/share/mender/identity/mender-device-identity

# The Mender inventory script — reports base_version (the real installed
# theia-runtime deb version) so the GS gets device truth, not an optimistic tag.
# Shipped as a real file (brace-heavy dpkg format doesn't survive an inline
# Dockerfile printf).
COPY rig/mender-inventory-theia /usr/share/mender/inventory/mender-inventory-theia
RUN chmod 0755 /usr/share/mender/inventory/mender-inventory-theia

# sshd needs a runtime dir + host keys. Allow root login by key only (no
# password); colony connects as root with its key (matches a real rig where
# colony provisions privileged). PermitRootLogin prohibit-password = key-only.
RUN mkdir -p /run/sshd && \
    ssh-keygen -A && \
    sed -i 's/^#\?PermitRootLogin.*/PermitRootLogin prohibit-password/' /etc/ssh/sshd_config && \
    sed -i 's/^#\?PasswordAuthentication.*/PasswordAuthentication no/' /etc/ssh/sshd_config

# The provisioning user. colony connects as `axadmin` (COLONY_SSH_USER default,
# matching the real rigs) with passwordless sudo — so the container provisions
# through the SAME path as a physical board. colony-api's pubkey (== GET /pubkey,
# the key it writes onto real preauthorized rigs) goes into BOTH axadmin's and
# root's authorized_keys, making the container colony-reachable from first boot.
RUN useradd -m -s /bin/bash axadmin && \
    echo "axadmin ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/axadmin && \
    chmod 440 /etc/sudoers.d/axadmin && \
    mkdir -p /home/axadmin/.ssh /root/.ssh
COPY rig/colony_authorized_keys /home/axadmin/.ssh/authorized_keys
COPY rig/colony_authorized_keys /root/.ssh/authorized_keys
RUN chown -R axadmin:axadmin /home/axadmin/.ssh && \
    chmod 700 /home/axadmin/.ssh /root/.ssh && \
    chmod 600 /home/axadmin/.ssh/authorized_keys /root/.ssh/authorized_keys

# Theia layout the provisioning fills (colony stages release + config here).
RUN mkdir -p /opt/theia/bin /opt/theia/config /var/log/theia/tombstones

# The SSH port is overridden per-container at run (central 2201, compute 2202)
# via the command in docker-compose; default 22 if run standalone.
EXPOSE 22

# The entrypoint registers the container's hostname in /etc/hosts (so colony's
# sudo provisioning resolves it) then exec's sshd. -p (from compose) passes a
# per-rig port — host net → both rigs share the port space, can't both bind :22.
COPY rig/entrypoint.sh /usr/local/bin/rig-entrypoint.sh
RUN chmod +x /usr/local/bin/rig-entrypoint.sh
ENTRYPOINT ["/usr/local/bin/rig-entrypoint.sh"]
CMD ["-p", "22"]
