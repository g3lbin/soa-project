
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
#include <linux/kprobes.h>

#define MODNAME "MULTI-FLOW DEV"
#define DEVICE_NAME "multi-flow-dev"

#define DEV_ENABLED (1)

#define WQ_NAME_LENGTH 24 // same as WQ_NAME_LEN which is not exported

#define MINORS 128

#define PRIORITIES (2)
#define LOW_PRIORITY (0)
#define HIGH_PRIORITY (1)

#define MAX_STREAM_SIZE (4096)          // just one page: 4KB
#define MAX_WAIT_TIMEINT (999999999L)   // represent infinite time in jiffies

#define IOC_MAGIC 'r' // https://www.kernel.org/doc/Documentation/ioctl/ioctl-number.txt
#define IOC_SWITCH_PRIORITY _IO(IOC_MAGIC, 0x20)
#define IOC_SWITCH_BLOCKING _IO(IOC_MAGIC, 0x21)
#define IOC_SET_WAIT_TIMEINT _IOW(IOC_MAGIC, 0x22, long *)

#define CHARP_ENTRY_SIZE 32             // number of bytes reserved for each entry of charp array parameters

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 0, 0)
#define get_major(session) MAJOR(session->f_inode->i_rdev)
#define get_minor(session) MINOR(session->f_inode->i_rdev)
#else
#define get_major(session) MAJOR(session->f_dentry->d_inode->i_rdev)
#define get_minor(session) MINOR(session->f_dentry->d_inode->i_rdev)
#endif

#define do_write(priority, dest, src, count)          \
    ({                                                \
        int __ret = 0;                                \
        if (priority == LOW_PRIORITY)                 \
        {                                             \
            memcpy(dest, src, count);                 \
        }                                             \
        else                                          \
        {                                             \
            __ret = copy_from_user(dest, src, count); \
        }                                             \
        __ret;                                        \
    })

#define mfd_module_param_array_named(name, array, type, nump, perm)		\
	param_check_##type(name, &((char **)array)[0]);				        \
	static const struct kparam_array __param_arr_##name		            \
	= { .max = ARRAY_SIZE(array), .num = nump,                          \
	    .ops = &mfd_param_ops_##type,					                \
	    .elemsize = sizeof(array[0]), .elem = array };		            \
	__module_param_call(MODULE_PARAM_PREFIX, name,			            \
			    &mfd_param_array_ops,				                    \
			    .arr = &__param_arr_##name,			                    \
			    perm, -1, 0);				                            \
	__MODULE_PARM_TYPE(name, "array of " #type)

typedef struct _device_struct
{
    wait_queue_head_t waitq[PRIORITIES];    // for blocking read and write operations
    struct workqueue_struct *wr_workq;      // for asynchronous execution of low priority write operations
    struct mutex sync[PRIORITIES];          // operation synchronizer of each stream
    short start[PRIORITIES];                // first valid byte of each stream
    atomic_t valid_bytes[PRIORITIES];       // valid bytes of each stream
    atomic_t actually_writable[PRIORITIES]; // free space to execute write operations
    char *streams[PRIORITIES];              // streams' addresses
    atomic_t waiting_threads[PRIORITIES];   // number of threads currently waiting for data along the two flows
} device_struct;

typedef struct _session_data
{
    short current_priority; // priority level for the operation
    long timeout;           // timeout in jiffies to break the wait
} session_data;

typedef struct _packed_write
{
    struct work_struct the_work;
    short major;
    short minor;
    short priority;
    char *buf;
    size_t count;
    ssize_t (*real_write)(short, int, const char *, size_t);
} packed_write;

static int dev_open(struct inode *, struct file *);
static int dev_release(struct inode *, struct file *);
static ssize_t dev_read(struct file *, char *, size_t, loff_t *);
static ssize_t dev_write(struct file *, const char *, size_t, loff_t *);
static long dev_ioctl(struct file *, unsigned int, unsigned long);
void deferred_write(unsigned long);
ssize_t actual_write(short, int, const char *, size_t);
ssize_t actual_read(short, int, char *, size_t);

static int major;                                           // major number assigned to the device driver
static device_struct devices[MINORS];
static int arr_size = MINORS;

/* module parameters hook */
static int mfd_param_get_charp(char *buffer, const struct kernel_param *kp)
{
    device_struct *dev;
    
    if (strncmp(kp->name, "bytes_present_on_devices_streams",
            strlen("bytes_present_on_devices_streams")) == 0) {
        dev = devices + (long)kp->arg;
        return scnprintf(buffer, PAGE_SIZE, "dev[%ld] - high:%d,low:%d\n", (long)kp->arg,
            atomic_read(&(dev->valid_bytes[HIGH_PRIORITY])),
            atomic_read(&(dev->valid_bytes[LOW_PRIORITY])));
    } else if (strncmp(kp->name, "waiting_threads_on_devices_streams",
            strlen("waiting_threads_on_devices_streams")) == 0) {
            dev = devices + (long)kp->arg;
        return scnprintf(buffer, PAGE_SIZE, "dev[%ld] - high:%d,low:%d\n", (long)kp->arg,
            atomic_read(&(dev->waiting_threads[HIGH_PRIORITY])),
            atomic_read(&(dev->waiting_threads[LOW_PRIORITY])));
    } else {
        return scnprintf(buffer, PAGE_SIZE, "%s\n", *((char **)kp->arg));
    }
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

static const struct kernel_param_ops mfd_param_array_ops = {
	.get = mfd_param_array_get,
};

static const struct kernel_param_ops mfd_param_ops_charp = 
{
    .get = &mfd_param_get_charp,
};


/* Module parameters */
static int device_status[MINORS];
static char bytes_present[MINORS][32];
static char waiting_threads[MINORS][32];

module_param_array(device_status, int, &arr_size, S_IRUGO | S_IWUSR);
mfd_module_param_array_named(bytes_present_on_devices_streams, bytes_present, charp, &arr_size, S_IRUGO);
mfd_module_param_array_named(waiting_threads_on_devices_streams, waiting_threads, charp, &arr_size, S_IRUGO);

/* Internal functions */

void deferred_write(unsigned long data)
{
    packed_write *wr_info;
    device_struct *dev;

    wr_info = (packed_write *)container_of((void *)data, packed_write, the_work);
    printk("%s: somebody called a low priority write on dev with [major,minor] number [%d,%d]\n",
           MODNAME, wr_info->major, wr_info->minor);

    dev = devices + wr_info->minor;

    mutex_lock(&(dev->sync[wr_info->priority]));
    (wr_info->real_write)(wr_info->priority, wr_info->minor, wr_info->buf, wr_info->count);
    wake_up_interruptible(&(dev->waitq[wr_info->priority]));
    mutex_unlock(&(dev->sync[wr_info->priority]));

    free_page((unsigned long)wr_info->buf);
    kfree((void *)wr_info);
    module_put(THIS_MODULE);
}

ssize_t actual_write(short priority, int minor, const char *buf, size_t count)
{
    short ret;
    short amount;
    int first_free_byte;
    device_struct *dev = devices + minor;

    first_free_byte = (dev->start[priority] + atomic_read(&(dev->valid_bytes[priority]))) % MAX_STREAM_SIZE;
    if (count > (MAX_STREAM_SIZE - first_free_byte))
    { // due to the circularity of the buffer
        amount = (MAX_STREAM_SIZE - first_free_byte);
        ret = do_write(priority, dev->streams[priority] + first_free_byte, buf, amount);
        first_free_byte = (first_free_byte + (amount - ret)) % MAX_STREAM_SIZE;
        atomic_add((amount - ret), &(dev->valid_bytes[priority]));
        if (priority == HIGH_PRIORITY)
            atomic_sub((amount - ret), &(dev->actually_writable[priority]));
        buf += (amount - ret);
        amount = (count - (amount - ret));
    }
    else
    {
        amount = count;
    }
    ret = do_write(priority, dev->streams[priority] + first_free_byte, buf, amount);
    atomic_add((amount - ret), &(dev->valid_bytes[priority]));
    if (priority == HIGH_PRIORITY)
        atomic_sub((amount - ret), &(dev->actually_writable[priority]));

    return (count - ret);
}

ssize_t actual_read(short priority, int minor, char *buf, size_t count)
{
    short ret;
    short amount;
    device_struct *dev = devices + minor;

    if ((ret = (short)atomic_read(&(dev->valid_bytes[priority]))) < count)
        count = ret;

    if (count > (MAX_STREAM_SIZE - dev->start[priority]))
    { // due to the circularity of the buffer
        amount = (MAX_STREAM_SIZE - dev->start[priority]);
        ret = copy_to_user(buf, dev->streams[priority] + dev->start[priority], amount);
        dev->start[priority] = (dev->start[priority] + (amount - ret)) % MAX_STREAM_SIZE;
        atomic_sub((amount - ret), &(dev->valid_bytes[priority]));
        atomic_add((amount - ret), &(dev->actually_writable[priority]));
        buf += (amount - ret);
        amount = (count - (amount - ret));
    }
    else
    {
        amount = count;
    }
    ret = copy_to_user(buf, dev->streams[priority] + dev->start[priority], amount);
    dev->start[priority] = (dev->start[priority] + (amount - ret)) % MAX_STREAM_SIZE;
    atomic_sub((amount - ret), &(dev->valid_bytes[priority]));
    atomic_add((amount - ret), &(dev->actually_writable[priority]));

    return (count - ret);
}

/* the actual driver */

static int dev_open(struct inode *inode, struct file *file)
{
    device_struct *dev;
    int minor = get_minor(file);
    gfp_t mask = GFP_KERNEL;

    if (minor >= MINORS)
        return -ENODEV;

    if (!device_status[minor])
        return -EINVAL;

    dev = devices + minor;

    if (file->f_flags & O_NONBLOCK)
        mask |= GFP_ATOMIC;

    file->private_data = kzalloc(sizeof(session_data), mask);
    if (!file->private_data)
    {
        return -ENOMEM;
    }
    /* set defaults values for new session */
    ((session_data *)file->private_data)->current_priority = LOW_PRIORITY;
    ((session_data *)file->private_data)->timeout = MAX_WAIT_TIMEINT; // (DA VERIFICARE) no timeout

    if (!try_module_get(THIS_MODULE))
        return -ENODEV;

    // INIZIO DA RIMUOVERE
    if (file->f_flags & O_NONBLOCK)
        printk("%s: device file successfully opened with flag O_NONBLOCK for object with minor %d\n", MODNAME, minor);
    else
        printk("%s: device file successfully opened for object with minor %d\n", MODNAME, minor);
    // FINE DA RIMUOVERE

    return 0;
}

static int dev_release(struct inode *inode, struct file *file)
{
    int minor;
    minor = get_minor(file);

    kfree(file->private_data);

    module_put(THIS_MODULE);
    printk("%s: device file closed for object with minor %d\n", MODNAME, minor);

    return 0;
}

ssize_t dev_read(struct file *file, char *buf, size_t count, loff_t *pos)
{
    long ret;
    short idx;
    device_struct *dev;

    dev = devices + get_minor(file);
    printk("%s: somebody called a read on dev with [major,minor] number [%d,%d]\n", MODNAME, get_major(file), get_minor(file));

    idx = ((session_data *)file->private_data)->current_priority;

    if (file->f_flags & O_NONBLOCK)
    {
        if (!mutex_trylock(&(dev->sync[idx])))
            return -EAGAIN;
        if (atomic_read(&(dev->valid_bytes[idx])) == 0)
        {
            mutex_unlock(&(dev->sync[idx]));
            return -EAGAIN;
        }
    }
    else
    {
        atomic_inc(&(dev->waiting_threads[idx]));
    retry_read:
        ret = wait_event_interruptible_timeout(dev->waitq[idx],
                atomic_read(&(dev->valid_bytes[idx])) > 0,
                ((session_data *)file->private_data)->timeout);
        printk("%s: reader returned on runqueue\n", MODNAME);
        if (ret == 0) {
            return -ETIME;
        }
        if (ret == -ERESTARTSYS) {
            return -EINTR;
        }
        if (!mutex_trylock(&(dev->sync[idx])))
            goto retry_read;
        if (atomic_read(&(dev->valid_bytes[idx])) == 0) {
            mutex_unlock(&(dev->sync[idx]));
            goto retry_read;                    // someone has read the bytes before you
        }
    }
    atomic_dec(&(dev->waiting_threads[idx]));
    ret = actual_read(idx, get_minor(file), buf, count);

    wake_up_interruptible(&(dev->waitq[idx]));
    mutex_unlock(&(dev->sync[idx]));

    return (ssize_t)ret;
}

ssize_t dev_write(struct file *file, const char *buf, size_t count, loff_t *pos)
{
    short ret;
    short idx;
    packed_write *work_container;
    device_struct *dev;
    gfp_t mask = GFP_KERNEL;

    dev = devices + get_minor(file);
    idx = ((session_data *)file->private_data)->current_priority;

    if (file->f_flags & O_NONBLOCK)
    {
        if (!mutex_trylock(&(dev->sync[idx])))
            return -EAGAIN;
        if (atomic_read(&(dev->actually_writable[idx])) == 0)
        { // the stream is full
            mutex_unlock(&(dev->sync[idx]));
            return -EAGAIN;
        }
    }
    else
    {
    retry_write:
        printk("%s: writer (%d) before wait queue API\n", MODNAME, current->pid);
        ret = wait_event_interruptible_timeout(dev->waitq[idx],
                                               atomic_read(&(dev->actually_writable[idx])) > 0,
                                               ((session_data *)file->private_data)->timeout);
        printk("%s: writer (%d) returned on runqueue\n", MODNAME, current->pid);
        if (ret == 0) {
            return -ETIME;
        }
        if (ret == -ERESTARTSYS) {
            return -EINTR;
        }
        if (!mutex_trylock(&(dev->sync[idx])))
            goto retry_write;
        if (atomic_read(&(dev->actually_writable[idx])) == 0) {
            mutex_unlock(&(dev->sync[idx]));
            goto retry_write; // someone has write before you
        }
    }

    if (atomic_read(&(dev->actually_writable[idx])) < count) // do partial write
        count = atomic_read(&(dev->actually_writable[idx]));

    if (idx == HIGH_PRIORITY)
    {
        printk("%s: somebody called an high priority write on dev with [major,minor] number [%d,%d]\n", MODNAME, get_major(file), get_minor(file));
        ret = actual_write(idx, get_minor(file), buf, count);
        wake_up_interruptible(&(dev->waitq[idx]));
        mutex_unlock(&(dev->sync[idx]));

        return ret;
    }

    if (!try_module_get(THIS_MODULE))
    {
        mutex_unlock(&(dev->sync[idx]));
        return -ENODEV;
    }

    if (file->f_flags & O_NONBLOCK)
        mask |= GFP_ATOMIC; // non blocking memory allocation

    work_container = (packed_write *)kzalloc(sizeof(packed_write), mask);
    if (!work_container)
    {
        mutex_unlock(&(dev->sync[idx]));
        module_put(THIS_MODULE);
        return -ENOMEM;
    }

    work_container->major = get_major(file);
    work_container->minor = get_minor(file);
    work_container->priority = idx;
    work_container->buf = (char *)get_zeroed_page(mask);
    if (!work_container->buf)
    {
        kfree((void *)work_container);
        mutex_unlock(&(dev->sync[idx]));
        module_put(THIS_MODULE);
        return -ENOMEM;
    }
    if (count > MAX_STREAM_SIZE)
        count = MAX_STREAM_SIZE;
    ret = copy_from_user(work_container->buf, buf, count);

    work_container->count = (count - ret);
    work_container->real_write = actual_write;

    __INIT_WORK(&(work_container->the_work), (void *)deferred_write, (unsigned long)(&(work_container->the_work)));
    queue_work(dev->wr_workq, &work_container->the_work);
    atomic_sub((count - ret), &(dev->actually_writable[idx]));

    mutex_unlock(&(dev->sync[idx]));

    return (count - ret);
}

static long dev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    long __user *argp = (long __user *)arg;
    long ret;

    printk("%s: somebody called a ioctl on dev with [major,minor] number [%d,%d] and command %d\n", MODNAME, get_major(file), get_minor(file), cmd);
    printk("%s: %d\n", MODNAME, IOC_SWITCH_BLOCKING);

    switch (cmd)
    {
    case IOC_SWITCH_PRIORITY:
        if (((session_data *)file->private_data)->current_priority == LOW_PRIORITY)
            ret = ((session_data *)file->private_data)->current_priority = HIGH_PRIORITY;
        else
            ret = ((session_data *)file->private_data)->current_priority = LOW_PRIORITY;
        break;
    case IOC_SWITCH_BLOCKING:
        if (file->f_flags & O_NONBLOCK)
            file->f_flags ^= O_NONBLOCK;
        else
            file->f_flags |= O_NONBLOCK;
        ret = 0;
        break;
    case IOC_SET_WAIT_TIMEINT:
        if (copy_from_user(&ret, argp, sizeof(ret)) != 0)
            return -EBADTYPE; // the argument is not a long
        if (ret <= 0)
            return -EINVAL; // invalid argument
        ret *= HZ;
        if (ret > MAX_WAIT_TIMEINT)
            ((session_data *)file->private_data)->timeout = ret = MAX_WAIT_TIMEINT;
        else
            ((session_data *)file->private_data)->timeout = ret;
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
    .unlocked_ioctl = dev_ioctl};

int multi_flow_dev_init(void)
{
    int i, j, k;
    char queue_name[WQ_NAME_LENGTH];

    // initialize the drive internal state
    for (i = 0; i < MINORS; i++)
    {
        device_status[i] = DEV_ENABLED;
        snprintf(queue_name, sizeof(queue_name), "%s-%d", DEVICE_NAME, i);
        devices[i].wr_workq = create_singlethread_workqueue(queue_name);
        if (!devices[i].wr_workq)
        {
            if (i > 0)
            {
                j = 0;
                goto revert_allocation;
            }
            return -ENOMEM;
        }
        for (j = 0; j < PRIORITIES; j++)
        {
            init_waitqueue_head(&(devices[i].waitq[j]));
            mutex_init(&(devices[i].sync[j]));
            devices[i].start[j] = 0;
            atomic_set(&(devices[i].valid_bytes[j]), 0);
            atomic_set(&(devices[i].actually_writable[j]), MAX_STREAM_SIZE);
            atomic_set(&(devices[i].waiting_threads[j]), 0);
            devices[i].streams[j] = NULL;
            devices[i].streams[j] = (char *)get_zeroed_page(GFP_KERNEL);
            if (devices[i].streams[j] == NULL)
                goto revert_allocation;
        }
    }

    major = __register_chrdev(0, 0, 256, DEVICE_NAME, &fops);

    if (major < 0)
    {
        printk("%s: registering device failed\n", MODNAME);
        return major;
    }
    printk(KERN_INFO "%s: new device registered, it is assigned major number %d\n", MODNAME, major);

    return 0;

revert_allocation:
    for (; i >= 0; i--)
    {
        destroy_workqueue(devices[i].wr_workq);
        for (k = 0; k < PRIORITIES; k++)
        {
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
    for (i = 0; i < MINORS; i++)
    {
        destroy_workqueue(devices[i].wr_workq);
        for (k = 0; k < PRIORITIES; k++)
        {
            free_page((unsigned long)devices[i].streams[k]);
        }
    }

    unregister_chrdev(major, DEVICE_NAME);

    printk(KERN_INFO "%s: new device unregistered, it was assigned major number %d\n", MODNAME, major);

    return;
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Cristiano Cuffaro <cristiano.cuffaro@outlook.com>");
MODULE_DESCRIPTION("Linux character device driver implementing low and high priority flows of data.");

module_init(multi_flow_dev_init);
module_exit(multi_flow_dev_cleanup);