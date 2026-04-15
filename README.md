# 🚀 Supervised Multi-Container Runtime

> A lightweight container runtime with kernel-level monitoring, scheduling control, and IPC-driven supervision.


### 👥 Team Information
* **Team Member 1:** [Shriya S R] - [PES1UG24CS449]
* **Team Member 2:** [Ahana S S] - [PES1UG24CS910] 

---

### ⚙️ Build, Load, and Run Instructions
### Prerequisites
```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```
*Note: These instructions assume a fresh Ubuntu 22.04/24.04 VM environment.*

### 🛠️ Build the Project
```bash
cd /path/to/OS-Jackfruit
make
```

### 🧠 Load the Kernel Module
```bash
sudo insmod monitor.ko
ls -l /dev/container_monitor ## Verify control device exists
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

The design combines process isolation, parent-child lifecycle management, inter-process communication, concurrent logging, kernel-space memory enforcement, and scheduling experiments. Each part shows a different OS concept in action.

**Isolation Mechanism:**

The runtime creates container boundaries using Linux namespaces and a separate filesystem root. PID namespaces give each container its own process view, so processes inside the container do not see the host’s process tree in the same way they would on the host. UTS namespaces isolate the hostname, which makes the container appear like its own machine. Mount namespaces isolate mount points, so filesystems mounted inside the container do not affect the host namespace.

The filesystem boundary is created with chroot or, if used, pivot_root. Both change the root directory that the process sees, so paths like /bin/sh resolve inside the container rootfs instead of the host filesystem. This is what makes the container feel self-contained. At the same time, the host kernel is still shared by all containers. That is the key difference between containers and virtual machines. The containers do not get their own kernel; they share the same scheduler, memory manager, and device drivers, which is why isolation is strong enough for separation but not equivalent to full hardware virtualization.

**Supervisor and process lifecycle:**

The runtime uses a long-running supervisor because container management is not a one-shot action. The supervisor is responsible for starting containers, tracking their metadata, reaping them when they exit, and responding to stop requests. This avoids orphaned processes and zombies, which would happen if child processes terminated without the parent collecting their exit status.

The supervisor maintains container state across the entire lifecycle: starting, running, exited, stopped, killed, or hard-limit-killed. That metadata is important because it gives the system a single source of truth for commands like ps and logs. The supervisor also handles SIGCHLD so that terminated children are reaped with waitpid(..., WNOHANG) without blocking the rest of the runtime. That matters because the supervisor must keep serving commands while containers are running. In other words, the parent process is not just a launcher; it is the control plane for the whole runtime.

**IPC, Threads and Synchronization:** 

The project uses two different communication paths, and they solve different problems. One path is for control commands between the CLI and the supervisor. The other path is for container output that flows into the logging pipeline. Separating these two channels is the right design because command traffic and log traffic have very different timing and reliability needs.

The control channel is handled with UNIX domain sockets or another local IPC mechanism. This gives a structured way for the CLI to send commands like start, ps, logs, and stop, and receive responses back from the supervisor. Because the supervisor is long-running, this interface acts like a small local service.

The logging path is more concurrency-sensitive. Container output can arrive at any time, so a bounded-buffer producer-consumer design is appropriate. Producers read from container pipes and place records into the shared buffer. A consumer thread removes records and writes them to the correct log destination. Without synchronization, two major problems would appear. First, the buffer could be corrupted if multiple threads update its indices at the same time. Second, the system could deadlock or drop output if the buffer becomes full and producers are not correctly blocked until the consumer makes space.

That is why mutexes and condition variables are the right user-space primitives here. The mutex protects the shared buffer and metadata structures. The condition variables let producers sleep when the buffer is full and let consumers sleep when the buffer is empty. In the kernel module, spinlocks are appropriate for shared list protection because kernel code may run in contexts where sleeping is not allowed. The important idea is that each synchronization primitive matches the context where it is used.

**Memory Management and Enforcement** 

The kernel monitor is based on the fact that user space cannot reliably enforce memory policy by itself. RSS, or resident set size, measures how much physical memory is currently mapped into a process. It does not mean total virtual memory reserved, and it does not include memory that is merely allocated but not resident. That distinction matters because a process may reserve a large address space without actually consuming that much physical RAM.

The project uses both soft and hard limits because they serve different purposes. A soft limit is a warning or early-pressure threshold. It tells the system that a container is using more memory than expected, but it does not necessarily end the process immediately. A hard limit is the actual enforcement boundary. Once the hard limit is reached, the process must be terminated. This difference makes the memory policy more flexible and more realistic than a single kill-on-limit rule.

The enforcement belongs in kernel space because the kernel has authoritative access to process memory state and can terminate a process reliably at the right time. A user-space checker can observe memory usage, but it is always reactive and can be bypassed or delayed. The kernel module can inspect memory usage directly, maintain a tracked list of monitored processes, and apply policy consistently. That is why the module is not just an implementation detail; it is the enforcement point.

**Scheduling Behavior:** 

The scheduling experiment shows how Linux uses Completely Fair Scheduler behavior to divide CPU time between competing tasks. CFS does not give each process the same absolute number of cycles. Instead, it uses weights derived from nice values and tracks virtual runtime to decide who should run next. A lower nice value gives the process a higher weight, which means its virtual runtime grows more slowly and it gets more CPU time. A higher nice value does the opposite.

Because the workloads are CPU-bound, they do not sleep voluntarily, so the scheduler has to make a clearer choice about who gets the processor. That is why the difference between a high-priority and low-priority container becomes visible in the results. The higher-priority process gets most of the CPU share, while the lower-priority process receives very little.

If the test is pinned to a single core, the effect is easier to see because the scheduler cannot hide the imbalance by moving tasks to another CPU. That makes the experiment more controlled and makes the relationship between nice value, virtual runtime, and CPU share easier to explain. The result is not that Linux is unfair. The result is that Linux is intentionally fair according to the weights the user requested. That is the point of CFS: better responsiveness and throughput allocation based on policy, not equal CPU time for everything.

## Design Decisions and tradeoffs

| Subsystem               | Design Choice Made                                              | Concrete Tradeoff                                                                 | Justification                                                                                                      |
|------------------------|-----------------------------------------------------------------|------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------|
| Namespace Isolation    | Used `chroot` instead of `pivot_root`.                         | Less secure; processes can theoretically escape a simple chroot jail.             | Simpler to implement for an academic prototype without requiring complex mount propagation setups.               |
| Supervisor Architecture | Single-threaded event loop (with one async logging thread).   | The supervisor cannot process multiple CLI commands simultaneously if one takes too long to execute. | Avoids complex race conditions and deadlocks when modifying the container metadata linked list.                  |
| IPC & Logging          | Used UNIX Domain Sockets instead of FIFOs.                     | Requires manual unlinking of the `.sock` file during startup and teardown to prevent bind errors. | Sockets provide a robust, bi-directional communication channel essential for sending full `ps` metadata back to the client. |
| Kernel Monitor         | Spinlocks for list protection instead of Mutexes.              | Disables interrupts on the local CPU, which can marginally impact system responsiveness if held too long. | Kernel timers run in interrupt context where sleeping (Mutexes) is strictly forbidden; spinlocks are mandatory here. |
| Scheduling Experiments | Hard-pinning the supervisor to Core 0 (`taskset`).             | Limits the maximum throughput of the container runtime to a single CPU core.      | Necessary to force a genuine CPU bottleneck so the CFS priority `nice` mechanics could be visibly demonstrated.   |

The main tradeoff in this project is between realism and simplicity. Using namespaces, chroot, a supervisor, a bounded logging queue, and a kernel monitor gives a design that behaves like a real runtime, but each part is still simple enough to reason about in an academic setting.

Using chroot is simpler than pivot_root, but it is also weaker. Using a single supervisor keeps metadata management understandable, but it means the control path is not highly parallel. Using UNIX domain sockets gives clean local IPC, but it requires careful cleanup of socket files. Using kernel-space memory enforcement gives strong control, but it raises the cost of synchronization mistakes. Pinning scheduling tests to one core makes the experiment easier to interpret, but it reduces total throughput. Those are all reasonable choices because the goal of the project is to demonstrate OS principles clearly, not to build a production-grade container platform.

Overall, the project shows how containers are built from standard Linux primitives rather than from one special container feature. The runtime depends on namespaces for isolation, process management for lifecycle control, IPC for coordination, synchronization for correctness, kernel support for enforcement, and scheduler behavior for performance experiments. That combination is what makes the system a good educational example of operating-system design.

## Scheduler Experiment Results

| Container | Workload Type        | Nice Value              | Execution Time / CPU %     |
|-----------|----------------------|-------------------------|-----------------------------|
| Alpha     | `cpu_hog` (Math Loop) | -20 (Highest Priority)  | ~99.0% CPU                  |
| Beta      | `cpu_hog` (Math Loop) | 19 (Lowest Priority)    | ~0.0% - 1.0% CPU            |


Analysis of Linux Scheduling Behavior
The data perfectly demonstrates the mechanics of the Linux Completely Fair Scheduler (CFS). Because both workloads were entirely CPU-bound (infinite math loops with zero I/O sleep time), they both demanded 100% of the processing power.

By applying a nice value of -20 to alpha, we instructed the kernel to advance alpha's virtual runtime (vruntime) extremely slowly. Conversely, beta's nice value of 19 caused its vruntime to advance incredibly fast. Because CFS always schedules the process with the lowest accumulated vruntime, alpha was continuously re-selected by the scheduler to run on the processor. beta was subsequently starved of resources, proving that Linux provides administrators precise, ruthless control over resource allocation through user-space priority values.
