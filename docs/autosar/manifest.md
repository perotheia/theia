# Manifests in Adaptive AUTOSAR

## Model

Our runtime follows the **Adaptive AUTOSAR** model: C++14, service-oriented
architecture (SOA), with flexibility at development and deployment time but
**planned dynamics** at runtime — the system integrator restricts the
dynamic behavior of the system through manifests.

Typical reductions of behavioral dynamics, enforced via manifests, cover the
four classic dimensions (communication, memory, execution, timing):

- Pre-determination of the service discovery process
- Restriction of dynamic memory allocation to the **startup phase only**
- Fair scheduling policy in addition to priority-based scheduling
- Fixed allocation of processes to CPU cores
- Access to **pre-existing files** in the file system only
- Constraints on Adaptive Platform API usage by applications

These constraints are not hard-coded — they are declared in the deployment
manifest and enforced by the runtime (primarily Execution Management).

## Execution Management

Execution Management (EM) is the control plane for processes of the Adaptive
Platform itself (Foundation/Service FCs) and the Adaptive Applications (AAs).

Responsibilities:

- Entry point of the Adaptive Platform — started by the OS during boot
- Controls startup and shutdown of the Adaptive Platform
- Configures process resources (CPU time, memory, core affinity) from the
  manifest
- Optionally performs **authenticated boot**

### Startup sequence

1. OS or hypervisor initializes the machine
2. OS starts EM as the **first** Adaptive AUTOSAR process
3. EM starts the other Functional Clusters (FCs) and Adaptive Applications
   (AAs) according to the manifest, in dependency order
4. If configured, authenticated boot is performed along the way
5. EM reports each started process's state to PHM (Platform Health
   Management) for supervision

## States

Three nested levels of state are tracked by EM and consumed by State
Management:

| Level                                   | Meaning                                                                                                  | Examples                                  |
| --------------------------------------- | -------------------------------------------------------------------------------------------------------- | ----------------------------------------- |
| **Machine Functional Group (FG) State** | Group of processes (platform FCs + AAs) available in a given Machine State                               | `Startup`, `Running`, `Shutdown`          |
| **FG State**                            | Group of processes available in a Function-Group state requested by State Management                     | `Startup`, `Driving`, `Restart`, `Parking`|
| **Process / Execution State**           | State of a single process                                                                                | `Initializing`, `Running`, `Terminating`  |

Each level is just a curated set of processes (plus their lifecycle hooks)
declared in the manifest.

## Manifest — definition

> A **Manifest** is a formal specification of configuration content,
> combined with other artifacts (typically binaries containing executable
> code) to which the Manifest applies, in order to provide specific
> functionality.

A Manifest of one Functional Cluster may be split across several physical
files. The transformation from the design-time manifest (model-level) to
the deploy-time manifest (the artifact actually shipped to the target) is
called **serialization**.

```
  Design Manifest  ──serialization──▶  Deploy Manifest
  (model, ARXML)                       (artifact on target)
```

## Manifest kinds we need

To describe our runtime end-to-end we need three classes of manifest:

### 1. Execution Manifest

Per-process configuration consumed by Execution Management.

- Process lifecycle: startup options, dependencies, shutdown behavior
- Resource group binding: CPU cores, scheduling policy/priority, memory budget
- Mapping to FG states (in which states the process should run)
- Optional authenticated-boot data

### 2. Service Manifest

Per-service SOA configuration consumed by Communication Management.

- Service interface, version, instance IDs
- Binding (SOME/IP, DDS, IPC) and endpoints
- Service discovery configuration (or its static pre-determination)
- Event/method/field QoS and access constraints

### 3. Machine Manifest

Per-machine (ECU / VM) configuration — the platform-wide settings.

- Machine States and their FG composition
- Network interfaces, OS resources, time bases
- Platform-wide security and crypto configuration
- Resource groups available on the machine

Together, these three manifests define our runtime: **what processes
exist, how they talk, and on what machine they live.**
