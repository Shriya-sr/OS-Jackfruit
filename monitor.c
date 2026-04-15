/*
 * monitor.c - Multi-Container Memory Monitor (Linux Kernel Module)
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"
#define CHECK_INTERVAL_SEC 1

/* ==============================================================
 * TODO 1: Define your linked-list node struct.
 * ============================================================== */
/* Each node tracks one monitored container/process pair and stores the
 * configured soft/hard limits plus a flag to avoid repeating soft warnings.
 */
struct monitor_node {
    /* Process ID being tracked by the monitor. */
    pid_t pid;
    /* Soft memory cap in bytes; crossing this only raises a warning. */
    unsigned long soft_limit_bytes;
    /* Hard memory cap in bytes; crossing this triggers SIGKILL. */
    unsigned long hard_limit_bytes;
    /* Human-readable container name copied from the user request. */
    char container_id[MONITOR_NAME_LEN];
    /* Prevents repeated soft-limit warnings for the same entry. */
    bool soft_warning_emitted;
    /* Kernel linked-list hook used to chain all monitored entries. */
    struct list_head list;
};

/* ==============================================================
 * TODO 2: Declare the global monitored list and a lock.
 * ============================================================== */
/* The list holds all active registrations, and the spinlock protects it
 * from concurrent access by the ioctl path and the periodic timer callback.
 */
static LIST_HEAD(monitor_list);
/* Spinlock that serializes access to the monitor list in process and timer context. */
static DEFINE_SPINLOCK(monitor_lock);

/* --- Provided: internal device / timer state --- */
static struct timer_list monitor_timer;
static dev_t dev_num;
static struct cdev c_dev;
static struct class *cl;

/* ---------------------------------------------------------------
 * Provided: RSS Helper
 * --------------------------------------------------------------- */
static long get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct *mm;
    long rss_pages = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        return -1;
    }
    get_task_struct(task);
    rcu_read_unlock();

    mm = get_task_mm(task);
    if (mm) {
        rss_pages = get_mm_rss(mm);
        mmput(mm);
    }
    put_task_struct(task);

    return rss_pages * PAGE_SIZE;
}

/* ---------------------------------------------------------------
 * Provided: soft-limit helper
 * --------------------------------------------------------------- */
static void log_soft_limit_event(const char *container_id,
                                 pid_t pid,
                                 unsigned long limit_bytes,
                                 long rss_bytes)
{
    printk(KERN_WARNING
           "[container_monitor] SOFT LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
           container_id, pid, rss_bytes, limit_bytes);
}

/* ---------------------------------------------------------------
 * Provided: hard-limit helper
 * --------------------------------------------------------------- */
static void kill_process(const char *container_id,
                         pid_t pid,
                         unsigned long limit_bytes,
                         long rss_bytes)
{
    struct task_struct *task;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task)
        send_sig(SIGKILL, task, 1);
    rcu_read_unlock();

    printk(KERN_WARNING
           "[container_monitor] HARD LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
           container_id, pid, rss_bytes, limit_bytes);
}

/* ---------------------------------------------------------------
 * Timer Callback - fires every CHECK_INTERVAL_SEC seconds.
 * --------------------------------------------------------------- */
static void timer_callback(struct timer_list *t)
{
    /* ==============================================================
     * TODO 3: Implement periodic monitoring.
     * ============================================================== */
    /* The timer runs once per second, scans every tracked PID, checks RSS,
     * and applies the correct action based on the configured limits.
     */
    /* entry points to the current node being examined. */
    struct monitor_node *entry, *tmp;
    /* flags stores interrupt state while the spinlock is held. */
    unsigned long flags;
    /* rss_bytes stores the latest resident-set size returned by the helper. */
    long rss_bytes;

    /* timer_list is unused because the callback always works on the global timer. */
    (void)t;

    /* Lock the list before walking or modifying it. */
    spin_lock_irqsave(&monitor_lock, flags);

    /* Safe iteration is required because the loop may delete entries. */
    list_for_each_entry_safe(entry, tmp, &monitor_list, list) {
        /* Read the current RSS of the process tied to this node. */
        rss_bytes = get_rss_bytes(entry->pid);

        /* If the PID no longer exists, stop tracking that entry. */
        if (rss_bytes < 0) {
            /* Remove the list link first so no future scan sees it. */
            list_del(&entry->list);
            /* Free the node because it is no longer needed. */
            kfree(entry);
            continue;
        }

        /* A hard-limit breach means the process must be terminated. */
        if (rss_bytes > entry->hard_limit_bytes) {
            /* Send SIGKILL to the offending process. */
            kill_process(entry->container_id, entry->pid, entry->hard_limit_bytes, rss_bytes);
            /* Remove the node after killing the process. */
            list_del(&entry->list);
            /* Release the memory for the node. */
            kfree(entry);
        }
        /* Soft-limit breach only prints once per container entry. */
        else if (rss_bytes > entry->soft_limit_bytes && !entry->soft_warning_emitted) {
            /* Emit the warning message to the kernel log. */
            log_soft_limit_event(entry->container_id, entry->pid, entry->soft_limit_bytes, rss_bytes);
            /* Mark that the warning has already been shown. */
            entry->soft_warning_emitted = true;
        }
    }

    /* Unlock after the scan is complete. */
    spin_unlock_irqrestore(&monitor_lock, flags);

    /* Re-arm the timer for the next monitoring cycle. */
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);
}

/* ---------------------------------------------------------------
 * IOCTL Handler
 * --------------------------------------------------------------- */
static long monitor_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    /* req receives the registration or unregister request from user space. */
    struct monitor_request req;
    /* new_node is used only when creating a new monitor entry. */
    struct monitor_node *new_node, *entry, *tmp;
    /* flags stores interrupt state while the monitor list is locked. */
    unsigned long flags;
    /* found becomes nonzero when unregister locates a matching PID. */
    int found = 0;

    (void)f;

    if (cmd != MONITOR_REGISTER && cmd != MONITOR_UNREGISTER)
        return -EINVAL;

    if (copy_from_user(&req, (struct monitor_request __user *)arg, sizeof(req)))
        return -EFAULT;

    if (cmd == MONITOR_REGISTER) {
        printk(KERN_INFO
               "[container_monitor] Registering container=%s pid=%d soft=%lu hard=%lu\n",
               req.container_id, req.pid, req.soft_limit_bytes, req.hard_limit_bytes);

        /* ==============================================================
         * TODO 4: Add a monitored entry.
         * ============================================================== */
        /* First validate the request, then allocate, initialize, and publish
         * the new node under the protection of the spinlock.
         */
        /* Soft limit must never be greater than the hard limit. */
        if (req.soft_limit_bytes > req.hard_limit_bytes)
            return -EINVAL;

        /* Allocate memory for the new tracking node. */
        new_node = kmalloc(sizeof(*new_node), GFP_KERNEL);
        if (!new_node)
            return -ENOMEM;

        /* Copy the PID from user space into the node. */
        new_node->pid = req.pid;
        /* Store the soft limit for later warning checks. */
        new_node->soft_limit_bytes = req.soft_limit_bytes;
        /* Store the hard limit for later kill checks. */
        new_node->hard_limit_bytes = req.hard_limit_bytes;
        /* Copy the container identifier safely and keep it terminated. */
        strncpy(new_node->container_id, req.container_id, MONITOR_NAME_LEN - 1);
        new_node->container_id[MONITOR_NAME_LEN - 1] = '\0';
        /* No warning has been sent yet for this entry. */
        new_node->soft_warning_emitted = false;

        /* Lock the list before publishing the new node. */
        spin_lock_irqsave(&monitor_lock, flags);
        /* Add the entry at the tail so registrations are kept in order. */
        list_add_tail(&new_node->list, &monitor_list);
        /* Release the lock after insertion. */
        spin_unlock_irqrestore(&monitor_lock, flags);

        /* Success: the process is now under monitoring. */
        return 0;
    }

    printk(KERN_INFO
           "[container_monitor] Unregister request container=%s pid=%d\n",
           req.container_id, req.pid);

    /* ==============================================================
     * TODO 5: Remove a monitored entry on explicit unregister.
     * ============================================================== */
    /* Search for the matching PID, remove it from the list, and free it. */
    /* The same spinlock protects unregister, register, and timer cleanup. */
    spin_lock_irqsave(&monitor_lock, flags);
    /* Safe iteration again because the matching node may be deleted. */
    list_for_each_entry_safe(entry, tmp, &monitor_list, list) {
        /* Match by PID because that is the tracking key used by the monitor. */
        if (entry->pid == req.pid) {
            /* Detach the node from the active list. */
            list_del(&entry->list);
            /* Free its memory immediately. */
            kfree(entry);
            /* Remember that a match was found so we can return success. */
            found = 1;
            break;
        }
    }
    /* Release the lock before returning to user space. */
    spin_unlock_irqrestore(&monitor_lock, flags);

    /* Return success only if we actually removed an entry. */
    return found ? 0 : -ENOENT;
}

/* --- Provided: file operations --- */
static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

/* --- Provided: Module Init --- */
static int __init monitor_init(void)
{
    if (alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME) < 0)
        return -1;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    cl = class_create(DEVICE_NAME);
#else
    cl = class_create(THIS_MODULE, DEVICE_NAME);
#endif
    if (IS_ERR(cl)) {
        unregister_chrdev_region(dev_num, 1);
        return PTR_ERR(cl);
    }

    if (IS_ERR(device_create(cl, NULL, dev_num, NULL, DEVICE_NAME))) {
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    cdev_init(&c_dev, &fops);
    if (cdev_add(&c_dev, dev_num, 1) < 0) {
        device_destroy(cl, dev_num);
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    timer_setup(&monitor_timer, timer_callback, 0);
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);

    printk(KERN_INFO "[container_monitor] Module loaded. Device: /dev/%s\n", DEVICE_NAME);
    return 0;
}

/* --- Provided: Module Exit --- */
static void __exit monitor_exit(void)
{
    /* entry points to each node that still remains in the list at unload time. */
    struct monitor_node *entry, *tmp;
    /* flags stores interrupt state while the spinlock is held. */
    unsigned long flags;

    /* Stop the periodic monitor before destroying the tracked list. */
    del_timer_sync(&monitor_timer);

    /* ==============================================================
     * TODO 6: Free all remaining monitored entries.
     * ============================================================== */
    /* Walk the list one last time and release every node still registered. */
    spin_lock_irqsave(&monitor_lock, flags);
    /* Safe iteration is required because each entry is deleted during the walk. */
    list_for_each_entry_safe(entry, tmp, &monitor_list, list) {
        /* Detach the node from the list. */
        list_del(&entry->list);
        /* Free the node so the module unload does not leak memory. */
        kfree(entry);
    }
    /* Release the lock after cleanup is complete. */
    spin_unlock_irqrestore(&monitor_lock, flags);

    /* Destroy the character device and release all allocated device resources. */
    cdev_del(&c_dev);
    /* Remove the /dev node that was created during init. */
    device_destroy(cl, dev_num);
    /* Destroy the device class used for the node. */
    class_destroy(cl);
    /* Return the major/minor number back to the kernel. */
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "[container_monitor] Module unloaded.\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Supervised multi-container memory monitor");
