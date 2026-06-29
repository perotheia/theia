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
        && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

# sshd needs a runtime dir + host keys. Allow root login by key only (no
# password); colony connects as root with its key (matches a real rig where
# colony provisions privileged). PermitRootLogin prohibit-password = key-only.
RUN mkdir -p /run/sshd && \
    ssh-keygen -A && \
    sed -i 's/^#\?PermitRootLogin.*/PermitRootLogin prohibit-password/' /etc/ssh/sshd_config && \
    sed -i 's/^#\?PasswordAuthentication.*/PasswordAuthentication no/' /etc/ssh/sshd_config

# colony-api's pubkey → root's authorized_keys (the provisioning key). This is
# the same key colony serves at GET /pubkey and writes onto real preauthorized
# rigs; baking it here makes the container colony-reachable from first boot.
COPY rig/colony_authorized_keys /root/.ssh/authorized_keys
RUN chmod 700 /root/.ssh && chmod 600 /root/.ssh/authorized_keys

# Theia layout the provisioning fills (colony stages release + config here).
RUN mkdir -p /opt/theia/bin /opt/theia/config /var/log/theia/tombstones

# The SSH port is overridden per-container at run (central 2201, compute 2202)
# via the command in docker-compose; default 22 if run standalone.
EXPOSE 22

# Run sshd in the foreground. -p lets compose pass a per-rig port (host net →
# both rigs share the host's port space, so they must not both bind :22).
ENTRYPOINT ["/usr/sbin/sshd", "-D", "-e"]
CMD ["-p", "22"]
