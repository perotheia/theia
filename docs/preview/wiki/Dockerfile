# Wiki image — MkDocs Material with the mermaid2 plugin pre-installed.
#
# We pin the base image and the plugin version so `docker compose up --build`
# is reproducible regardless of when it runs. Upgrade by bumping these
# explicitly, not by latest-tag drift.
#
# squidfunk/mkdocs-material already includes pymdownx (which provides
# superfences) so the only extra we need on top is mkdocs-mermaid2-plugin.

FROM squidfunk/mkdocs-material:9.5.49

RUN pip install --no-cache-dir mkdocs-mermaid2-plugin==1.2.1

# Compose mounts the repo root at /docs and runs `mkdocs serve`. We
# expose the default port so `127.0.0.1:8000` on the host hits it.
EXPOSE 8000
