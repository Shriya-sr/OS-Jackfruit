# 🚀 Supervised Multi-Container Runtime

> A lightweight container runtime with kernel-level monitoring, scheduling control, and IPC-driven supervision.


### 👥 Team Information
* **Team Member 1:** [Shriya S R] - [PES1UG24CS449]
* **Team Member 2:** [Ahana S S] - [PES1UG24CS910] 

---

### ⚙️ Build, Load, and Run Instructions
*Note: These instructions assume a fresh Ubuntu 22.04/24.04 VM environment.*

### 🛠️ Build the Project
```bash
make
```

### 🧠 Load the Kernel Module
```bash
sudo insmod monitor.ko
ls -l /dev/container_monitor 
```

### 📦Prepare the Filesystems & Workloads

```bash
# Create per-container writable rootfs copies (assuming rootfs-base is one level up)

cp -a ../rootfs-base ../rootfs-alpha
cp -a ../rootfs-base ../rootfs-beta

# Copy test workloads into the isolated containers
cp memory_hog ../rootfs-alpha/
cp cpu_hog ../rootfs-beta/

```

### 🧵 Start the Supervisor (Pinned to a Single Core)
Note: We use taskset -c 0 to pin the supervisor to a single CPU core. This ensures that child containers fight for the same physical processor, allowing the Completely Fair Scheduler (CFS) priority experiment to work accurately in a multi-core VM. 
```bash
sudo taskset -c 0 ./engine supervisor ../rootfs-base
```


### Launch Containers and Run CLI Commands (In a second terminal)
```bash
# Start alpha with a memory pressure workload
sudo ./engine start alpha ../rootfs-alpha /memory_hog --soft-mib 48 --hard-mib 80

# Start beta with a CPU-bound workload (Lowest priority)
sudo ./engine start beta ../rootfs-beta "/cpu_hog 60" --nice 19

# List tracked containers and verify states
sudo ./engine ps
# Inspect a specific container's logs
sudo ./engine logs alpha

```

### CFS Priority Scheduling
Open a Terminal 2:
```bash
# 1. Launch the high-priority container (runs for 120 seconds)
sudo ./engine start alpha ../rootfs-alpha "/cpu_hog 120" --nice -20

# 2. Launch the low-priority container (runs for 120 seconds)
sudo ./engine start beta ../rootfs-beta "/cpu_hog 120" --nice 19

# 3. Verify they are both running
sudo ./engine ps
```

Open Terminal 3
```bash
top
```

### 🧹 Stop and Clean Up
```bash
# Stop containers safely
sudo ./engine stop alpha
sudo ./engine stop beta

# (Or press Ctrl+C in the supervisor terminal for graceful shutdown)

# Check kernel logs for memory limit enforcements
dmesg | tail -n 20

# Unload the kernel module
sudo rmmod monitor

```

## 📸 Demo with Screenshots
1. Multi-container supervision

<img width="1090" height="360" alt="image" src="https://github.com/user-attachments/assets/0641eda6-3e5b-4ab5-9045-47111cfabba6" />

Two containers (alpha and beta) running simultaneously under a single supervisor process.

2. Metadata tracking

<img width="1090" height="99" alt="image" src="https://github.com/user-attachments/assets/36e9cd22-5c9b-47bc-a54a-dccddaf85539" />

Output of the ps command showing the tracked state changing from running to killed.

3. Bounded-buffer logging

<img width="1090" height="305" alt="image" src="https://github.com/user-attachments/assets/c3389306-ff21-443b-9ab6-8b7f01eb6ea0" />

Evidence of the producer/consumer logging pipeline capturing async logs (e.g., [LOG] alpha:started).

4. CLI and IPC communication

<img width="1090" height="75" alt="image" src="https://github.com/user-attachments/assets/6da66add-d36b-4bbd-b9b5-91657c0545dc" />

A CLI command (ps or stop) being issued and successfully receiving a response from the supervisor via UNIX domain sockets.

5. Soft-limit and hard-limit warning

<img width="1090" height="281" alt="image" src="https://github.com/user-attachments/assets/62a31a4f-2c2a-4a0f-b2ab-7a01ecc8a24c" />

<img width="1090" height="82" alt="image" src="https://github.com/user-attachments/assets/dcde55fa-25e7-404c-9ea4-d35b71ce8561" />

<img width="1090" height="292" alt="image" src="https://github.com/user-attachments/assets/16eca9f0-858d-4a24-8b49-09bab3d17399" />



6. Scheduling experiment

<img width="1090" height="97" alt="image" src="https://github.com/user-attachments/assets/2230a0fd-3d3e-4335-849d-9f373995f510" />


<img width="1090" height="761" alt="image" src="https://github.com/user-attachments/assets/c366ecea-4107-4803-a73a-a250913be8bc" />


Output showing the Completely Fair Scheduler allocating 99.0% CPU to the high-priority process (NI -20) and starving the low-priority process (NI 19) down to 0.0%.

7. Clean teardown

<img width="1090" height="326" alt="image" src="https://github.com/user-attachments/assets/e869795f-3cb8-401f-873c-bac8d5e6ad35" />

Supervisor exit messages confirming all remaining containers are reaped via SIGKILL and threads are cleanly joined during shutdown.


### Engineering Analysis

Namespace Isolation: Our runtime utilizes the clone() system call with CLONE_NEWPID, CLONE_NEWNS, and CLONE_NEWUTS flags to physically isolate the container's process tree, mount points, and hostnames from the host OS. By executing chroot() into the designated rootfs directory before executing /bin/sh or a workload, the process is completely jailed in a distinct filesystem hierarchy, mirroring the core behavior of enterprise container runtimes like Docker.

Supervisor Architecture: The supervisor operates as a monolithic event loop. It maintains a linked list of container metadata. To prevent zombie processes, the supervisor implements a non-blocking zombie reaper using waitpid(..., WNOHANG). If a process terminates (naturally or via a SIGKILL from our kernel module), the supervisor safely reaps it and updates the metadata state to CONTAINER_EXITED or CONTAINER_KILLED without freezing the main listening socket.

IPC and Logging: Control commands are transmitted using UNIX Domain Sockets (AF_UNIX). The SOCK_CLOEXEC flag is heavily utilized to prevent file descriptor leaks, ensuring child containers do not inherit and indefinitely hold the server socket open. Logging utilizes a thread-safe bounded buffer powered by pthread_mutex_t and condition variables (pthread_cond_t), allowing the main thread to push metadata logs asynchronously without blocking execution, while a background consumer thread prints them to stdout.

Kernel Monitor (LKM): Because standard user-space applications cannot easily track true Resident Set Size (RSS) directly or terminate processes with absolute authority, we implemented a Loadable Kernel Module. The module uses get_mm_rss() inside a kernel timer interrupt to periodically check page allocations. If a hard limit is breached, the kernel context bypasses user-space permission checks to immediately dispatch a fatal SIGKILL to the offending PID.

Scheduling Behavior: Linux uses the Completely Fair Scheduler (CFS), which balances CPU time by tracking the vruntime (virtual runtime) of each process. Processes with lower vruntime are chosen to run next. The nice value acts as a multiplier to this metric. In our testing, we explicitly pinned our supervisor to a single core (taskset -c 0) to prevent the OS from simply assigning the containers to different physical processors.

## Design Decisions and tradeoffs

| Subsystem               | Design Choice Made                                              | Concrete Tradeoff                                                                 | Justification                                                                                                      |
|------------------------|-----------------------------------------------------------------|------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------|
| Namespace Isolation    | Used `chroot` instead of `pivot_root`.                         | Less secure; processes can theoretically escape a simple chroot jail.             | Simpler to implement for an academic prototype without requiring complex mount propagation setups.               |
| Supervisor Architecture | Single-threaded event loop (with one async logging thread).   | The supervisor cannot process multiple CLI commands simultaneously if one takes too long to execute. | Avoids complex race conditions and deadlocks when modifying the container metadata linked list.                  |
| IPC & Logging          | Used UNIX Domain Sockets instead of FIFOs.                     | Requires manual unlinking of the `.sock` file during startup and teardown to prevent bind errors. | Sockets provide a robust, bi-directional communication channel essential for sending full `ps` metadata back to the client. |
| Kernel Monitor         | Spinlocks for list protection instead of Mutexes.              | Disables interrupts on the local CPU, which can marginally impact system responsiveness if held too long. | Kernel timers run in interrupt context where sleeping (Mutexes) is strictly forbidden; spinlocks are mandatory here. |
| Scheduling Experiments | Hard-pinning the supervisor to Core 0 (`taskset`).             | Limits the maximum throughput of the container runtime to a single CPU core.      | Necessary to force a genuine CPU bottleneck so the CFS priority `nice` mechanics could be visibly demonstrated.   |

## Scheduler Experiment Results

| Container | Workload Type        | Nice Value              | Execution Time / CPU %     |
|-----------|----------------------|-------------------------|-----------------------------|
| Alpha     | `cpu_hog` (Math Loop) | -20 (Highest Priority)  | ~99.0% CPU                  |
| Beta      | `cpu_hog` (Math Loop) | 19 (Lowest Priority)    | ~0.0% - 1.0% CPU            |


Analysis of Linux Scheduling Behavior
The data perfectly demonstrates the mechanics of the Linux Completely Fair Scheduler (CFS). Because both workloads were entirely CPU-bound (infinite math loops with zero I/O sleep time), they both demanded 100% of the processing power.

By applying a nice value of -20 to alpha, we instructed the kernel to advance alpha's virtual runtime (vruntime) extremely slowly. Conversely, beta's nice value of 19 caused its vruntime to advance incredibly fast. Because CFS always schedules the process with the lowest accumulated vruntime, alpha was continuously re-selected by the scheduler to run on the processor. beta was subsequently starved of resources, proving that Linux provides administrators precise, ruthless control over resource allocation through user-space priority values.
