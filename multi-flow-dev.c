
#define EXPORT_SYMTAB
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/sched.h>        
#include <linux/pid.h>          /* For pid types */
#include <linux/version.h>      /* For LINUX_VERSION_CODE */
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/workqueue.h>
#include <linux/string.h>
#include <linux/wait.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Cristiano Cuffaro");

#define MODNAME "MULTI-FLOW DEV"
#define DEVICE_NAME "multi-flow-dev"

#define WQ_NAME_LENGTH 24       // same as WQ_NAME_LEN which is not exported

#define MINORS 128

#define PRIORITIES (2)
#define LOW_PRIORITY (0)
#define HIGH_PRIORITY (1)

#define MAX_STREAM_SIZE  (4096)         // just one page: 4KB

enum {
    SWITCH_PRIORITY_IOCTL = 0,
    SWITCH_BLOCKING_IOCTL,
};

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 0, 0)
#define get_major(session)      MAJOR(session->f_inode->i_rdev)
#define get_minor(session)      MINOR(session->f_inode->i_rdev)
#else
#define get_major(session)      MAJOR(session->f_dentry->d_inode->i_rdev)
#define get_minor(session)      MINOR(session->f_dentry->d_inode->i_rdev)
#endif

#define do_write(priority, dest, src, count, ret)       \
    do {                                                \
        if (priority == LOW_PRIORITY) {                 \
            memcpy(dest, src, count);                   \
            ret = 0;                                    \
        } else {                                        \
            ret = copy_from_user(dest, src, count);     \
        }                                               \
    } while (0)

typedef struct _device_state {
    wait_queue_head_t waitq[PRIORITIES];        // for blocking read and write operations
    struct workqueue_struct *wr_workq;          // for asynchronous execution of low priority write operations
    struct mutex sync[PRIORITIES];              // operation synchronizer of each stream
    short start[PRIORITIES];                    // first valid byte of each stream
    short valid_bytes[PRIORITIES];              // valid bytes of each stream
    char *streams[PRIORITIES];                  // streams' addresses
} device_state;

typedef struct _session_data {
    short current_priority;                     // priority level for the operation
    long timeout;                               // timeout in jiffies to break the wait
    bool read_residual;                         // to distinguish the first attempt to read from the following
    bool write_residual;                        // to distinguish the first attempt to write from the following
} session_data;

typedef struct _packed_write {
    struct work_struct the_work;
    short major;
    short minor;
    short priority;
    char *buf;
    size_t count;
    ssize_t (*real_write)(short, device_state *, const char *, size_t);
} packed_write;

static int dev_open(struct inode *, struct file *);
static int dev_release(struct inode *, struct file *);
static ssize_t dev_read(struct file *, char *, size_t, loff_t *);
static ssize_t dev_write(struct file *, const char *, size_t, loff_t *);
static long dev_ioctl(struct file *, unsigned int, unsigned long);
void deferred_write(unsigned long);
ssize_t actual_write(short, device_state *, const char *, size_t);

static int Major;                       // major number assigned to broadcast device driver
device_state devices[MINORS];

/* internal functions */

void deferred_write(unsigned long data)
{
    packed_write *wr_info;
    device_state *dev;

    wr_info = (packed_write *)container_of((void *)data, packed_write, the_work);
    printk("%s: somebody called a low priority write on dev with [major,minor] number [%d,%d]\n",
        MODNAME, wr_info->major, wr_info->minor);

    dev = devices + wr_info->minor;

    mutex_lock(&(dev->sync[wr_info->priority]));
    (wr_info->real_write)(wr_info->priority, dev, wr_info->buf, wr_info->count);
    wake_up_interruptible(&(dev->waitq[wr_info->priority]));
    mutex_unlock(&(dev->sync[wr_info->priority]));

    free_page((unsigned long)wr_info->buf);
    kfree((void *)wr_info);
    module_put(THIS_MODULE);
}

ssize_t actual_write(short priority, device_state *dev, const char *buf, size_t count)
{
    short ret;
    short amount;
    short first_free_byte;
    
    first_free_byte = (dev->start[priority] + dev->valid_bytes[priority]) % MAX_STREAM_SIZE;
    if (count > (MAX_STREAM_SIZE - first_free_byte)) {      // due to the circularity of the buffer
        amount = (MAX_STREAM_SIZE - first_free_byte);
        do_write(priority, dev->streams[priority] + first_free_byte, buf, amount, ret);
        first_free_byte = (first_free_byte + (amount - ret)) % MAX_STREAM_SIZE;
        dev->valid_bytes[priority] += (amount - ret);
        buf += (amount - ret);
        amount = (count - (amount - ret));
    } else {
        amount = count;
    }
    do_write(priority, dev->streams[priority] + first_free_byte, buf, amount, ret);
    dev->valid_bytes[priority] += (amount - ret);

    return (count - ret);
}

ssize_t actual_read(short priority, device_state *dev, char *buf, size_t count)
{
    short ret;
    short amount;

    if (dev->valid_bytes[priority] < count)
        count = dev->valid_bytes[priority];

    if (count > (MAX_STREAM_SIZE - dev->start[priority])) {  // due to the circularity of the buffer
        amount = (MAX_STREAM_SIZE - dev->start[priority]);
        ret = copy_to_user(buf, dev->streams[priority] + dev->start[priority], amount);
        dev->start[priority] = (dev->start[priority] + (amount - ret)) % MAX_STREAM_SIZE;
        dev->valid_bytes[priority] -= (amount - ret);
        buf += (amount - ret);
        amount = (count - (amount - ret));
    } else {
        amount = count;
    }
    ret = copy_to_user(buf, dev->streams[priority] + dev->start[priority], amount);
    dev->start[priority] = (dev->start[priority] + (amount - ret)) % MAX_STREAM_SIZE;
    dev->valid_bytes[priority] -= (amount - ret);

    return (count - ret);
}

/* the actual driver */

static int dev_open(struct inode *inode, struct file *file)
{
    device_state *dev;
    int minor = get_minor(file);
    gfp_t mask = GFP_KERNEL;

    if (minor >= MINORS)
        return -ENODEV;

    dev = devices + minor;

    if (file->f_flags & O_NONBLOCK)
        mask |= GFP_ATOMIC;

    file->private_data = kzalloc(sizeof(session_data), mask);
	if (!file->private_data) {
		return -ENOMEM;
	}
    /* set defaults values for new session */
    ((session_data *)file->private_data)->current_priority = LOW_PRIORITY;
    ((session_data *)file->private_data)->timeout = 999999999;      // (DA CAMBIARE) no timeout
    ((session_data *)file->private_data)->read_residual = false;    // first attempt to read
    ((session_data *)file->private_data)->write_residual = false;   // first attempt to write

    if(!try_module_get(THIS_MODULE))
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
    device_state *dev;
    
    dev = devices + get_minor(file);
    printk("%s: somebody called a read on dev with [major,minor] number [%d,%d]\n",MODNAME,get_major(file),get_minor(file));

    idx = ((session_data *)file->private_data)->current_priority;

    if (file->f_flags & O_NONBLOCK) {
        if (!mutex_trylock(&(dev->sync[idx])))
            return -EAGAIN;
        if (dev->valid_bytes[idx] == 0) {
            mutex_unlock(&(dev->sync[idx]));
            return -EAGAIN;
        }
    } else {
retry_read:
        ret = wait_event_interruptible_timeout(dev->waitq[idx],
                dev->valid_bytes[idx] > 0 || ((session_data *)file->private_data)->read_residual,
                ((session_data *)file->private_data)->timeout);
        printk("%s: reader returned on runqueue with residual value: %d\n", MODNAME, ((session_data *)file->private_data)->read_residual);
        if (ret == 0) {
            return -ETIME;
        }
        if (ret == -ERESTARTSYS) {
            return -EINTR;
        }
        if (!mutex_trylock(&(dev->sync[idx])))
            goto retry_read;
        if (dev->valid_bytes[idx] == 0) {
            mutex_unlock(&(dev->sync[idx]));
            if (((session_data *)file->private_data)->read_residual) {
                ((session_data *)file->private_data)->read_residual = false;
                return 0;           // no bytes left on current device stream
            }
            goto retry_read;        // someone has read the bytes before you
        }
        ((session_data *)file->private_data)->read_residual = false;
    }
    
    ret = actual_read(idx, dev, buf, count);
    if (ret < count)
        ((session_data *)file->private_data)->read_residual = true;

    wake_up_interruptible(&(dev->waitq[idx]));
    mutex_unlock(&(dev->sync[idx]));

    return (ssize_t)ret;
}

ssize_t dev_write(struct file *file, const char *buf, size_t count, loff_t *pos)
{
    short ret;
    short idx;
    packed_write *work_container;
    device_state *dev;
    gfp_t mask = GFP_KERNEL;
    
    dev = devices + get_minor(file);
    idx = ((session_data *)file->private_data)->current_priority;

    if (file->f_flags & O_NONBLOCK) {
        if (!mutex_trylock(&(dev->sync[idx])))
            return -EAGAIN;
        if (dev->valid_bytes[idx] == MAX_STREAM_SIZE) {            // the stream is full 
            mutex_unlock(&(dev->sync[idx]));
            return -EAGAIN;
        }
    } else {
retry_write:
        ret = wait_event_interruptible_timeout(dev->waitq[idx],
                dev->valid_bytes[idx] < MAX_STREAM_SIZE || ((session_data *)file->private_data)->write_residual,
                ((session_data *)file->private_data)->timeout);
        printk("%s: writer returned on runqueue with residual value: %d\n", MODNAME, ((session_data *)file->private_data)->write_residual);
        if (ret == 0) {
            return -ETIME;
        }
        if (ret == -ERESTARTSYS) {
            return -EINTR;
        }
        if (!mutex_trylock(&(dev->sync[idx])))
            goto retry_write;
        if (dev->valid_bytes[idx] == MAX_STREAM_SIZE) {
            mutex_unlock(&(dev->sync[idx]));
            if (((session_data *)file->private_data)->write_residual) {
                ((session_data *)file->private_data)->write_residual = false;
                return 0;           // no bytes left on current device stream
            }
            goto retry_write;       // someone has write before you
        }
        ((session_data *)file->private_data)->write_residual = false;
    }
    
    if ((MAX_STREAM_SIZE - dev->valid_bytes[idx]) < count)      // do partial write
        count = (MAX_STREAM_SIZE - dev->valid_bytes[idx]);

    if (idx == HIGH_PRIORITY) {
        printk("%s: somebody called an high priority write on dev with [major,minor] number [%d,%d]\n",MODNAME,get_major(file),get_minor(file));
        ret = actual_write(idx, dev, buf, count);
        wake_up_interruptible(&(dev->waitq[idx]));
        mutex_unlock(&(dev->sync[idx]));
        
        return ret;
    }
    
    if(!try_module_get(THIS_MODULE)) {
        mutex_unlock(&(dev->sync[idx]));
        return -ENODEV;
    }

    if (file->f_flags & O_NONBLOCK)
        mask |= GFP_ATOMIC;                 // non blocking memory allocation

    work_container = (packed_write *)kzalloc(sizeof(packed_write), mask);
    if (!work_container) {
        mutex_unlock(&(dev->sync[idx]));
        module_put(THIS_MODULE);
        return -ENOMEM;
    }

    work_container->major = get_major(file);
    work_container->minor = get_minor(file);
    work_container->priority = idx;
    work_container->buf = (char *)get_zeroed_page(mask);
    if (!work_container->buf) {
        kfree((void *)work_container);
        mutex_unlock(&(dev->sync[idx]));
        module_put(THIS_MODULE);
        return -ENOMEM;
    }
    if (count > MAX_STREAM_SIZE)
        count = MAX_STREAM_SIZE;
    ret = copy_from_user(work_container->buf, buf, count);

    work_container->count = count - ret;
    work_container->real_write = actual_write;
    
    __INIT_WORK(&(work_container->the_work), (void *)deferred_write, (unsigned long)(&(work_container->the_work)));
    queue_work(dev->wr_workq, &work_container->the_work);
    mutex_unlock(&(dev->sync[idx]));

    return (count - ret);
}

static long dev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    long ret;

    printk("%s: somebody called a ioctl on dev with [major,minor] number [%d,%d] and command %d\n",MODNAME,get_major(file),get_minor(file),cmd);

    switch (cmd)
    {
    case SWITCH_PRIORITY_IOCTL:
        if (((session_data *)file->private_data)->current_priority == LOW_PRIORITY)
            ret = ((session_data *)file->private_data)->current_priority = HIGH_PRIORITY;
        else
            ret = ((session_data *)file->private_data)->current_priority = LOW_PRIORITY;
        break;
    case SWITCH_BLOCKING_IOCTL:
        if (file->f_flags & O_NONBLOCK)
            file->f_flags ^= O_NONBLOCK;
        else
            file->f_flags |= O_NONBLOCK;
        ret = 0;
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
    .open =  dev_open,
    .release = dev_release,
    .unlocked_ioctl = dev_ioctl
};

int multi_flow_dev_init(void)
{
    int i, j, k;
    char queue_name[WQ_NAME_LENGTH];

    // initialize the drive internal state
    for (i = 0; i < MINORS; i++) {
        snprintf(queue_name, sizeof(queue_name), "%s-%d", DEVICE_NAME, i);
        devices[i].wr_workq = create_singlethread_workqueue(queue_name);
        if (!devices[i].wr_workq) {
            if (i > 0) {
                j = 0;
                goto revert_allocation;
            }
            return -ENOMEM;
        }
        for (j = 0; j < PRIORITIES; j++) {
            init_waitqueue_head(&(devices[i].waitq[j]));
            mutex_init(&(devices[i].sync[j]));
            devices[i].start[j] = 0;
            devices[i].valid_bytes[j] = 0;
            devices[i].streams[j] = NULL;
            devices[i].streams[j] = (char *)get_zeroed_page(GFP_KERNEL);
            if(devices[i].streams[j] == NULL)
                goto revert_allocation;
        }
    }

    Major = __register_chrdev(0, 0, 256, DEVICE_NAME, &fops);

    if (Major < 0) {
        printk("%s: registering device failed\n",MODNAME);
        return Major;
    }
    printk(KERN_INFO "%s: new device registered, it is assigned major number %d\n",MODNAME, Major);

    return 0;

revert_allocation:
        for (; i >= 0; i--){
            destroy_workqueue(devices[i].wr_workq);
            for (k = 0; k < PRIORITIES; k++) {
                free_page((unsigned long)devices[i].streams[k]);
                if (i == 0 && k == j)
                    break;
            }
        }
        return -ENOMEM;
}

void multi_flow_dev_cleanup(void) {

    int i, k;
    for(i = 0; i < MINORS; i++) {
        destroy_workqueue(devices[i].wr_workq);
        for (k = 0; k < PRIORITIES; k++) {
                free_page((unsigned long)devices[i].streams[k]);
            }
    }

    unregister_chrdev(Major, DEVICE_NAME);

    printk(KERN_INFO "%s: new device unregistered, it was assigned major number %d\n",MODNAME, Major);

    return;
}

module_init(multi_flow_dev_init);
module_exit(multi_flow_dev_cleanup);