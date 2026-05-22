# deploy/central/Dockerfile — central host image.
#
# Thin layer over theia-base:latest. Sets HOSTNAME=central so the
# entrypoint picks /etc/puppet/manifests/central.pp.
#
# Build (from workspace root):
#   docker build -f deploy/Dockerfile.base -t theia-base:latest deploy/
#   docker build -f deploy/central/Dockerfile -t theia-central:latest deploy/

FROM theia-base:latest

# Hostname dispatcher for run-supervisor.sh.
ENV HOSTNAME=central

# central exposes the services/com gRPC bridge on 7700 so the GUI
# (or the compute container's downstream caller) can reach it.
EXPOSE 7700

# The .ipk, executor.yaml, machines.yaml, supervisor binary, and
# Puppet manifests are all bind-mounted via docker-compose at
# runtime — no COPYs here. That lets `bazel build` and edit-rebuild
# cycles flow into the running container without docker rebuild.
