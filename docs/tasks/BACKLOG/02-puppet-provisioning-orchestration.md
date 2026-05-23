# Puppet two-phase deploy scripts (provisioning + orchestration)

`Machine.os_packages` + `Machine.opkg_artifacts` already land in
`dist/manifest/<machine>/machine.yaml`. Per-app `.ipk`s land in
`dist/manifest/<machine>/application.yaml`. Both files are the
source of truth — Puppet just reads them.

## Two phases

The user spelled out the lifecycle:

| phase | scope | trigger |
|---|---|---|
| **provisioning** | OS packages + opkg artifacts (supervisor, gateway, etcd) + systemd units. Full update touching gateway/supervisor goes here. Includes **services/db offline migration** if a state-schema delta is detected. | first install; reprovisioning for a major release |
| **orchestration** | Application `.ipk` replacement under `/opt/theia/apps/` + systemd reload. **No** supervisor/gateway/etcd restart. | day-to-day app push |

Partial update is orchestration; full update is reprovisioning.

## Deliverable (stub level, for reference)

```
deploy/puppet/
├── provisioning.pp        # phase 1: machine.yaml driven
├── orchestration.pp       # phase 2: application.yaml driven
└── README.md              # explains the two-phase model
```

### provisioning.pp (sketch)

```puppet
# Stage 1: OS packages (apt or opkg).
$machine = load_yaml('/etc/theia/manifest/machine.yaml')['machine']

$machine['os_packages'].each |$pkg| {
  package { $pkg['name']:
    ensure   => $pkg['version'] ? { '' => 'latest', default => $pkg['version'] },
    provider => $pkg['source'],     # apt | opkg
  }
}

# Stage 2: Theia opkg artifacts under /opt/theia/.
$machine['opkg_artifacts'].each |$art| {
  package { "theia-${art['name']}":
    ensure  => 'installed',
    source  => "/var/theia/artifacts/${art['name']}.ipk",
    require => Package['libsystemd0'],
  }
  file { $art['systemd_unit']:
    ensure  => 'file',
    source  => "puppet:///modules/theia/${art['name']}.service",
    require => Package["theia-${art['name']}"],
    notify  => Service["theia-${art['name']}"],
  }
  service { "theia-${art['name']}":
    ensure => 'running',
    enable => $art['enable_on_boot'],
  }
}

# Stage 3: services/db migration if a schema-version delta exists.
# (Defer until services/db lands; for now, just a comment marker.)
# exec { 'theia-db-migrate':
#   command => '/opt/theia/services-db/bin/migrate',
#   onlyif  => '/opt/theia/services-db/bin/migrate --check',
# }
```

### orchestration.pp (sketch)

```puppet
# Replace application .ipks; do NOT touch the supervisor/gateway.
$apps = load_yaml('/etc/theia/manifest/application.yaml')['applications']

$apps.each |$app| {
  $app['components'].each |$cmp| {
    package { "theia-app-${cmp['name']}":
      ensure  => 'latest',
      source  => "/var/theia/apps/${cmp['name']}.ipk",
      notify  => Service["theia-app-${cmp['name']}"],
    }
    service { "theia-app-${cmp['name']}":
      ensure  => 'running',
      enable  => true,
    }
  }
}

# Tell the supervisor to reload its child list (no restart).
exec { 'theia-supervisor-reload':
  command     => '/opt/theia/supervisor/bin/reload',
  refreshonly => true,
  subscribe   => Package['theia-app-*'],
}
```

## Definition of done

- Both `.pp` files exist with the structure above (stubs are fine —
  no working Puppet master required this session).
- `README.md` explains:
  - "provisioning = first install OR full update including supervisor/
    gateway; orchestration = app-only redeploy".
  - "full update implies offline DB migration; provisioning step runs
    `services/db --check` before promoting".
  - how to invoke each phase (`puppet apply deploy/puppet/{provisioning,orchestration}.pp`).
- Optional: a docker-compose smoke script that simulates a fresh
  provisioning run on a clean Ubuntu image.

## Not in scope (defer)

- A real Puppet master / agent setup. We're not running Puppet in
  prod yet; this is the reference shape so when we do, the manifests
  match what `dist/manifest/<machine>/` already emits.
- The `services/db` migration step. Stub the `exec` block but leave
  it commented out until services/db itself exists.
