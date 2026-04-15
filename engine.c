/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 * - command-line shape is defined
 * - key runtime data structures are defined
 * - bounded-buffer skeleton is defined
 * - supervisor / client split is outlined
 *
 * Students are expected to design:
 * - the control-plane IPC implementation
 * - container lifecycle and metadata synchronization
 * - clone + namespace setup for each container
 * - producer/consumer behavior for log buffering
 * - signal handling and graceful shutdown
 */

#define _GNU_SOURCE 
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

/* Global flag for graceful shutdown */
volatile sig_atomic_t keep_running = 1;
/*When you press Ctrl+C, it sets the keep_running to 0 and exits the execution of the supervisor*/
static void handle_signal(int signo)
{
    if (signo == SIGINT || signo == SIGTERM) { //SIGINT = Interrupt, SIGTERM = Termination
        keep_running = 0;
    }
}

typedef enum {
    CMD_SUPERVISOR = 0, //process should boot up as the long-running daemon itself.
    CMD_START, //Triggers the spawn container logic
    CMD_RUN, //Tells the client to wait in the foreground until the container exits
    CMD_PS, //Read LL and send back the metadate to all the containers
    CMD_LOGS, //Request the supervisor to read specific log files and stream it
    CMD_STOP //Commands the supervisor to find a specific PID and send it a SIGKILL
} command_kind_t;

/*Helps track the lifecycle of the containers, used in PS command, logging and cleanup*/
typedef enum {
    CONTAINER_STARTING = 0, //Container is being created
    CONTAINER_RUNNING, //Container is actively running
    CONTAINER_STOPPED, // Gracefully stopped by user
    CONTAINER_KILLED, // Forcefully killed (SIGKILL or memory limit)
    CONTAINER_EXITED // Finished normally
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN]; //Name of the container (alpha, beta), used to identify containers
    pid_t host_pid; //actual Linux PID of the container process
    time_t started_at; //When container was launched , useful for logs and scheduling experiments
    container_state_t state; //Tracks current lifecycle stage
    unsigned long soft_limit_bytes; //Soft limit → warning only
    unsigned long hard_limit_bytes;// Hard limit → kill process
    int exit_code; //If container exits normally: give exit code
    int exit_signal; //If killed: give exit signal
    char log_path[PATH_MAX]; //Where logs are stored
    struct container_record *next; //Points to next container in list
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN]; //Which container generated this log
    size_t length; //Size of actual data inside data[]
    char data[LOG_CHUNK_SIZE]; //Actual log content
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY]; //Array storing log messages
    size_t head; //Points to next item to remove
    size_t tail; // Points to next free slot to insert
    size_t count; //Number of items currently in buffer
    int shutting_down; 
    pthread_mutex_t mutex; ////only one thread accesses buffer at a time, mutex
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

/* It’s a structure used to send commands from the CLI to the supervisor.*/
typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;
/*reply from the supervisor back to the CLI.*/
typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;
/*holds all the info needed to start a container process.*/
typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;
/*main brain/state of the supervisor.*/
typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t; /* This structure holds the main state of the supervisor, including the 
server socket, monitor device, logging thread, log buffer, metadata lock, and linked list of container records.*/

static void usage(const char *prog)//Prints usage instructions for the CLI commands.
{
    fprintf(stderr, 
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}
/* Parses a MIB flag and converts it to bytes. */
static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) { // Expect flags in pairs: --flag value
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) { // Check if there's a value following the flag
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) { // Parse soft limit in MiB and convert to bytes
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) { // Parse hard limit in MiB and convert to bytes
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) { //Parse nice value and validate range
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {// Validate that soft limit does not exceed hard limit
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state) 
{ //Converts a container state enum value to a human-readable string for display.
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL); //Create a mutex to protect access to the buffer. 
    if (rc != 0) //This ensures that only one thread can modify the buffer at a time, preventing race conditions.
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL); //Create a condition variable to signal when the buffer transitions from empty to non-empty.

    if (rc != 0) { 
        return rc;
    }
    rc = pthread_cond_init(&buffer->not_full, NULL); // Signal to producer when space is there in buffer
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{// Clean up resources associated with the bounded buffer, including mutex and condition variables.
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{// Signal all threads that shutdown is beginning, so they can exit gracefully.
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/*
 * TODO:
 * Implement producer-side insertion into the bounded buffer.
 *
 * Requirements:
 * - block or fail according to your chosen policy when the buffer is full
 * - wake consumers correctly
 * - stop cleanly if shutdown begins
 */

/*Adds log item to the buffer when multiple threads are accessing it*/
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);//Only 1 thread can modify the buffer at a time, so we lock the mutex before accessing it.

    // Wait if the buffer is full, unless we are shutting down
    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    }

    // Abort if a shutdown was triggered while we were waiting
    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    // Insert the item
    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    // Wake up any waiting consumers, before there was nothing, now there is something, so we signal the not_empty condition.
    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);

    return 0;
}

/*
 * TODO:
 * Implement consumer-side removal from the bounded buffer.
 *
 * Requirements:
 * - wait correctly while the buffer is empty
 * - return a useful status when shutdown is in progress
 * - avoid races with producers and shutdown
 */
/* Removes a log item from the buffer when multiple threads are accessing it */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    // Wait if the buffer is empty, unless we are shutting down
    while (buffer->count == 0 && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }

    // If we are shutting down AND the buffer is empty, time to exit
    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    // Remove the item
    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    // Wake up any waiting producers, before there was no space, now there is space, so we signal the not_full condition.
    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);

    return 0;
}

/*
 * TODO:
 * Implement the logging consumer thread.
 *
 * Suggested responsibilities:
 * - remove log chunks from the bounded buffer
 * - route each chunk to the correct per-container log file
 * - exit cleanly when shutdown begins and pending work is drained
 */
static void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = arg;//Gives access to the supervisor context, which includes the log buffer and container metadata.

    while (!ctx->should_stop) {//Keep running until the supervisor signals shutdown.
        memset(&item, 0, sizeof(item));

        int rc = bounded_buffer_pop(&ctx->log_buffer, &item);//Wait for a log item to be available in the buffer, and pop it out.

        if (rc == 0 && item.container_id[0] != '\0') {//If we successfully got a log item and it has a valid container ID, we can process it. 
            printf("[LOG] %s\n", item.container_id);
            fflush(stdout);
        }
    }

    return NULL;
}
/* Logs an event for a specific container */
static void log_event(supervisor_ctx_t *ctx,
                      const char *id,
                      const char *event)
{// Create a log item with the container ID and event, and push it to the bounded buffer for the logging thread to consume.
    log_item_t item;
    memset(&item, 0, sizeof(item));

    snprintf(item.container_id,
             sizeof(item.container_id),
             "%s:%s",
             id,
             event);// Format the log message to include the container ID and event type.
    printf("DEBUG: pushing log %s:%s\n", id, event);

    bounded_buffer_push(&ctx->log_buffer, &item);
}

/*
 * TODO:
 * Implement the clone child entrypoint.
 *
 * Required outcomes:
 * - isolated PID / UTS / mount context
 * - chroot or pivot_root into rootfs
 * - working /proc inside container
 * - stdout / stderr redirected to the supervisor logging path
 * - configured command executed inside the container
 */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    /* Apply nice priority before locking down the container */
    if (cfg->nice_value != 0) {
        nice(cfg->nice_value);
    }

    chdir(cfg->rootfs);// Change the current working directory to the new root filesystem, so that relative paths inside the container work correctly.
    chroot(cfg->rootfs);// Change the root directory for the process to the new root filesystem, effectively isolating it from the host filesystem.

    mount("proc", "/proc", "proc", 0, NULL);// Mount the proc filesystem inside the container, which is necessary for many tools and commands to work properly.

    /* Disconnect standard input so the container can't steal the terminal */
    int null_fd = open("/dev/null", O_RDWR);
    if (null_fd >= 0) {
        dup2(null_fd, STDIN_FILENO);
        close(null_fd);
    }

    /* Execute the command passed from the config */
    execl("/bin/sh", "sh", "-c", cfg->command, NULL);

    perror("execl");
    return 1;
}
/* Spawns a new container process */
static pid_t spawn_container(const control_request_t *req)
{
    char *stack = malloc(STACK_SIZE);// Allocate a stack for the child process created by clone. The stack grows downwards.
    child_config_t *cfg = malloc(sizeof(child_config_t));
    
    strcpy(cfg->rootfs, req->rootfs);// Copy the root filesystem path from the control request to the child configuration structure, so the child process knows where to chroot.
    strcpy(cfg->command, req->command);// Copy the command to execute from the control request to the child configuration structure, so the child process knows what command to run.
    cfg->nice_value = req->nice_value;

    int flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;// Use clone to create a new process with its own PID namespace, UTS namespace, and mount namespace. The SIGCHLD flag ensures that the parent gets notified when the child exits.
    pid_t pid = clone(child_fn, stack + STACK_SIZE, flags, cfg);
    
    if (pid < 0) {// If clone fails, print an error message and return -1.
        perror("clone");
    }
    return pid;
}
/* Registers a container with the monitoring system */
int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{// Prepare a monitor_request structure with the container's PID, memory limits, and ID, and send it to the kernel monitor device using an ioctl call.
    struct monitor_request req;

    memset(&req, 0, sizeof(req));// Clear the request structure to ensure there are no uninitialized fields.
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;// Set the soft memory limit for the container, which is a threshold for when the monitor should start issuing warnings about memory usage.
    req.hard_limit_bytes = hard_limit_bytes;// Set the hard memory limit for the container, which is the maximum amount of memory the container is allowed to use before the monitor kills it.
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);// Copy the container ID into the request structure, which is used by the monitor to identify which container the request is for.

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)// Send the registration request to the kernel monitor device. If the ioctl call fails, print an error message and return -1.
        return -1;

    return 0;
}
/* Adds a new container to the supervisor's tracking list */
static void add_container(supervisor_ctx_t *ctx, const char *id, pid_t pid)
{// Create a new container record with the given ID and PID, and add it to the linked list of containers in the supervisor context. This allows the supervisor to track the state of each container and manage them accordingly.
    container_record_t *rec = malloc(sizeof(container_record_t));// Allocate memory for a new container record, which will hold metadata about the container such as its ID, PID, state, and log path.
    if (!rec)
        return;

    memset(rec, 0, sizeof(*rec));// Clear the new container record to ensure all fields are initialized to default values.

    strncpy(rec->id, id, CONTAINER_ID_LEN - 1);// Copy the container ID into the record, which is used to identify the container in logs and status reports.
    rec->host_pid = pid;
    rec->state = CONTAINER_RUNNING;
    rec->started_at = time(NULL);

    pthread_mutex_lock(&ctx->metadata_lock);// Lock the metadata mutex to ensure that adding the new container record to the linked list is thread
    rec->next = ctx->containers;
    ctx->containers = rec;// Add the new container record to the head of the linked list of containers in the supervisor context. This allows the supervisor to keep track of all active containers and their metadata.
    pthread_mutex_unlock(&ctx->metadata_lock);
}

/*
 * TODO:
 * Implement the long-running supervisor process.
 *
 * Suggested responsibilities:
 * 1) open /dev/container_monitor
 * 2) create the control socket / FIFO / shared-memory channel
 * 3) install SIGCHLD / SIGINT / SIGTERM handling
 * 4) spawn the logger thread
 * 5) enter the supervisor event loop
 * - accept control requests and update container state
 * - reap children and respond to signals
 */

/* Main supervisor function */
static int run_supervisor(const char *rootfs)
{// Initialize the supervisor context, including mutexes, log buffer, and container tracking structures. 
//This sets up the internal state needed for the supervisor to manage containers and handle logging.
    supervisor_ctx_t ctx;// Create a supervisor context structure to hold the state of the supervisor, including the server socket, monitor device, logging thread, log buffer, metadata lock, and linked list of container records.
    int rc;

    memset(&ctx, 0, sizeof(ctx));// Clear the supervisor context to ensure all fields are initialized to default values.

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);// Initialize a mutex for protecting access to the container metadata linked list. This ensures that only one thread can modify the container list at
    if (rc != 0) {
        perror("pthread_mutex_init");
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);// Initialize the bounded buffer for logging. This sets up the data structure and synchronization primitives needed for the producer-consumer logging system.
    if (rc != 0) {
        perror("bounded_buffer_init");
        return 1;
    }

    printf("Supervisor started for rootfs: %s\n", rootfs);

    /* create control socket */
    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);// Create a UNIX domain socket for the control plane, which will be used to receive commands from the CLI. The socket is created in the filesystem at CONTROL_PATH.
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));// Set up the address structure for the UNIX domain socket, specifying the family and path where the socket will be created.
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {// Bind the server socket to the specified path. This makes the socket available for clients to connect to. If the bind call fails, print an error message and return -1.
        perror("bind");
        return 1;
    }

    if (listen(ctx.server_fd, 5) < 0) {// Start listening for incoming connections on the control socket. This allows the supervisor to accept commands from the CLI and manage containers accordingly.
        perror("listen");
        return 1;
    }

    ctx.should_stop = 0;// Initialize the should_stop flag to 0, indicating that the supervisor should continue running until it receives a shutdown signal.
    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc != 0) {
        perror("pthread_create");// Create a separate thread for handling logging. This allows the supervisor to process log messages from containers asynchronously, without blocking the main event loop.
        return 1;
    }

    /* Register signal handlers for graceful shutdown */
    struct sigaction sa;// Set up signal handlers for SIGINT and SIGTERM to allow for graceful shutdown of the supervisor. When these signals are received, the handler will set the keep_running flag to 0, which will cause the main event loop to exit and trigger cleanup.
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);// Initialize the signal set to be empty, meaning that no signals will be blocked during the execution of the signal handler.
    sa.sa_flags = 0; 
    sigaction(SIGINT, &sa, NULL);// Register the signal handler for SIGINT (interrupt signal, typically sent by Ctrl+C) to allow the supervisor to exit gracefully when the user requests it.
    sigaction(SIGTERM, &sa, NULL);

    /* Open the kernel monitor device */
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);// Open the kernel monitor device, which is used to register containers for memory monitoring and enforcement. This allows the supervisor to set memory limits on containers and receive notifications when those limits are exceeded.
    if (ctx.monitor_fd < 0) {// If opening the monitor device fails, print an error message but continue running without monitoring capabilities. The supervisor can still manage containers, but it won't be able to enforce memory limits or receive notifications about memory usage.
        perror("Failed to open /dev/container_monitor");
    }

    /* serve CLI requests until SIGINT or SIGTERM is caught */
    while (keep_running) {
        
        /* 1. Check for dead containers (Zombie Reaper) */
        pthread_mutex_lock(&ctx.metadata_lock);// Lock the metadata mutex to safely access the linked list of container records. This ensures that we can check the state of each container and
        container_record_t *c = ctx.containers;// Iterate through the linked list of container records to check for any containers that have exited or been killed. This is necessary to update the state of containers and perform any necessary cleanup.
        while (c) {
            if (c->state == CONTAINER_RUNNING) {// If the container is currently running, we check if it has exited or been killed by calling waitpid with the WNOHANG option, which allows us to check the status of the child process without blocking.
                int wstatus;
                if (waitpid(c->host_pid, &wstatus, WNOHANG) > 0) {// If waitpid returns a positive value, it means that the child process has changed state (exited or been killed), and we can check the status to determine how it ended.
                    if (WIFSIGNALED(wstatus) && WTERMSIG(wstatus) == SIGKILL) {
                        c->state = CONTAINER_KILLED;// If the child process was killed by a SIGKILL signal, we update the container state to CONTAINER_KILLED and log this event. This indicates that the container was forcefully terminated, either by the user or by the monitor due to memory limits.
                        log_event(&ctx, c->id, "hard_limit_killed");
                    } else {// If the child process exited normally (not killed by a signal), we update the container state to CONTAINER_EXITED and log this event. This indicates that the container finished its execution on its own.
                        c->state = CONTAINER_EXITED;
                        log_event(&ctx, c->id, "exited");
                    }
                }
            }
            c = c->next;
        }
        pthread_mutex_unlock(&ctx.metadata_lock);// Unlock the metadata mutex after we are done checking the state of all containers. This allows other threads to access the container metadata when needed.

        /* 2. Wait for CLI commands (SOCK_CLOEXEC prevents fd leaks to children) */
        int client_fd = accept4(ctx.server_fd, NULL, NULL, SOCK_CLOEXEC);// Wait for incoming connections on the control socket. When a client connects, accept the connection and get a file descriptor for communicating with that client. The SOCK_CLOEXEC flag ensures that this file descriptor will be automatically closed if the supervisor spawns a child process, preventing file descriptor leaks.
        if (client_fd < 0) {// If accepting a connection fails, check if it was due to an interrupt signal (EINTR). If it was, we break out of the loop to allow for graceful shutdown. Otherwise, we print an error message and continue waiting for the next connection.
            if (errno == EINTR) break;
            continue;
        }

        control_request_t req;// Create a control_request_t structure to hold the command sent by the client. This structure will be filled with the data received from the client, which includes the command type and any associated parameters.
        ssize_t n = recv(client_fd, &req, sizeof(req), 0);
        
        if (n <= 0) {// If receiving data from the client fails (n <= 0), we close the client file descriptor and continue waiting for the next connection. This handles cases where the client disconnects or sends invalid data.
            close(client_fd);
            continue;
        }

        /* 3. Handle specific commands */
        if (req.kind == CMD_START) {// If the command is CMD_START, we call the spawn_container function to create a new container process based on the parameters specified in the control request. If the container is successfully spawned (new_pid > 0), we add it to the supervisor's tracking list and log that it has started. If the monitor device is available, we also register the new container with the monitor to enforce memory limits.
            pid_t new_pid = spawn_container(&req);// Spawn a new container process based on the control request parameters, which include the container ID, root filesystem, command to execute, and resource limits. This creates a new process with its own namespaces and executes the specified command inside the container.
            if (new_pid > 0) {// If the container was successfully spawned, we add it to the supervisor's tracking list using the add_container function, which creates a new container record and adds it to the linked list of containers in the supervisor context. We also log that the container has started using the log_event function, which pushes a log item to the bounded buffer for the logging thread to consume. If the monitor device is available, we register the new container with the monitor using the register_with_monitor function, which sends an ioctl request to the kernel monitor device to set up memory monitoring and enforcement for this container.
                add_container(&ctx, req.container_id, new_pid);
                log_event(&ctx, req.container_id, "started");
                
                if (ctx.monitor_fd >= 0) {// If the monitor device was successfully opened, we register the new container with the monitor to enforce memory limits. This allows the monitor to track the container's memory usage and take action if it exceeds the specified limits.
                    register_with_monitor(ctx.monitor_fd, req.container_id, new_pid, req.soft_limit_bytes, req.hard_limit_bytes);
                }
            }
        } 
        else if (req.kind == CMD_STOP) {// If the command is CMD_STOP, we look up the specified container in the linked list of container records. If we find a matching container that is currently running, we send it a SIGKILL signal to forcefully terminate it, update its state to CONTAINER_KILLED, and log this event. This allows the user to stop a container immediately through the CLI.
            pthread_mutex_lock(&ctx.metadata_lock);
            container_record_t *cur = ctx.containers;
            while (cur) {// Iterate through the linked list of container records to find the container with the specified ID that is currently running. If we find it, we send it a SIGKILL signal to terminate it, update its state to CONTAINER_KILLED, and log this event. This allows the user to stop a container immediately through the CLI.
                if (strcmp(cur->id, req.container_id) == 0 && cur->state == CONTAINER_RUNNING) {
                    kill(cur->host_pid, SIGKILL);// Send a SIGKILL signal to the container's host PID to forcefully terminate it. This is used for the CMD_STOP command to allow the user to stop a container immediately.
                    cur->state = CONTAINER_KILLED;
                    log_event(&ctx, cur->id, "killed");
                    break;
                }
                cur = cur->next;
            }
            pthread_mutex_unlock(&ctx.metadata_lock);// Unlock the metadata mutex after we are done searching for the container and potentially modifying its state. This allows other threads to access the container metadata when needed.
        }

        /* 4. Always send container list back to client */
        pthread_mutex_lock(&ctx.metadata_lock);// Lock the metadata mutex to safely access the linked list of container records. This ensures that we can read the state of each container and send an accurate list back to the client.
        container_record_t *cur = ctx.containers;
        while (cur) {// Iterate through the linked list of container records and send the ID, PID, and state of each container back to the client. This allows the client to see the current status of all containers after issuing a command.
            dprintf(client_fd, "ID=%s PID=%d STATE=%s\n", cur->id, cur->host_pid, state_to_string(cur->state));
            cur = cur->next;
        }
        pthread_mutex_unlock(&ctx.metadata_lock);// Unlock the metadata mutex after we are done reading the container metadata. This allows other threads to access the container metadata when needed.
        close(client_fd);
    }

    /* =========================================
     * TEARDOWN AND CLEANUP PHASE
     * ========================================= */
    printf("\nShutting down supervisor...\n");

    ctx.should_stop = 1;
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);

    close(ctx.server_fd);
    unlink(CONTROL_PATH);

    pthread_mutex_lock(&ctx.metadata_lock);// Lock the metadata mutex to safely access the linked list of container records. This ensures that we can iterate through the list and perform cleanup actions on any remaining containers.
    container_record_t *cur = ctx.containers;// Iterate through the linked list of container records and send a SIGKILL signal to any containers that are still running. This ensures that all containers are terminated when the supervisor shuts down, preventing any lingering processes from continuing to run after the supervisor has exited.
    while (cur) {// If the container is still running, we send it a SIGKILL signal to forcefully terminate it. This is part of the cleanup process when the supervisor is shutting down, ensuring that no containers are left running after the supervisor exits.
        if (cur->state == CONTAINER_RUNNING || cur->state == CONTAINER_STARTING) {
            printf("Terminating lingering container: %s (PID: %d)\n", cur->id, cur->host_pid);
            kill(cur->host_pid, SIGKILL);
        }
        cur = cur->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);// Unlock the metadata mutex after we are done checking the state of all containers and sending termination signals. This allows other threads to access the container metadata if needed during the shutdown process.

    printf("Cleanup complete. Exiting.\n");
    return 0;
}

/*
 * TODO:
 * Implement the client-side control request path.
 *
 * The CLI commands should use a second IPC mechanism distinct from the
 * logging pipe. A UNIX domain socket is the most direct option, but a
 * FIFO or shared memory design is also acceptable if justified.
 */
/* Thread function to send control requests to the supervisor */
static int send_control_request(const control_request_t *req)// This function is responsible for sending a control request to the supervisor process using a UNIX domain socket. It creates a socket, connects to the supervisor's control socket, sends the control request, and then reads and prints the response from the supervisor until the connection is closed.
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);// Create a UNIX domain socket for communicating with the supervisor. This socket will be used to send the control request and receive the response. If the socket creation fails, we print an error message and return -1.
    if (fd < 0) {// If creating the socket fails, print an error message and return 1 to indicate failure. This could happen if there are insufficient resources or if the system does not support UNIX domain sockets.
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;// Set up the address structure for the UNIX domain socket, specifying the family and path where the supervisor's control socket is located. This is necessary to connect to the correct socket for sending the control request.
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);// Copy the path to the supervisor's control socket into the address structure. This is the path where the supervisor is listening for control requests, and we need to connect to this socket to send our request.

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {// Connect to the supervisor's control socket using the specified address. If the connection fails, we print an error message, close the socket file descriptor to clean up resources, and return 1 to indicate failure. This could happen if the supervisor is not running or if there are permission issues with the socket.
        perror("connect");
        close(fd);
        return 1;
    }

    /* Send the full struct over IPC so the supervisor gets all arguments */
    write(fd, req, sizeof(control_request_t));// Write the control request structure to the socket, sending all the necessary information for the supervisor to process the command. This includes the command type, container ID, root filesystem, command to execute, and resource limits.

    char buffer[512];
    ssize_t n;

    while ((n = read(fd, buffer, sizeof(buffer))) > 0) {// Read the response from the supervisor in a loop until the connection is closed. The supervisor will send back information about the current state of containers, which we print to standard output. If reading from the socket fails, we break out of the loop and close the socket.
        write(STDOUT_FILENO, buffer, n);
    }

    close(fd);
    return 0;
}

static int cmd_start(int argc, char *argv[])// This function handles the "start" command from the CLI. It parses the command-line arguments to fill a control_request_t structure with the necessary information to start a new container, including the container ID, root filesystem, command to execute, and optional resource limits. It then calls send_control_request to send this request to the supervisor.
{
    control_request_t req;

    if (argc < 5) {// If there are not enough arguments provided for the "start" command, we print a usage message to standard error and return 1 to indicate failure. The expected usage is: <program> start <id> <container-rootfs> <command> [optional flags].
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));// Clear the control request structure to ensure all fields are initialized to default values. This prevents any uninitialized data from being sent to the supervisor.
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);// Copy the container ID from the command-line arguments into the control request structure. This ID is used by the supervisor to identify the container being started.
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);// Copy the root filesystem path from the command-line arguments into the control request structure. This specifies the root filesystem that the new container will use when it is started.
    strncpy(req.command, argv[4], sizeof(req.command) - 1);// Copy the command to execute from the command-line arguments into the control request structure. This is the command that will be run inside the new container when it is started.
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;// Set the default soft memory limit for the container in the control request structure. This is the default value that will be used if the user does not specify a different limit using optional flags.
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;// Set the default hard memory limit for the container in the control request structure. This is the default value that will be used if the user does not specify a different limit using optional flags.

    if (parse_optional_flags(&req, argc, argv, 5) != 0)// Parse any optional flags provided in the command-line arguments to override the default resource limits or nice value. If parsing the optional flags fails (e.g., due to invalid input), we return 1 to indicate failure.
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])// This function handles the "run" command from the CLI, which is similar to "start" but may have different semantics (e.g., it might wait for the container to finish). It parses the command-line arguments to fill a control_request_t structure and then sends it to the supervisor using send_control_request.
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_ps(void)// This function handles the "ps" command from the CLI, which requests a list of all containers and their statuses from the supervisor. It creates a control_request_t structure with the CMD_PS command and sends it to the supervisor using send_control_request. The supervisor will respond with the list of containers, which is printed to standard output.
{
    control_request_t req;// Create a control request structure for the CMD_PS command, which is used to request a list of all containers and their statuses from the supervisor. We clear the structure to ensure it is initialized properly, set the command type to CMD_PS, and then send it to the supervisor using send_control_request.
    memset(&req, 0, sizeof(req));// Clear the control request structure to ensure all fields are initialized to default values. This prevents any uninitialized data from being sent to the supervisor, which could cause undefined behavior.

    req.kind = CMD_PS;// Set the command type in the control request structure to CMD_PS, indicating that we want to request a list of all containers and their statuses from the supervisor.

    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])// This function handles the "logs" command from the CLI, which requests the logs for a specific container from the supervisor. It parses the command-line arguments to get the container ID, fills a control_request_t structure with the CMD_LOGS command and the container ID, and then sends it to the supervisor using send_control_request. The supervisor will respond with the logs for that container, which is printed to standard output.
{
    control_request_t req;

    if (argc < 3) {// If there are not enough arguments provided for the "logs" command, we print a usage message to standard error and return 1 to indicate failure. The expected usage is: <program> logs <id>.
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));// Clear the control request structure to ensure all fields are initialized to default values. This prevents any uninitialized data from being sent to the supervisor, which could cause undefined behavior.
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);// Copy the container ID from the command-line arguments into the control request structure. This specifies which container's logs we want to retrieve from the supervisor.

    return send_control_request(&req);
}

static int cmd_stop(const char *id)// This function handles the "stop" command from the CLI, which requests that a specific container be stopped by the supervisor. It takes the container ID as an argument, fills a control_request_t structure with the CMD_STOP command and the container ID, and then sends it to the supervisor using send_control_request. The supervisor will respond with the updated list of containers, which is printed to standard output.
{
    control_request_t req;
    memset(&req, 0, sizeof(req));

    req.kind = CMD_STOP;// Set the command type in the control request structure to CMD_STOP, indicating that we want to request that a specific container be stopped by the supervisor.
    strncpy(req.container_id, id, sizeof(req.container_id) - 1);

    return send_control_request(&req);
}
/* Main function to handle command-line arguments and dispatch to appropriate command handlers */
int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
            return 1;
        }
        return cmd_stop(argv[2]);
    }

    usage(argv[0]);
    return 1;
}
