/**
 * @file multi-flow-dev.c
 * @brief This is the main source for the Linux Kernel Module which implements
 *        a multi-flow char device driver.
 * 
 * Through an open session to the device file a thread can read/write data
 * segments. The data delivery follows a First-in-First-out policy along each of
 * the two different data flows (low and high priority). After read operations,
 * the read data disappear from the flow. Also, the high priority data flow
 * offers synchronous write operations while the low priority data flow offers
 * an asynchronous execution (based on delayed work) of write operations, while
 * still keeping the interface able to synchronously notify the outcome.
 * Read operations are all executed synchronously.
 * 
 * The device driver supports 128 devices corresponding to the same amount of
 * minor numbers.
 *
 * @author Cristiano Cuffaro
 * 
 * @date March 12, 2022
 */

#define EXPORT_SYMTAB
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/version.h>
#include <linux/slab.h>

#include "include/mfd.h"
#include "include/ioctl.h"

#define MODNAME         "MULTI-FLOW DEV"
#define DEVICE_NAME     MODNAME         /* because module name has only one type of device */
#define MINORS          (128)           /* the device driver supports 128 devices */

/* Driver functions */
static int      mfd_open(struct inode *inode, struct file *file);
static int      mfd_release(struct inode *inode, struct file *file);
static ssize_t  mfd_read(struct file *file, char *buf, size_t count,
                                loff_t *unused);
static ssize_t  mfd_write(struct file *file, const char *buf, size_t count,
                                loff_t *unused);
static long     mfd_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
/* Internal driver functions */
static int      schedule_deferred_work(int minor, struct work_struct *work);
static void     deferred_write(struct work_struct *work);
static void     actual_write(short prio, int minor, struct data_segment *new);
static ssize_t  actual_read(short prio, int minor, char *buf, size_t count);
/* Module parameters hooks */
static int      mfd_param_get_charp(char *buffer, const struct kernel_param *kp);
static int      mfd_param_array_get(char *buffer, const struct kernel_param *kp);

/*
 * This struct holds the function that is invoked whenever
 * the value of a charp parameter is requested.
 */
const struct kernel_param_ops mfd_param_ops_charp = {
        .get = mfd_param_get_charp,
};
EXPORT_SYMBOL(mfd_param_ops_charp);

/*
 * This struct holds the function that is invoked whenever
 * the value of an array of parameters is requested.
 */
const struct kernel_param_ops mfd_param_array_ops = {
        .get = mfd_param_array_get,
};
EXPORT_SYMBOL(mfd_param_array_ops);

/* Global variables */
static int                      major;                  /* major number assigned to the device driver */
static struct device_struct     devices[MINORS];        /* the multi-flow device objects */

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
                                 "[%03ld] - high:%5d, low:%5d\n",
                                 (long)kp->arg,
                                 get_stream_len(dev, HIGH_PRIORITY),
                                 get_stream_len(dev, LOW_PRIORITY));
        }
        if (strncmp(kp->name, "waiting_threads_on_devices_streams", 34) == 0) {
                dev = devices + (long)kp->arg;
                return scnprintf(buffer,
                                 CHARP_ENTRY_SIZE,
                                 "[%03ld] - high:%5d, low:%5d\n",
                                 (long)kp->arg,
                                 get_waiting_threads(dev, HIGH_PRIORITY),
                                 get_waiting_threads(dev, LOW_PRIORITY));
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
 * schedule_deferred_work - initializes and schedules the work item
 * @minor: the device's minor number
 * @work: the work item in question
 * @mask: GFP flag combination
 * 
 * Internally, the function increments the module usage counter to ensure
 * that it is not removed before the deferred work is executed.
 */
static int schedule_deferred_work(int minor, struct work_struct *work)
{
        struct device_struct *dev = devices + minor;

        if (!try_module_get(THIS_MODULE))
                return -ENODEV;
        __INIT_WORK(work, deferred_write, (unsigned long)work);
        if (!queue_work(dev->wr_workq, work))
                return -ENOMEM;
        return 0;
}

/**
 * deferred_write - performs the asynchronous writing
 * @work: workqueue item
 * 
 * This function is performed by the kworker daemon which invokes the actual
 * writing of data to the stream and decrement the module usage counter that
 * was incremented by the schedule_deferred_work function.
 */
static void deferred_write(struct work_struct *work)
{
        struct packed_write *wr_info;
        struct device_struct *dev;

        wr_info = to_packed_write(work);

        pr_debug("%s: kworker '%d' writes on device with [major,minor] number [%d,%d]\n",
               MODNAME, current->pid, major, wr_info->minor);

        dev = devices + wr_info->minor;

        mutex_lock(&(dev->sync[LOW_PRIORITY]));
        (wr_info->real_write)(LOW_PRIORITY, wr_info->minor, wr_info->seg);
        wake_up_interruptible(&(dev->waitq[LOW_PRIORITY]));
        mutex_unlock(&(dev->sync[LOW_PRIORITY]));

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
 * As the data streams are implemented, the actual writing is reduced to
 * appending the data segment at the end of the respective list.
 */
static void actual_write(short prio, int minor, struct data_segment *new)
{
        struct device_struct *dev = devices + minor;
        struct list_head *tail = &(dev->streams[prio].tail);

        list_add_tail(&(new->list), tail);
        /* Update stream metadata */
        add_stream_len(new->size, dev, prio);
        if (prio == HIGH_PRIORITY)
                sub_free_space(new->size, dev, prio);
        return;
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
        struct list_head *pos;
        struct data_segment *seg;
        long b_read = 0;
        struct device_struct *dev = devices + minor;

        if ((ret = get_stream_len(dev, prio)) < count)
                count = ret;

        pos = dev->streams[prio].head.next;
        for (; b_read < count; pos = pos->next) {
                seg = list_entry(pos, struct data_segment, list);
                amount = ((count - b_read) > seg->size) ?
                                seg->size : (count - b_read);
                ret = copy_to_user(buf, seg->data + seg->pos, amount);
                b_read += (amount - ret);
                seg->pos += (amount - ret);
                seg->size -= (amount - ret);
                if (seg->size == 0) {
                        remove_list_item(seg->list);
                        free_data_segment(seg);
                }
                if (ret != 0) break;
                buf += (amount - ret);
        }
        sub_stream_len(b_read, dev, prio);
        add_free_space(b_read, dev, prio);
        return b_read;
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

        if (file->f_flags & O_NONBLOCK)
                mask |= GFP_ATOMIC;
        file->private_data = kzalloc(sizeof(struct session_data), mask);
        if (!file->private_data)
                return -ENOMEM;
#ifdef SINGLE_SESSION_OBJECT
        if (!mutex_trylock(&(dev->busy)))
                return -EBUSY;
#endif
        session = (struct session_data *)file->private_data;
        /* set defaults values for new session */
        session->current_priority = LOW_PRIORITY;
        session->timeout = 10 * HZ;     // 10 seconds

        pr_debug("%s: device file opened for object with [major,minor] number [%d,%d]\n",
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

        pr_debug("%s: device file closed for object with [major,minor] number [%d,%d]\n",
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

        idx = session->current_priority;
        pr_debug("%s: thread '%d' called a %s read on device with [major,minor] number [%d,%d]\n",
                 MODNAME, current->pid, get_priority_str(idx),
                 get_major(file), get_minor(file));

        if (count == 0)
                return 0;

        if (file->f_flags & O_NONBLOCK) {
                if (!mutex_trylock(&(dev->sync[idx])))  // busy
                        return -EAGAIN;
                if (get_stream_len(dev, idx) == 0) {    // the stream is empty
                        wake_up_interruptible(&(dev->waitq[idx]));
                        mutex_unlock(&(dev->sync[idx]));
                        return -EAGAIN;
                }
        } else {
                inc_waiting_threads(dev, idx);
                ret = wait_event_interruptible_timeout(dev->waitq[idx],
                                lock_and_check(get_stream_len(dev, idx) > 0,
                                               &(dev->sync[idx])),
                                session->timeout);
                if (ret == 0) {
                        dec_waiting_threads(dev, idx);
                        return -ETIME;
                }
                if (ret == -ERESTARTSYS) {
                        dec_waiting_threads(dev, idx);
                        return -EINTR;
                }
                dec_waiting_threads(dev, idx);
        }
        ret = actual_read(idx, get_minor(file), buf, count);
        wake_up_interruptible(&(dev->waitq[idx]));
        mutex_unlock(&(dev->sync[idx]));
        return ret;
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
        bool nonblock;
        struct data_segment *seg;
        struct packed_write *container;
        struct device_struct *dev = devices + get_minor(file);
        struct session_data *session = (struct session_data *)file->private_data;
        short idx = session->current_priority;
        gfp_t mask = GFP_KERNEL;

        pr_debug("%s: thread '%d' called a %s write on device with [major,minor] number [%d,%d]\n",
                MODNAME, current->pid, get_priority_str(idx), get_major(file), get_minor(file));

        if (count == 0)
                return 0;

        nonblock = file->f_flags & O_NONBLOCK;
        if (nonblock)
                mask |= GFP_ATOMIC;     // for non-blocking memory allocation

        /* Create new item of segment list */
        ret = alloc_data_segment(&seg, count, mask);
        if (ret < 0)
                return ret;

        if (idx == LOW_PRIORITY) {
                /* Create the work to enqueue */
                ret = alloc_packed_write(&container, mask);
                if (ret < 0) {
                        free_data_segment(seg);
                        return ret;
                }
                /* Fill struct packed_write */
                container->minor = get_minor(file);
                container->seg = seg;
                container->real_write = actual_write;
        }

        if (nonblock) {
                if (!mutex_trylock(&(dev->sync[idx]))) {
                        ret = -EBUSY;
                        goto free_mem_and_exit;
                }
                if (get_free_space(dev, idx) == 0) {
                        ret = -EAGAIN;
                        goto unlock_and_exit;
                }
        } else {
                ret = wait_event_interruptible_timeout(dev->waitq[idx],
                                lock_and_check(get_free_space(dev, idx) > 0,
                                               &(dev->sync[idx])),
                                session->timeout);
                if (ret == 0) {
                        ret = -ETIME;
                        goto free_mem_and_exit;
                }
                if (ret == -ERESTARTSYS) {
                        ret = -EINTR;
                        goto free_mem_and_exit;
                }
        }
        ret = get_free_space(dev, idx);
        if (ret < count)        // do partial write
                count = ret;

        /* Fill data segment */
        ret = copy_from_user(seg->data, buf, count);
        count -= ret;
        seg->size = count;
        if (count == 0) {
                ret = 0;
                goto unlock_and_exit;
        }

        /* Synchronous write */
        if (idx == HIGH_PRIORITY) {
                actual_write(idx, get_minor(file), seg);
                wake_up_interruptible(&(dev->waitq[idx]));
                mutex_unlock(&(dev->sync[idx]));
                return count;
        }
        /* Asynchronous write */
        ret = schedule_deferred_work(get_minor(file), &container->the_work);
        if (ret < 0)
                goto unlock_and_exit;
        sub_free_space(count, dev, idx);
        mutex_unlock(&(dev->sync[idx]));
        return count;

unlock_and_exit:
        wake_up_interruptible(&(dev->waitq[idx]));
        mutex_unlock(&(dev->sync[idx]));
free_mem_and_exit:
        free_data_segment(seg);
        if (idx == LOW_PRIORITY)
                free_packed_write(container);
        return ret;
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
 * the timeout interval in secs after setting it if @cmd is IOC_SET_WAIT_TIMEINT,
 * -EINVAL if @arg is not valid,
 * -ENOTTY if @cmd is not valid.
 */
static long mfd_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
        long __user *argp = (long __user *)arg;
        struct session_data *session = file->private_data;
        long ret = 0;

        pr_debug("%s: thread '%d' called an ioctl on device with [major,minor] number [%d,%d] and command '%d'\n",
               MODNAME, current->pid, get_major(file), get_minor(file), cmd);

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
                if (ret > MAX_WAIT_TIMEINT)
                        ret = MAX_WAIT_TIMEINT;         // avoid overflow
                /* Convert secs in jiffies */
                session->timeout = ret * HZ;
                ret = 0;
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
 * This function performs the needed initializations of the multi-flow
 * devices informations.
 * 
 * Note: by default, each device is equipped with two data streams one
 * for high priority and one for low priority operations, both with a
 * maximum size equal to MAX_STREAM_SIZE.
 * 
 * Returns 0 if initialization is successful, otherwise a negative value.
 */
static int multi_flow_dev_init(void)
{
        int i, j;
        char queue_name[WQ_NAME_LENGTH];
        struct list_head *head;
        struct list_head *tail;

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
                memset((void *)&(devices[i]), 0x0, sizeof(struct device_struct));
                devices[i].wr_workq = create_singlethread_workqueue(queue_name);
                if (!devices[i].wr_workq) {
                        if (i > 0)
                                goto revert_initialization;
                        return -ENOMEM;
                }
                for (j = 0; j < STREAMS_NUM; j++) {
                        init_waitqueue_head(&(devices[i].waitq[j]));
                        mutex_init(&(devices[i].sync[j]));
                        devices[i].free_b[j] = MAX_STREAM_SIZE;
                        head = &(devices[i].streams[j].head);
                        tail = &(devices[i].streams[j].tail);
                        INIT_LIST_HEAD(head);
                        INIT_LIST_HEAD(tail);
                        head->next = tail;
                        tail->prev = head;
                }
        }
        major = __register_chrdev(0, 0, MINORS, DEVICE_NAME, &fops);
        if (major < 0) {
                pr_err("%s: registering device failed\n", MODNAME);
                return major;
        }
        pr_info("%s: new device registered, it is assigned major number %d\n",
                MODNAME, major);
        return 0;

revert_initialization:
        for (; i >= 0; i--)
                destroy_workqueue(devices[i].wr_workq);
        return -ENOMEM;
}

/**
 * multi_flow_dev_cleanup - does the module cleanup
 */
static void multi_flow_dev_cleanup(void)
{
        int i, k;
        struct list_head *head;
        struct list_head *tail;
        struct list_head *pos;
        struct list_head *tmp;
        struct data_segment *seg;
        for (i = 0; i < MINORS; i++) {
                destroy_workqueue(devices[i].wr_workq);
                for (k = 0; k < STREAMS_NUM; k++) {
                        head = &(devices[i].streams[k].head);
                        tail = &(devices[i].streams[k].tail);
                        for (pos = head->next; pos != tail;) {
                                tmp = pos->next;
                                seg = list_entry(pos, struct data_segment, list);
                                free_data_segment(seg);
                                pos = tmp;
                        }
                }
        }
        __unregister_chrdev(major, 0, MINORS, DEVICE_NAME);
        pr_info("%s: new device unregistered, it was assigned major number %d\n",
                MODNAME, major);
        return;
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Cristiano Cuffaro <cristiano.cuffaro@outlook.com>");
MODULE_DESCRIPTION("Linux character device driver implementing low and high priority flows of data.");

module_init(multi_flow_dev_init);
module_exit(multi_flow_dev_cleanup);