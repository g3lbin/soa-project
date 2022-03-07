
#define EXPORT_SYMTAB
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/version.h>
#include <linux/slab.h>

#include "mfdlib/ioctl.h"

#define MODNAME         "MULTI-FLOW DEV"
#define DEVICE_NAME     "multi-flow-dev"
#define MINORS          (128)           // the device driver supports 128 devices

/* Uncomment the following line if you want just one session per I/O node at a time */
// #define SINGLE_SESSION_OBJECT

#define DEV_ENABLED     (1)             // operating status of the enabled device

#define WQ_NAME_LENGTH  (24)            // same as WQ_NAME_LEN which is not exported

#define STREAMS_NUM     (2)             // number of different data flows
#define LOW_PRIORITY    (0)             // index for low priority stream
#define HIGH_PRIORITY   (1)             // index for high priority stream

#define MAX_STREAM_SIZE         PAGE_SIZE       // the size of each stream
#define MAX_WAIT_TIMEINT        LONG_MAX        // represent infinite time in jiffies

#define CHARP_ENTRY_SIZE        (32)    // number of bytes reserved for each entry of charp array parameters

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 0, 0)
#define get_major(session) MAJOR(session->f_inode->i_rdev)
#define get_minor(session) MINOR(session->f_inode->i_rdev)
#else
#define get_major(session) MAJOR(session->f_dentry->d_inode->i_rdev)
#define get_minor(session) MINOR(session->f_dentry->d_inode->i_rdev)
#endif

/**
 * lock_and_check - lock the mutex and check the condition
 * @condition: a C expression for the event to wait for
 * @mutexp: the pointer to the mutex to be locked
 * 
 * Returns:
 * 0 if the @condition is evaluated as %false,
 * 1 if the @condition is evaluated as %true.
 * In the first case, the mutex is unlocked before return.
 */
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

/**
 * mfd_module_param_array_named - renamed parameter which is an array of some type
 * @name: a valid C identifier which is the parameter name
 * @array: the name of the array variable
 * @type: the type of each entry
 * @nump: optional pointer filled in with the number written
 * @perm: visibility in sysfs
 *
 * This is a specific reimplementation of the module_param_array_named() which
 * permits to re-define the param_ops_##type and the param_array_ops, in order to
 * satisfy the project' specification.
 */
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

/**
 * device_struct - keeps the multi-flow device file informations
 * @busy: mutex used for single session operative mode
 * @waitq: waitqueues for blocking read and write operations
 * @wr_workq: workqueues for asynchronous execution of low priority writes
 * @sync: operation synchronizer of each stream
 * @streams: streams' addresses
 * @start: first valid byte (readable) of each stream
 * @valid_b: valid bytes (readables) of each stream
 * @free_b: free space to execute write operations
 * @waiting_for_data: number of threads currently waiting for data along the streams
 */
struct device_struct {
#ifdef SINGLE_SESSION_OBJECT
        struct mutex            busy;
#endif
        wait_queue_head_t       waitq[STREAMS_NUM];
        struct workqueue_struct *wr_workq;
        struct mutex            sync[STREAMS_NUM];
        char                    *streams[STREAMS_NUM];
        int                     start[STREAMS_NUM];
        atomic_t                valid_b[STREAMS_NUM];
        atomic_t                free_b[STREAMS_NUM];
        atomic_t                waiting_for_data[STREAMS_NUM];
} __randomize_layout;

/**
 * session_data - the data associated with the I/O session
 * @current_priority: priority level (high or low) for the operations
 * @timeout: timeout interval (in jiffies) to break the wait of blocking ops
 */
struct session_data {
        short   current_priority;
        long    timeout;
};

/**
 * packed_write - keeps the information observable from within the deferred work
 * @the_work: the deferred work
 * @minor: minor number of the multi-flow device file
 * @buf: the kernel buffer which contains the source data
 * @count: number of bytes to write
 * @real_write: pointer to actual write function
 */
struct packed_write {
        struct work_struct      the_work;
        int                     minor;
        char                    *buf;
        size_t                  count;
        ssize_t                 (*real_write)(short, int, const char *, size_t);
};

/* Driver functions */
static int      mfd_open(struct inode *, struct file *);
static int      mfd_release(struct inode *, struct file *);
static ssize_t  mfd_read(struct file *, char *, size_t, loff_t *);
static ssize_t  mfd_write(struct file *, const char *, size_t, loff_t *);
static long     mfd_ioctl(struct file *, unsigned int, unsigned long);
/* Internal driver functions */
static void     deferred_write(struct work_struct *);
static ssize_t  actual_write(short, int, const char *, size_t);
static ssize_t  actual_read(short, int, char *, size_t);
/* Module parameters hooks */
static int      mfd_param_get_charp(char *, const struct kernel_param *);
static int      mfd_param_array_get(char *, const struct kernel_param *);
/* Helper functions */
static inline struct packed_write       *to_packed_write(struct work_struct *);
static __always_inline int              do_write(short, char *, const char *, size_t);

/*
 * This struct holds the function that is invoked whenever
 * the value of a charp parameter is requested.
 */
static const struct kernel_param_ops mfd_param_ops_charp = {
        .get = &mfd_param_get_charp,
};

/*
 * This struct holds the function that is invoked whenever
 * the value of an array of parameters is requested.
 */
static const struct kernel_param_ops mfd_param_array_ops = {
        .get = mfd_param_array_get,
};

/* Global variables */
static int                      major;                  // major number assigned to the device driver
static struct device_struct     devices[MINORS];        // the multi-flow device objects

/* Module parameters */
static int      device_status[MINORS];                          // can be enabled (1) or disabled (0)
static char     bytes_present[MINORS][CHARP_ENTRY_SIZE];        // bytes present on the devices streams
static char     waiting_for_data[MINORS][CHARP_ENTRY_SIZE];     // waiting threads on the devices streams

module_param_array(device_status, int, NULL, S_IRUGO | S_IWUSR);
mfd_module_param_array_named(bytes_present_on_devices_streams,
                             bytes_present, charp, NULL, S_IRUGO);
mfd_module_param_array_named(waiting_threads_on_devices_streams,
                             waiting_for_data, charp, NULL, S_IRUGO);

/**
 * mfd_param_get_charp - writes a representation of the parameter value
 * @buffer: the buffer to place the result into
 * @kp: kernel parameter
 * 
 * Returns the number of characters written into @buffer not including
 * the trailing '\0'.
 */
static int mfd_param_get_charp(char *buffer, const struct kernel_param *kp)
{
        struct device_struct *dev;

        if (strncmp(kp->name, "bytes_present_on_devices_streams", 32) == 0) {
                dev = devices + (long)kp->arg;
                return scnprintf(buffer,
                                 CHARP_ENTRY_SIZE,
                                 "dev[%03ld] - high:%4d, low:%4d\n",
                                 (long)kp->arg,
                                 atomic_read(&(dev->valid_b[HIGH_PRIORITY])),
                                 atomic_read(&(dev->valid_b[LOW_PRIORITY])));
        }
        if (strncmp(kp->name, "waiting_threads_on_devices_streams", 34) == 0) {
                dev = devices + (long)kp->arg;
                return scnprintf(buffer,
                                 CHARP_ENTRY_SIZE,
                                 "dev[%03ld] - high:%4d, low:%4d\n",
                                 (long)kp->arg,
                                 atomic_read(&(dev->waiting_for_data[HIGH_PRIORITY])),
                                 atomic_read(&(dev->waiting_for_data[LOW_PRIORITY])));
        }
        return scnprintf(buffer, PAGE_SIZE, "%s\n", *((char **)kp->arg));
}

/**
 * mfd_param_array_get - writes the parameter array value that appears in sysfs
 * @buffer: the buffer to place the result into
 * @kp: kernel parameter pointing to the array
 * 
 * Returns the number of characters written into @buffer not including
 * the trailing '\0'.
 */
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

/**
 * to_packed_write - retrieves the external struct that embeds the work_struct
 * @work: workqueue item
 * 
 * Returns a pointer to packed_write structure.
 */
static inline struct packed_write *to_packed_write(struct work_struct *work)
{
	return container_of(work, struct packed_write, the_work);
}

/**
 * do_write - writes up to count bytes from src to dest
 * @prio: the data flow priority
 * @dest: the destination buffer
 * @src: the source buffer
 * @count: the number of bytes you hope to write
 * 
 * If @prio is LOW_PRIORITY, the @src buffer is located in the kernel space.
 * If @prio is HIGH_PRIORITY, the @src buffer is located in the user space,
 * so the writing can be truncated.
 * 
 * Returns the number of unwritten bytes.
 */
static __always_inline int do_write(short prio, char *dest, const char *src, size_t count)
{
        int ret = 0;

        if (prio == LOW_PRIORITY)
                memcpy(dest, src, count);
        else
                ret = copy_from_user(dest, src, count);
        return ret;
}

/**
 * deferred_write - performs the asynchronous writing
 * @work: workqueue item
 * 
 * This function is performed by the kworker daemon which
 * invokes the actual writing of data to the stream.
 */
static void deferred_write(struct work_struct *work)
{
        struct packed_write *wr_info;
        struct device_struct *dev;

        wr_info = to_packed_write(work);

        pr_debug("%s: kworker (%d) processes a low prio write on dev with [major,minor] number [%d,%d]\n",
               MODNAME, current->pid, major, wr_info->minor);

        dev = devices + wr_info->minor;

        mutex_lock(&(dev->sync[LOW_PRIORITY]));
        (wr_info->real_write)(LOW_PRIORITY,
                              wr_info->minor,
                              wr_info->buf,
                              wr_info->count);
        wake_up_interruptible(&(dev->waitq[LOW_PRIORITY]));
        mutex_unlock(&(dev->sync[LOW_PRIORITY]));

        free_page((unsigned long)wr_info->buf);
        kfree((void *)wr_info);
        module_put(THIS_MODULE);
        return;
}

/**
 * actual_write - performs actual writing of data to the stream
 * @prio: priority level of the write operation
 * @minor: device's minor number
 * @buf: temporary buffer
 * @count: the number of bytes you hope to write
 * 
 * Note: the @buf temporary buffer can be in kernel space or user space.
 * 
 * Returns the number of characters written within the stream.
 */
static ssize_t actual_write(short prio, int minor, const char *buf, size_t count)
{
        int ret;
        int amount;
        int first_free_b;
        struct device_struct *dev = devices + minor;

        first_free_b = (dev->start[prio] + atomic_read(&(dev->valid_b[prio])))
                        % MAX_STREAM_SIZE;
        /* Due to the circularity of the buffer */
        if (count > (MAX_STREAM_SIZE - first_free_b)) {
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

/**
 * actual_read - performs the actual reading of the data from the stream
 * @prio: priority level of the read operation
 * @minor: device's minor number
 * @buf: buffer corresponding to device stream
 * @count: the number of bytes you hope to read
 * 
 * Returns the number of characters read from the stream.
 */
static ssize_t actual_read(short prio, int minor, char *buf, size_t count)
{
        int ret;
        int amount;
        struct device_struct *dev = devices + minor;

        if ((ret = atomic_read(&(dev->valid_b[prio]))) < count)
                count = ret;
        /* Due to the circularity of the buffer */
        if (count > (MAX_STREAM_SIZE - dev->start[prio])) {
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

/**
 * mfd_open - open the multi-flow device file with f_flags initialized
 * @inode: I/O metadata of the device file
 * @file: I/O session to the device file
 * 
 * Returns 0 if the operation is successful, otherwise a negative value.
 */
static int mfd_open(struct inode *inode, struct file *file)
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

        pr_debug("%s: device file successfully opened for object with [major,minor] number [%d,%d]\n",
               MODNAME, get_major(file), get_minor(file));
        return 0;
}

/**
 * mfd_release - release the multi-flow device file
 * @inode: I/O metadata of the device file
 * @file: I/O session to the device file
 * 
 * Returns 0
 */
static int mfd_release(struct inode *inode, struct file *file)
{
#ifdef SINGLE_SESSION_OBJECT
        struct device_struct *dev = devices + get_minor(file);
        mutex_unlock(&(dev->busy));
#endif
        kfree(file->private_data);

        pr_debug("%s: device file closed for object with dev with [major,minor] number [%d,%d]\n",
               MODNAME, get_major(file), get_minor(file));
        return 0;
}

/**
 * mfd_read - attempts to read up to count bytes from multi-flow device file
 * @file: I/O session to the device file
 * @buf: destination buffer
 * @count: the number of bytes you hope to read
 * @unused: unused parameter
 * 
 * Data delivery follows a First-in-First-out policy, so after the read 
 * operations, the read data disappears from the stream.
 * 
 * Note: read operations are all executed synchronously.
 * 
 * Returns the number of characters read from the stream.
 */
static ssize_t mfd_read(struct file *file, char *buf, size_t count, loff_t *unused)
{
        long ret;
        short idx;
        struct device_struct *dev = devices + get_minor(file);
        struct session_data *session = (struct session_data *)file->private_data;

        pr_debug("%s: somebody called a read on dev with [major,minor] number [%d,%d]\n",
               MODNAME, get_major(file), get_minor(file));

        idx = session->current_priority;

        if (file->f_flags & O_NONBLOCK) {
                if (!mutex_trylock(&(dev->sync[idx])))          // busy
                        return -EAGAIN;
                if (atomic_read(&(dev->valid_b[idx])) == 0) {   // the stream is empty
                        mutex_unlock(&(dev->sync[idx]));
                        return -EAGAIN;
                }
        } else {
                atomic_inc(&(dev->waiting_for_data[idx]));
                ret = wait_event_interruptible_timeout(dev->waitq[idx],
                                lock_and_check(atomic_read(&(dev->valid_b[idx])) > 0,
                                               &(dev->sync[idx])),
                                session->timeout);
                if (ret == 0) {
                        atomic_dec(&(dev->waiting_for_data[idx]));
                        return -ETIME;
                }
                if (ret == -ERESTARTSYS) {
                        atomic_dec(&(dev->waiting_for_data[idx]));
                        return -EINTR;
                }
        }
        atomic_dec(&(dev->waiting_for_data[idx]));
        ret = actual_read(idx, get_minor(file), buf, count);
        wake_up_interruptible(&(dev->waitq[idx]));
        mutex_unlock(&(dev->sync[idx]));
        return (ssize_t)ret;
}

/**
 * mfd_write - attempts to write up to count bytes from multi-flow device file
 * @file: I/O session to the device file
 * @buf: source buffer
 * @count: the number of bytes you hope to write
 * @unused: unused parameter
 * 
 * Data delivery follows a First-in-First-out policy, so the data is written
 * at the end of those already present.
 * 
 * Note: the write operation on the high priority data stream is synchronous,
 * while the one on the low priority data stream is asynchronous, but still
 * keeping the interface able to synchronously notify the outcome.
 * 
 * Returns the number of characters written to the stream.
 */
static ssize_t mfd_write(struct file *file, const char *buf, size_t count, loff_t *unused)
{
        long ret;
        struct packed_write *container;
        struct device_struct *dev = devices + get_minor(file);
        struct session_data *session = (struct session_data *)file->private_data;
        short idx = session->current_priority;
        gfp_t mask = GFP_KERNEL;

        pr_debug("%s: somebody called a write on dev with [major,minor] number [%d,%d]\n",
                MODNAME, get_major(file), get_minor(file));

        if (file->f_flags & O_NONBLOCK) {
                if (!mutex_trylock(&(dev->sync[idx])))          // busy
                        return -EAGAIN;
                if (atomic_read(&(dev->free_b[idx])) == 0) {    // the stream is full
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
        /* Schedule deferred work */
        if (!try_module_get(THIS_MODULE)) {
                mutex_unlock(&(dev->sync[idx]));
                return -ENODEV;
        }
        if (file->f_flags & O_NONBLOCK)
                mask |= GFP_ATOMIC; // non-blocking memory allocation
        container = (struct packed_write *)kzalloc(sizeof(struct packed_write),
                                                   mask);
        if (!container) {
                mutex_unlock(&(dev->sync[idx]));
                module_put(THIS_MODULE);
                return -ENOMEM;
        }
        /* Fill struct packed_write */
        container->minor = get_minor(file);
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
                    deferred_write,
                    (unsigned long)(&(container->the_work)));
        queue_work(dev->wr_workq, &container->the_work);
        atomic_sub((count - ret), &(dev->free_b[idx]));
        mutex_unlock(&(dev->sync[idx]));
        return (count - ret);
}

/**
 * mfd_ioctl - handles the I/O control requests
 * @file: I/O session to the device file
 * @cmd: IOCTL command code
 * @arg: command argument
 * 
 * Returns:
 * the current priority level after changing it if @cmd is IOC_SWITCH_PRIORITY,
 * the status of O_NONBLOCK flag after changing it if @cmd is IOC_SWITCH_BLOCKING,
 * the timeout interval in jiffies after setting it if @cmd is IOC_SET_WAIT_TIMEINT,
 * -EINVAL if @arg is not valid,
 * -ENOTTY if @cmd is not valid.
 */
static long mfd_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
        long __user *argp = (long __user *)arg;
        struct session_data *session = file->private_data;
        long ret = 0;

        pr_debug("%s: somebody called a ioctl on dev with [major,minor] number [%d,%d] and command %d\n",
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
                ret = (file->f_flags & O_NONBLOCK);
                break;
        case IOC_SET_WAIT_TIMEINT:
                if (copy_from_user(&ret, argp, sizeof(ret)) != 0)
                        return -EINVAL;
                if (ret <= 0)
                        return -EINVAL;
                /* Convert secs in jiffies */
                ret *= HZ;
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

/*
 * This struct holds the functions that make up the character device driver.
 */
static struct file_operations fops = {
        .owner          = THIS_MODULE,
        .write          = mfd_write,
        .read           = mfd_read,
        .open           = mfd_open,
        .release        = mfd_release,
        .unlocked_ioctl = mfd_ioctl
};

/**
 * multi_flow_dev_init - does the module initialization
 * 
 * This function performs the needed memory allocations and initializations of
 * multi-flow devices informations.
 * 
 * Note: by default, each device is equipped with a buffer for high priority and
 * one for low priority operations, both with a size equal to PAGE_SIZE.
 * 
 * Returns 0 if initialization is successful, otherwise a negative value.
 */
static int multi_flow_dev_init(void)
{
        int i, j, k;
        char queue_name[WQ_NAME_LENGTH];

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
        pr_err("%s: kernel version no longer supported\n", MODNAME);
        return -1;
#endif

        /* initialize the drive internal state */
        for (i = 0; i < MINORS; i++) {
#ifdef SINGLE_SESSION_OBJECT
                mutex_init(&(devices[i].busy));
#endif
                device_status[i] = DEV_ENABLED;
                snprintf(queue_name, sizeof(queue_name), "%s-%d", DEVICE_NAME, i);
                devices[i].wr_workq = create_singlethread_workqueue(queue_name);
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
                        devices[i].streams[j] = (char *)get_zeroed_page(GFP_KERNEL);
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
                        if (i == 0 && k == j) break;
                }
        }
        return -ENOMEM;
}

/**
 * multi_flow_dev_cleanup - does the module cleanup
 */
static void multi_flow_dev_cleanup(void)
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