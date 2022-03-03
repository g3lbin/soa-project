
#define EXPORT_SYMTAB
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/pid.h>     /* For pid types */
#include <linux/version.h> /* For LINUX_VERSION_CODE */
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/workqueue.h>
#include <linux/string.h>
#include <linux/wait.h>
#include <linux/moduleparam.h>

#define MODNAME         "MULTI-FLOW DEV"
#define DEVICE_NAME     "multi-flow-dev"
#define MINORS          (128)               // the device driver supports 128 devices
#define AUDIT           if(1)

// #define SINGLE_SESSION_OBJECT               // just one session per I/O node at a time

#define DEV_ENABLED     (1)                 // operating status of the enabled device

#define WQ_NAME_LENGTH  (24)                // same as WQ_NAME_LEN which is not exported

#define STREAMS_NUM     (2)
#define LOW_PRIORITY    (0)
#define HIGH_PRIORITY   (1)

#define MAX_STREAM_SIZE         PAGE_SIZE           // just one page: 4KB
#define MAX_WAIT_TIMEINT        LONG_MAX            // represent infinite time in jiffies

#define IOC_MAGIC 'r'           // https://www.kernel.org/doc/Documentation/ioctl/ioctl-number.txt
#define IOC_SWITCH_PRIORITY     _IO(IOC_MAGIC, 0x20)
#define IOC_SWITCH_BLOCKING     _IO(IOC_MAGIC, 0x21)
#define IOC_SET_WAIT_TIMEINT    _IOW(IOC_MAGIC, 0x22, long *)

#define CHARP_ENTRY_SIZE        (32)       // number of bytes reserved for each entry of charp array parameters

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 0, 0)
#define get_major(session) MAJOR(session->f_inode->i_rdev)
#define get_minor(session) MINOR(session->f_inode->i_rdev)
#else
#define get_major(session) MAJOR(session->f_dentry->d_inode->i_rdev)
#define get_minor(session) MINOR(session->f_dentry->d_inode->i_rdev)
#endif

#define do_write(prio, dest, src, count)                        \
({                                                              \
        int __ret = 0;                                          \
        if (prio == LOW_PRIORITY)                               \
                memcpy(dest, src, count);                       \
        else                                                    \
                __ret = copy_from_user(dest, src, count);       \
        __ret;                                                  \
})

#define lock_and_check(condition, mutexp)       \
({                                              \
        int __ret = 0;                          \
        mutex_lock(mutexp);                     \
        if (condition)                          \
                __ret = 1;                      \
        else                                    \
                mutex_unlock(mutexp);           \
        __ret;                                  \
})

#define mfd_module_param_array_named(name, array, type, nump, perm)     \
	param_check_##type(name, &((char **)array)[0]);                 \
	static const struct kparam_array __param_arr_##name             \
	= {     .max = ARRAY_SIZE(array), .num = nump,                  \
	        .ops = &mfd_param_ops_##type,                           \
	        .elemsize = sizeof(array[0]), .elem = array };          \
	__module_param_call(MODULE_PARAM_PREFIX, name,                  \
			    &mfd_param_array_ops,                       \
			    .arr = &__param_arr_##name,                 \
			    perm, -1, 0);                               \
	__MODULE_PARM_TYPE(name, "array of " #type)

struct device_struct {
#ifdef SINGLE_SESSION_OBJECT
        struct mutex busy;
#endif
        wait_queue_head_t waitq[STREAMS_NUM];   // f or blocking read and write operations
        struct workqueue_struct *wr_workq;      // for asynchronous execution of low prio write operations
        struct mutex sync[STREAMS_NUM];         // operation synchronizer of each stream
        int start[STREAMS_NUM];                 // first valid byte of each stream
        atomic_t valid_b[STREAMS_NUM];          // valid bytes of each stream
        atomic_t free_b[STREAMS_NUM];           // free space to execute write operations
        char *streams[STREAMS_NUM];             // streams' addresses
        atomic_t waiting_for_data[STREAMS_NUM]; // num   ber of threads currently waiting for data along the two flows
};

struct session_data {
        short current_priority;         // prio level for the operation
        long timeout;                   // timeout in jiffies to break the wait
};

struct packed_write {
        struct work_struct the_work;
        int minor;
        short prio;
        char *buf;
        size_t count;
        ssize_t (*real_write)(short, int, const char *, size_t);
};

static int dev_open(struct inode *, struct file *);
static int dev_release(struct inode *, struct file *);
static ssize_t dev_read(struct file *, char *, size_t, loff_t *);
static ssize_t dev_write(struct file *, const char *, size_t, loff_t *);
static long dev_ioctl(struct file *, unsigned int, unsigned long);
static void deferred_write(unsigned long);
static ssize_t actual_write(short, int, const char *, size_t);
static ssize_t actual_read(short, int, char *, size_t);
static int mfd_param_get_charp(char *, const struct kernel_param *);
static int mfd_param_array_get(char *, const struct kernel_param *);

static const struct kernel_param_ops mfd_param_ops_charp = {
        .get = &mfd_param_get_charp,
};

static const struct kernel_param_ops mfd_param_array_ops = {
        .get = mfd_param_array_get,
};

/* global variables */

static int major;                               // major number assigned to the device driver
static struct device_struct devices[MINORS];    // the devices objects

/* module parameters */

static int device_status[MINORS];
static char bytes_present[MINORS][CHARP_ENTRY_SIZE];
static char waiting_for_data[MINORS][CHARP_ENTRY_SIZE];

module_param_array(device_status, int, NULL, S_IRUGO | S_IWUSR);
mfd_module_param_array_named(bytes_present_on_devices_streams,
                             bytes_present, charp, NULL, S_IRUGO);
mfd_module_param_array_named(waiting_threads_on_devices_streams,
                             waiting_for_data, charp, NULL, S_IRUGO);

/* module parameters hook */

static int mfd_param_get_charp(char *buffer, const struct kernel_param *kp)
{
        struct device_struct *dev;
        
        if (strncmp(kp->name, "bytes_present_on_devices_streams", 32) == 0) {
                dev = devices + (long)kp->arg;
                return scnprintf(buffer,
                                 PAGE_SIZE,
                                 "dev[%ld] - high:%d,low:%d\n",
                                 (long)kp->arg,
                                 atomic_read(&(dev->valid_b[HIGH_PRIORITY])),
                                 atomic_read(&(dev->valid_b[LOW_PRIORITY])));
        }
        if (strncmp(kp->name, "waiting_threads_on_devices_streams", 34) == 0) {
                dev = devices + (long)kp->arg;
                return scnprintf(buffer,
                                 PAGE_SIZE,
                                 "dev[%ld] - high:%d,low:%d\n",
                                 (long)kp->arg,
                                 atomic_read(&(dev->waiting_for_data[HIGH_PRIORITY])),   // over the 80 columns
                                 atomic_read(&(dev->waiting_for_data[LOW_PRIORITY])));   // over the 80 columns
        }
        return scnprintf(buffer, PAGE_SIZE, "%s\n", *((char **)kp->arg));
}

static int mfd_param_array_get(char *buffer, const struct kernel_param *kp)
{
        int i, off, ret;
        const struct kparam_array *arr = kp->arr;
        struct kernel_param p = *kp;

        for (i = off = 0; i < (arr->num ? *arr->num : arr->max); i++) {
                p.arg = (void *)(long)i;
                ret = arr->ops->get(buffer + off, &p);
                if (ret < 0)
                        return ret;
                off += ret;
        }
        buffer[off] = '\0';
        return off;
}

/* internal functions */

static void deferred_write(unsigned long data)
{
        struct packed_write *wr_info;
        struct device_struct *dev;

        wr_info = (struct packed_write *)container_of((void *)data,
                                                     struct packed_write,
                                                     the_work);
        AUDIT
        printk("%s: kworker (%d) processes a low prio write on dev with [major,minor] number [%d,%d]\n",
               MODNAME, current->pid, major, wr_info->minor);

        dev = devices + wr_info->minor;

        mutex_lock(&(dev->sync[wr_info->prio]));
        (wr_info->real_write)(wr_info->prio,
                              wr_info->minor,
                              wr_info->buf,
                              wr_info->count);
        wake_up_interruptible(&(dev->waitq[wr_info->prio]));
        mutex_unlock(&(dev->sync[wr_info->prio]));

        free_page((unsigned long)wr_info->buf);
        kfree((void *)wr_info);
        module_put(THIS_MODULE);
        return;
}

static ssize_t actual_write(short prio, int minor, const char *buf, size_t count)       // over the 80 columns
{
        int ret;
        int amount;
        int first_free_b;
        struct device_struct *dev = devices + minor;

        first_free_b = (dev->start[prio] + atomic_read(&(dev->valid_b[prio])))
                        % MAX_STREAM_SIZE;

        if (count > (MAX_STREAM_SIZE - first_free_b)) {         // due to the circularity of the buffer
                amount = (MAX_STREAM_SIZE - first_free_b);
                ret = do_write(prio,
                               dev->streams[prio] + first_free_b,
                               buf,
                               amount);
                first_free_b = (first_free_b + amount - ret) % MAX_STREAM_SIZE;
                atomic_add((amount - ret), &(dev->valid_b[prio]));
                if (prio == HIGH_PRIORITY)
                        atomic_sub((amount - ret), &(dev->free_b[prio]));
                buf += (amount - ret);
                amount = (count - (amount - ret));
        } else {
                amount = count;
        }
        ret = do_write(prio, dev->streams[prio] + first_free_b, buf, amount);
        atomic_add((amount - ret), &(dev->valid_b[prio]));
        if (prio == HIGH_PRIORITY)
                atomic_sub((amount - ret), &(dev->free_b[prio]));
        return (count - ret);
}

ssize_t actual_read(short prio, int minor, char *buf, size_t count)
{
        int ret;
        int amount;
        struct device_struct *dev = devices + minor;

        if ((ret = atomic_read(&(dev->valid_b[prio]))) < count)
                count = ret;

        if (count > (MAX_STREAM_SIZE - dev->start[prio])) {         // due to the circularity of the buffer
                amount = (MAX_STREAM_SIZE - dev->start[prio]);
                ret = copy_to_user(buf,
                                   dev->streams[prio] + dev->start[prio],
                                   amount);
                dev->start[prio] = (dev->start[prio] + amount - ret)
                                    % MAX_STREAM_SIZE;
                atomic_sub((amount - ret), &(dev->valid_b[prio]));
                atomic_add((amount - ret), &(dev->free_b[prio]));
                buf += (amount - ret);
                amount = (count - (amount - ret));
        } else {
                amount = count;
        }
        ret = copy_to_user(buf, dev->streams[prio] + dev->start[prio], amount);
        dev->start[prio] = (dev->start[prio] + amount - ret) % MAX_STREAM_SIZE;
        atomic_sub((amount - ret), &(dev->valid_b[prio]));
        atomic_add((amount - ret), &(dev->free_b[prio]));
        return (count - ret);
}

/* the actual driver */

static int dev_open(struct inode *inode, struct file *file)
{
        struct device_struct *dev;
        struct session_data *session;
        int minor = get_minor(file);
        gfp_t mask = GFP_KERNEL;

        if (minor >= MINORS)
                return -ENODEV;
        if (!atomic_read((atomic_t *)&(device_status[minor])))
                return -EINVAL;         // the device is disabled
        
        dev = devices + minor;
#ifdef SINGLE_SESSION_OBJECT
        if (!mutex_trylock(&(dev->busy)))
                return -EBUSY;
#endif

        if (file->f_flags & O_NONBLOCK)
                mask |= GFP_ATOMIC;

        file->private_data = kzalloc(sizeof(struct session_data), mask);
        if (!file->private_data)
                return -ENOMEM;
        session = (struct session_data *)file->private_data;
        /* set defaults values for new session */
        session->current_priority = LOW_PRIORITY;
        session->timeout = MAX_WAIT_TIMEINT;            // no timeout

        if (!try_module_get(THIS_MODULE))
                return -ENODEV;
        AUDIT
        printk("%s: device file successfully opened for object with minor %d\n",
               MODNAME, minor);
        return 0;
}

static int dev_release(struct inode *inode, struct file *file)
{
#ifdef SINGLE_SESSION_OBJECT
        struct device_struct *dev = devices + get_minor(file);
        mutex_unlock(&(dev->busy));
#endif
        kfree(file->private_data);
        module_put(THIS_MODULE);
        AUDIT
        printk("%s: device file closed for object with minor %d\n",
               MODNAME, get_minor(file));
        return 0;
}

ssize_t dev_read(struct file *file, char *buf, size_t count, loff_t *pos)
{
        long ret;
        short idx;
        struct device_struct *dev = devices + get_minor(file);
        struct session_data *session = (struct session_data *)file->private_data;       // over the 80 columns

        AUDIT
        printk("%s: somebody called a read on dev with [major,minor] number [%d,%d]\n",
               MODNAME, get_major(file), get_minor(file));

        idx = session->current_priority;

        if (file->f_flags & O_NONBLOCK) {
                if (!mutex_trylock(&(dev->sync[idx])))
                        return -EAGAIN;
                if (atomic_read(&(dev->valid_b[idx])) == 0) {
                        mutex_unlock(&(dev->sync[idx]));
                        return -EAGAIN;
                }
        } else {
                atomic_inc(&(dev->waiting_for_data[idx]));
                ret = wait_event_interruptible_timeout(dev->waitq[idx],
                                lock_and_check(atomic_read(&(dev->valid_b[idx])) > 0,   // over the 80 columns
                                               &(dev->sync[idx])),
                                session->timeout);
                if (ret == 0)
                        return -ETIME;
                if (ret == -ERESTARTSYS)
                        return -EINTR;
        }
        atomic_dec(&(dev->waiting_for_data[idx]));
        ret = actual_read(idx, get_minor(file), buf, count);
        wake_up_interruptible(&(dev->waitq[idx]));
        mutex_unlock(&(dev->sync[idx]));
        return (ssize_t)ret;
}

ssize_t dev_write(struct file *file, const char *buf, size_t count, loff_t *pos)   // over the 80 columns
{
        long ret;
        struct packed_write *container;
        struct device_struct *dev = devices + get_minor(file);
        struct session_data *session = (struct session_data *)file->private_data;   // over the 80 columns
        short idx = session->current_priority;  
        gfp_t mask = GFP_KERNEL;

        AUDIT
        printk("%s: somebody called a write on dev with [major,minor] number [%d,%d]\n",
                MODNAME, get_major(file), get_minor(file));

        if (file->f_flags & O_NONBLOCK) {
                if (!mutex_trylock(&(dev->sync[idx])))
                        return -EAGAIN;
                if (atomic_read(&(dev->free_b[idx])) == 0) {  // the stream is full
                        mutex_unlock(&(dev->sync[idx]));
                        return -EAGAIN;
                }
        } else {
                ret = wait_event_interruptible_timeout(dev->waitq[idx],
                                lock_and_check(atomic_read(&(dev->free_b[idx])) > 0,
                                               &(dev->sync[idx])),
                                session->timeout);
                if (ret == 0)
                        return -ETIME;
                if (ret == -ERESTARTSYS)
                        return -EINTR;
        }
        ret = atomic_read(&(dev->free_b[idx]));
        if (ret < count)        // do partial write
                count = ret;
        if (idx == HIGH_PRIORITY) {
                ret = actual_write(idx, get_minor(file), buf, count);
                wake_up_interruptible(&(dev->waitq[idx]));
                mutex_unlock(&(dev->sync[idx]));
                return ret;
        }
        /* schedule deferred work */
        if (!try_module_get(THIS_MODULE)) {
                mutex_unlock(&(dev->sync[idx]));
                return -ENODEV;
        }
        if (file->f_flags & O_NONBLOCK)
                mask |= GFP_ATOMIC; // non blocking memory allocation
        container = (struct packed_write *)kzalloc(sizeof(struct packed_write),
                                                  mask);
        if (!container) {
                mutex_unlock(&(dev->sync[idx]));
                module_put(THIS_MODULE);
                return -ENOMEM;
        }
        /* fill struct packed_write */
        container->minor = get_minor(file);
        container->prio = idx;
        container->buf = (char *)get_zeroed_page(mask);
        if (!container->buf) {
                kfree((void *)container);
                mutex_unlock(&(dev->sync[idx]));
                module_put(THIS_MODULE);
                return -ENOMEM;
        }
        if (count > MAX_STREAM_SIZE)
                count = MAX_STREAM_SIZE;
        ret = copy_from_user(container->buf, buf, count);
        container->count = (count - ret);
        container->real_write = actual_write;
        __INIT_WORK(&(container->the_work),
                    (void *)deferred_write,
                    (unsigned long)(&(container->the_work)));
        queue_work(dev->wr_workq, &container->the_work);
        atomic_sub((count - ret), &(dev->free_b[idx]));
        mutex_unlock(&(dev->sync[idx]));
        return (count - ret);
}

static long dev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
        long __user *argp = (long __user *)arg;
        struct session_data *session = file->private_data;
        long ret = 0;

        AUDIT
        printk("%s: somebody called a ioctl on dev with [major,minor] number [%d,%d] and command %d\n",
               MODNAME, get_major(file), get_minor(file), cmd);

        switch (cmd)
        {
        case IOC_SWITCH_PRIORITY:
                if (session->current_priority == LOW_PRIORITY)
                        ret = session->current_priority = HIGH_PRIORITY;
                else
                        ret = session->current_priority = LOW_PRIORITY;
                break;
        case IOC_SWITCH_BLOCKING:
                if (file->f_flags & O_NONBLOCK)
                        file->f_flags ^= O_NONBLOCK;
                else
                        file->f_flags |= O_NONBLOCK;
                break;
        case IOC_SET_WAIT_TIMEINT:
                if (copy_from_user(&ret, argp, sizeof(ret)) != 0)
                        return -EBADTYPE;       // the argument is not a long
                if (ret <= 0)
                        return -EINVAL;         // invalid argument
                ret *= HZ;                      // convert secs in jiffies
                if (ret > MAX_WAIT_TIMEINT)
                        session->timeout = ret = MAX_WAIT_TIMEINT;
                else
                        session->timeout = ret;
                break;

        default:
                ret = -ENOTTY;
                break;
        }
        return ret;
}

static struct file_operations fops = {
        .owner = THIS_MODULE,
        .write = dev_write,
        .read = dev_read,
        .open = dev_open,
        .release = dev_release,
        .unlocked_ioctl = dev_ioctl
};

int multi_flow_dev_init(void)
{
        int i, j, k;
        char queue_name[WQ_NAME_LENGTH];

        /* initialize the drive internal state */
        for (i = 0; i < MINORS; i++) {
#ifdef SINGLE_SESSION_OBJECT
                mutex_init(&(devices[i].busy));
#endif
                device_status[i] = DEV_ENABLED;
                snprintf(queue_name, sizeof(queue_name), "%s-%d", DEVICE_NAME, i);      // over the 80 columns
                devices[i].wr_workq = create_singlethread_workqueue(queue_name);        // over the 80 columns
                if (!devices[i].wr_workq) {
                        if (i > 0) {
                                j = 0;
                                goto revert_allocation;
                        }
                        return -ENOMEM;
                }
                for (j = 0; j < STREAMS_NUM; j++) {
                        init_waitqueue_head(&(devices[i].waitq[j]));
                        mutex_init(&(devices[i].sync[j]));
                        devices[i].start[j] = 0;
                        atomic_set(&(devices[i].valid_b[j]), 0);
                        atomic_set(&(devices[i].free_b[j]), MAX_STREAM_SIZE);
                        atomic_set(&(devices[i].waiting_for_data[j]), 0);
                        devices[i].streams[j] = NULL;
                        devices[i].streams[j] = (char *)get_zeroed_page(GFP_KERNEL);    // over the 80 columns
                        if (devices[i].streams[j] == NULL)
                                goto revert_allocation;
                }
        }
        major = __register_chrdev(0, 0, 256, DEVICE_NAME, &fops);
        if (major < 0) {
                pr_err("%s: registering device failed\n", MODNAME);
                return major;
        }
        pr_info("%s: new device registered, it is assigned major number %d\n",
                MODNAME, major);
        return 0;

revert_allocation:
        for (; i >= 0; i--) {
                destroy_workqueue(devices[i].wr_workq);
                for (k = 0; k < STREAMS_NUM; k++) {
                        free_page((unsigned long)devices[i].streams[k]);
                        if (i == 0 && k == j)
                                break;
                }
        }
        return -ENOMEM;
}

void multi_flow_dev_cleanup(void)
{
        int i, k;
        for (i = 0; i < MINORS; i++) {
                destroy_workqueue(devices[i].wr_workq);
                for (k = 0; k < STREAMS_NUM; k++) {
                        free_page((unsigned long)devices[i].streams[k]);
                }
        }
        unregister_chrdev(major, DEVICE_NAME);
        pr_info("%s: new device unregistered, it was assigned major number %d\n",
                MODNAME, major);
        return;
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Cristiano Cuffaro <cristiano.cuffaro@outlook.com>");
MODULE_DESCRIPTION("Linux character device driver implementing low and high priority flows of data.");

module_init(multi_flow_dev_init);
module_exit(multi_flow_dev_cleanup);