# Adaptive AUTOSAR

Source: <https://nvdungx.github.io/Adaptive-AUTOSAR/>

## What is Adaptive AUTOSAR

A standardized middleware/architecture definition of automotive software for future high performance computing ECUs. It does not define a concrete design/implementation like Classic AUTOSAR but leaves a certain degree of freedom for the development.

```
Internal interfaces between the building blocks of the AUTOSAR Adaptive Platform shall not be standardized.
```

Adaptive AUTOSAR Platform addresses the following software building block functions over a POSIX base Operating System:

- **Runtime**: Execution Management, State Management, Log and Trace, Core, OS Interface.
- **Communication**: Communication Management, Time Synchronization, Raw Data Stream, Network Management.
- **Storage**: Persistency.
- **Safety**: Platform Health Management.
- **Security**: Cryptography, Intrusion Detection System Manager, Firewall.
- **Diagnostic**: Diagnostic Management.
- **Configuration**: Update and Configuration Management, Vehicle Update and Configuration Management, Registry.

![Adaptive AUTOSAR blocks](https://nvdungx.github.io/assets/img/blogs/2024_02_27/AdaptiveAUTOSAR_blocks.png)

```
Adaptive Application [AA]: user-level component that implements most of the project/ECU specific functionalities, and runs on top of the environment (ara), which is provided by Platform components (FC).
Functional Clusters [FC]: logical group of functionality, sub-component of above building blocks (2nd level of abstraction). Could be implemented as a library or processes (Library-based or Service based), which belong to either Adaptive Platform Foundation, Adaptive Platform Services, Standard Application/Interfaces or Vehicle Services.
Adaptive Platform [AP] Foundation: group of Functional Clusters that provide fundamental functionalities.
Adaptive Platform Service: group of FCs that provide standardized services.
```

## Why Adaptive AUTOSAR Exists

Three technological drivers necessitated Adaptive AUTOSAR's development:

1. **Increased computing power**: modern embedded processors offer higher core counts and computational capacity.
2. **Automotive Ethernet adoption**: provides larger communication bandwidth than traditional automotive networks.
3. **Complex functionalities**: vehicles demand dynamic, flexible architectures.

Adaptive AUTOSAR addresses V2X communication, computing power requirements, complex functions, scalability, and upgradeability — complementing rather than replacing Classic AUTOSAR for safety-critical actuator functions.

### Technology Stack

The platform employs the C++14 standard and service-oriented architecture, enabling deployment flexibility while allowing system integrators to restrict dynamic behavior through the Execution Manifest. Behavioral constraints include:

- Pre-determined service discovery.
- Dynamic memory allocation restricted to startup.
- Fair scheduling supplementing priority-based approaches.
- Fixed process-to-CPU-core allocation.
- Pre-existing file-system access only.
- API usage constraints for applications.
- Authenticated code execution requirement.

> Adaptive AUTOSAR = High performance/multi-core SoC + Automotive Ethernet + POSIX OS (PSE51) + Service-Oriented-Architecture.

## Functional Cluster Reference

| Functional Cluster | Shortname | Log&Trace Context ID |
|---|---|---|
| Adaptive Platform Core | core | #COR |
| Communication Management | com | #COM |
| Cryptography | crypto | #CRY |
| Diagnostics | diag | #DIA |
| Execution Management | exec | #EXE |
| Firewall | fw | #FWX |
| Intrusion Detection System Manager | idsm | #IDS |
| Log and Trace | log | #LOG |
| Network Management | nm | #NMX |
| Operating System Interface | n/a | #OSI |
| Persistency | per | #PER |
| Platform Health Management | phm | #PHM |
| Raw Data Stream | rds | #RDS |
| State Management | sm | #SMX |
| Time Synchronization | tsync | #TSY |
| Update and Configuration Management | ucm | #UCM |
| Vehicle Update and Configuration Management | vucm | #VUM |
| Safe Hardware Accelerator | shwa | #SHA |

---

## Part I: Overview of Functional Clusters

### Runtime Components

#### Execution Management

Responsible for controlling Processes of the AUTOSAR Adaptive Platform and Adaptive Applications (i.e. starts, configures, and stops Processes).

Key responsibilities:

- Serves as entry point, initiated during system boot.
- Controls startup/shutdown of the Adaptive Platform.
- Configures process resources (CPU time, memory) per Manifest.
- Optionally supports authenticated boot.

The startup sequence begins when the OS or hypervisor initializes, then launches Execution Management as the first Adaptive AUTOSAR process, which subsequently starts other FCs and AAs according to the Manifest.

![Execution Management](https://nvdungx.github.io/assets/img/blogs/2024_02_27/ExecutionManagement.png)

**State Management Concepts**:

- **Machine Functional Group State** groups processes available at specific Machine States (Startup, Running, Shutdown).
- **FG State** groups processes for requested states (Startup, Driving, Restart, Parking, etc.).
- **Process/Execution State** reflects individual process conditions (Initializing, Running, Terminating).

![Execution Management concept — Machine State](https://nvdungx.github.io/assets/img/blogs/2024_02_27/ExecutionManagement_MachineState.png)

![Execution Management concept — State Concept](https://nvdungx.github.io/assets/img/blogs/2024_02_27/ExecutionManagement_StateConcept.png)

![Execution Management concept — State Request](https://nvdungx.github.io/assets/img/blogs/2024_02_27/ExecutionManagement_StateRequest.png)

#### State Management

Determines the desired target state of the Adaptive Applications based on various application-specific inputs. The state control action is delegated to Execution Management (i.e. state = set of active Function Group States). State management operation remains application-specific.

![State Management](https://nvdungx.github.io/assets/img/blogs/2024_02_27/StateManagement.png)

#### Log and Trace

Provides functionality to build and log messages of different severity to different sinks (e.g. network, a serial bus, the console, and non-volatile storage).

Features include:

- Multiple severity-level log streams.
- Configurable output format and sinks.

![Log and Trace](https://nvdungx.github.io/assets/img/blogs/2024_02_27/Log&Trace.png)

#### Adaptive Platform Core

This foundational element provides initialization and de-initialization of the AUTOSAR Runtime for Adaptive Applications and process termination. It:

- Defines common data types used across multiple FCs.
- Supplies global initialization and shutdown functions.
- Handles general error management and abnormal process termination.

#### Operating System Interface

Provides functionality for implementing multi-threaded real-time embedded applications and corresponds to the POSIX PSE51 profile.

![OS Interface](https://nvdungx.github.io/assets/img/blogs/2024_02_27/OS%20Interface.png)

### Communication Components

#### Communication Management

Responsible for all levels of service-oriented communication between applications in a distributed real-time embedded environment — intra-process, inter-process, and inter-machine communication.

Capabilities include:

- Message acceptance/rejection with optional authenticator verification.
- Freshness value retrieval for transmitted/received messages.

![Communication Management interfaces](https://nvdungx.github.io/assets/img/blogs/2024_02_27/CommunicationManagement.png)

#### Raw Data Stream

Responsible for raw communication between applications in a distributed real-time embedded environment, providing client/server interfaces for reading/writing raw binary data streams over network connections.

![Raw Data Stream interfaces](https://nvdungx.github.io/assets/img/blogs/2024_02_27/RawDataStream.png)

#### Network Management

Enables applications to request and query the network states for logical network handles, including:

- Obtaining current or requested network states (PNC/VLAN/Physical Network active/inactive status).
- Setting new requested network states.

![Network Management interfaces](https://nvdungx.github.io/assets/img/blogs/2024_02_27/NetworkManagement.png)

#### Time Synchronization

Provides synchronized time information in distributed applications, offering interfaces to get/set current time points, rate deviation, current status, and received user data.

![Time Synchronization interfaces](https://nvdungx.github.io/assets/img/blogs/2024_02_27/TimeSynchronization.png)

### Storage Component

#### Persistency

Stores and retrieves information to/from non-volatile storage of a Machine. Key characteristics:

- Persistent data remains private to individual processes, persisting across boot and ignition cycles.
- Supports concurrent multi-threaded access within the same process.
- Provides integrity and confidentiality via EDC, ECC, and encryption.
- Supports both file storage and key-value pair storage.

![Persistency interfaces](https://nvdungx.github.io/assets/img/blogs/2024_02_27/Persistency.png)

### Security Components

#### Cryptography

Provides various cryptographic routines to ensure confidentiality and integrity of data (e.g. via hashes), and auxiliary functions such as key management and random number generation.

Offerings include:

- Encapsulation of security-sensitive operations.
- Crypto, key storage, and x509 certificate processing interfaces.

![Cryptography interfaces](https://nvdungx.github.io/assets/img/blogs/2024_02_27/Cryptography.png)

#### Intrusion Detection System Manager

Provides functionality to report security events.

![Intrusion System Detection Manager interfaces](https://nvdungx.github.io/assets/img/blogs/2024_02_27/IntrusionSystemDetectionManager.png)

#### Firewall

Responsible for filtering network traffic based on firewall rules to protect the system from malicious messages.

Functions include:

- Parsing firewall rules from the Manifest and configuring underlying firewall engines.
- Handling different modes (driving, parking, diagnostic sessions) through rule enabling/disabling.
- Reporting security events to the Intrusion Detection System Manager.

![Firewall interfaces](https://nvdungx.github.io/assets/img/blogs/2024_02_27/Firewall.png)

### Safety Component

#### Platform Health Manager

Performs safety-critical process execution monitoring and manages watchdog operation.

Responsibilities include:

- Supervision of processes (aliveness, logical, and deadline verification) in safety-critical setups.
- Reporting failures to State Management.
- Watchdog control.

![Platform Health Manager interfaces](https://nvdungx.github.io/assets/img/blogs/2024_02_27/PlatformHealthManager.png)

### Configuration Components

#### Update and Configuration Management

Responsible for updating, installing, removing and keeping a record of the software on an AUTOSAR Adaptive Platform in a safe and secure way.

![Update and Configuration Management interfaces](https://nvdungx.github.io/assets/img/blogs/2024_02_27/UpdateandConfigurationManagement.png)

#### Vehicle Update and Configuration Management

Responsible for updating, installing, removing and keeping a record of the software installed in an entire vehicle, enabling flexible software updates through over-the-air (OTA) mechanisms.

![Vehicle Update and Configuration Management interfaces](https://nvdungx.github.io/assets/img/blogs/2024_02_27/VehicleUpdateandConfigurationManagement.png)

#### Registry

Provides access to information stored in the Manifest (json file structure), intended to be used by Platform FCs only.

![Registry interfaces](https://nvdungx.github.io/assets/img/blogs/2024_02_27/Registry.png)

### Diagnostic Component

#### Diagnostic Management

Responsible for handling diagnostic functionalities, including:

- Managing diagnostic events from individual processes.
- Providing external Diagnostic Clients access to diagnostic data via standardized protocols (ISO 14229, 13400).

![Diagnostic Management interfaces](https://nvdungx.github.io/assets/img/blogs/2024_02_27/DiagnosticManagement.png)

---

## Part II: Development Methodology

The Adaptive AUTOSAR approach involves three stakeholder groups:

- **OEM**: defines vehicle function architecture.
- **Tier 1**: develops and integrates ECUs with Classic Platform, Adaptive Platform, mixed approaches, or non-AUTOSAR solutions.
- **Other suppliers**: develop platform or component-specific software.

The methodology follows a top-down approach. For deployment purposes, Adaptive AUTOSAR differs slightly from Classic AUTOSAR, though development activities for Adaptive Application development and Machine configuration/integration parallel Classic AUTOSAR's Application SWCs and BSW configuration/integration.

![Development Methodology](https://nvdungx.github.io/assets/img/blogs/2024_02_27/DevelopmentMethodology.svg)

### Output Artifacts

Development activities generate several artifact types:

- **Analysis and Design Documentation**: standard software development documentation.
- **ARXML files**: primary exchange medium describing software, network topology, communication, and configuration.
- **Implementation Artifacts**: C++ source code and resulting application/platform-level binaries deployed as POSIX-runtime processes.
- **Configuration Artifacts**: design manifest (ARXML), pre-compile generated code, post-build JSON, and runtime environment configuration files.

![Output artifacts](https://nvdungx.github.io/assets/img/blogs/2024_02_27/Output-Artifacts.svg)

---

## Part III: Adaptive AUTOSAR Design Concepts

### Foundational Understanding

Before designing or implementing the Adaptive Platform, comprehending these concepts is crucial: POSIX OS system calls, kernel IPC, shared memory mechanisms, filesystems, processes, executables, threading, and concurrency.

### Design Approach: Service-Oriented Architecture

The overall system design embraces service-oriented architecture through three principles:

1. Each AA or AP FC runs within its own process, providing services via interfaces.
2. AP FCs deliver services to AAs and other FCs through library interfaces (**ARA** — the collection of FC public C++ APIs exposed to AAs via headers/libraries).
3. AA communication shall not occur through direct IPC calls; instead, all interactions occur via the Communication Management FC, covering both intra- and inter-machine communication.

**ARA**: comprises application interfaces provided by Adaptive Platform FCs, typically implemented through standard libraries like the STL.

![Adaptive AUTOSAR design/implementation](https://nvdungx.github.io/assets/img/blogs/2024_02_27/AdaptiveAUTOSAR_design.png)

![Sample Implementation Source Structure](https://nvdungx.github.io/assets/img/blogs/2024_02_27/AdaptiveAUTOSAR_Design_Physical_Files.png)

### Manifest Specifications

The Manifest is a formal specification of configuration content, combining with executable binary files to provide specific functionalities. Manifest content may distribute across multiple physical files.

- **Design Phase**: Manifest data resides in ARXML files.
- **Deployment Phase**: Manifest data takes various forms:
  - Linux configuration files (.conf) loaded by kernel modules.
  - JSON files loaded by Adaptive AUTOSAR FCs during runtime.
  - Header/source files (.h, .cpp) included during build processes.

The transformation from design Manifest to deployment Manifest is called **SERIALIZATION**.

![Manifest serialization](https://nvdungx.github.io/assets/img/blogs/2024_02_27/manifest_serialization.svg)

### Manifest Categories

- **Application Manifest** — describes deployment aspects including:
  - Software component and composition design.
  - Executable description.
  - Process design.
  - Startup configuration and service-oriented communication endpoint configuration.
- **Machine Manifest** — per-machine runtime manifest describing deployment content for underlying machine configuration:
  - Network interface configuration.
  - Available hardware resources and states.
  - Implementation through machine-specific configuration file groups.
- **Service Manifest** — per-process runtime manifest describing service-oriented communication binding:
  - Adaptive platform data types.
  - Service interface definitions.
- **Execution Manifest** — per-process runtime manifest specifying deployment information:
  - Executable binding to processes with timing, priority, and resource attributes.
  - Startup configuration and inter-process dependencies and state relationships.

![Manifest Categories](https://nvdungx.github.io/assets/img/blogs/2024_02_27/configuration_manifest.svg)

### Software Deployment Process

After implementation completion, the system produces these artifacts:

- AA and FC executables.
- Corresponding shared libraries and external/public headers.
- Corresponding deployment manifests (.json, .conf).

Integration and deployment encompass all activities necessary to execute designated software on a specific machine, considering its hardware, connected networks, operating system, and platform-level Adaptive Software.

The deployment workflow involves:

1. Gathering executable binaries and libraries.
2. Integrating manifest configurations.
3. Deploying process definitions to the target POSIX runtime environment.
4. Establishing service communication endpoints.
5. Configuring runtime parameters based on machine-specific requirements.

![Adaptive AUTOSAR integration and deployment](https://nvdungx.github.io/assets/img/blogs/2024_02_27/integrate_deploy_adaptive.svg)
