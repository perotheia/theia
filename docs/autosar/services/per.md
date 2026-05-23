The **AUTOSAR Persistency (`ara::per`)** functional cluster provides a standardized C++ interface for storing and retrieving data across power cycles. Unlike a typical desktop file system, `ara::per` is designed for high-reliability automotive environments where power loss can occur at any moment.

### 1. Architectural Role

Persistency abstracts the underlying storage hardware (e.g., eMMC, Flash). It ensures that data remains consistent even during sudden resets and manages the "Software Cluster" lifecycle, ensuring that when an application is updated via **UCM**, its data is either migrated, preserved, or cleared according to the manifest.

---

### 2. Primary Storage Types

The specification divides storage into two distinct functional paradigms:

#### A. Key-Value Storage (KVS)

Used for small, structured parameters (calibration values, user settings, trip data).

* **Access:** Data is accessed via a unique string key.
* **Values:** Supports all `ara::core` basic types (`int`, `float`, `String`), as well as complex types via **AUTOSAR Serialization**.
* **Logic:** Similar to a dictionary or a lightweight NoSQL database.

#### B. File Storage (FS)

Used for large, unstructured data (logs, map data, multimedia files).

* **Access:** Provides a C++ interface that mirrors the behavior of `std::fstream`.
* **Logic:** Applications open a "virtual file" within a protected directory.

---

### 3. Database Schema & Data Modeling

The AUTOSAR Adaptive Platform does not use a "SQL Schema" in the traditional sense. Instead, the "schema" is defined in the **ARXML (AUTOSAR XML) Manifest**.

When an application is designed, the architect defines a **Persistency Key Value Database** or a **Persistency File Proxy**. The platform uses this manifest to pre-allocate storage.

#### Extracted Data Model (From Manifest)

While not a SQL table, the internal logic of a KVS "schema" is structured as follows in the deployment manifest:

| Element | Description | Requirement |
| --- | --- | --- |
| **`PersistencyKeyValuePair`** | The "Column." Defines the Key name and the Type (e.g., `uint16`). | Mandatory |
| **`UpdateStrategy`** | Defines what happens during a UCM update: `kKeep`, `kOverwrite`, or `kDelete`. | Mandatory |
| **`RedundancyHandling`** | Configuration for data safety: `kNone`, `kCRC`, or `kM-out-of-N` (voting). | Mandatory |
| **`DataIntegrity`** | Specifies if a checksum or hash is appended to verify data on read. | Mandatory |
| **`InitialValue`** | The default value assigned if the key is read before it is ever written. | Optional |

---

### 4. Technical Functions & C++ Usage

The API is found in the `ara::per` namespace. It is designed to be **fail-safe** and **atomic**.

* **Atomic Writes:** KVS operations are typically atomic. If the power fails mid-write, the system reverts to the last known good state.
* **`ara::per::SharedHandle`:** Handles to storage are managed via shared pointers to ensure memory safety.
* **`ara::core::Result`:** All operations return a `Result` to handle storage-specific errors without exceptions.

#### Basic Code Example

```cpp
auto kvsHandle = ara::per::OpenKeyValueStorage("MySettings");
if (kvsHandle) {
    // Write a value
    kvsHandle.Value()->GetValue<uint32_t>("Odometer").Set(12500);
    // Persist to physical media
    kvsHandle.Value()->SyncToStorage();
}

```

---

### 5. Interaction with Other Clusters

Persistency is a silent partner to most operations:

1. **UCM (Update):** When UCM updates an app, it checks the `UpdateStrategy` in the manifest. If it’s `kKeep`, UCM ensures the new version of the app can still access the old version's data.
2. **IAM (Identity & Access):** IAM ensures that "App A" cannot read the private data stored by "App B" unless explicitly shared in the manifest.
3. **State Management:** SM may trigger a `SyncToStorage` command during the "ShuttingDown" state to ensure all cached data is flushed to Flash before power-off.

---

### 6. Error Codes (`PerErrorDomain`)

* **`kIntegrityCorrupted`:** The data was read, but the checksum/CRC failed (hardware failure or bit-flip).
* **`kPhysicalStorageError`:** The underlying Flash memory is full or has reached its end-of-life (wear-out).
* **`kValidationFailed`:** The data type in the manifest doesn't match the type the application is trying to read.
