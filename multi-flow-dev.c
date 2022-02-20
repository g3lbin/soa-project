
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

#define SWITCH_PRIORITY_IOCTL (0)

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 0, 0)
#define get_major(session)      MAJOR(session->f_inode->i_rdev)
#define get_minor(session)      MINOR(session->f_inode->i_rdev)
#else
#define get_major(session)      MAJOR(session->f_dentry->d_inode->i_rdev)
#define get_minor(session)      MINOR(session->f_dentry->d_inode->i_rdev)
#endif

#define do_write(priority, dest, src, count, ret) do    \
    {   if (priority == LOW_PRIORITY) {                 \
            memcpy(dest, src, count);                   \
            ret = 0;                                    \
        } else {                                        \
            ret = copy_from_user(dest, src, count);     \
        }                                               \
    } while(0)

typedef struct _device_state {
    struct workqueue_struct *queue;     // for asynchronous execution of low priority write operations
    struct mutex sync[PRIORITIES];      // operation synchronizer of each stream
    short start[PRIORITIES];            // first valid byte of each stream
    short valid_bytes[PRIORITIES];      // valid bytes of each stream
    char *streams[PRIORITIES];          // streams' addresses
} device_state;

typedef struct _device_data {
    short current_priority;             // priority level for the operation
} device_data;

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
    
    wr_info = (packed_write *)container_of((void *)data, packed_write, the_work);
    printk("%s: somebody called a low priority write on dev with [major,minor] number [%d,%d]\n",
        MODNAME, wr_info->major, wr_info->minor);
    (wr_info->real_write)(wr_info->priority, devices + wr_info->minor, wr_info->buf, wr_info->count);
    
    free_page((unsigned long)wr_info->buf);
    kfree((void *)wr_info);
    module_put(THIS_MODULE);
}

ssize_t actual_write(short priority, device_state *dev, const char *buf, size_t count)
{
    short ret;
    short amount;
    short first_free_byte;

    mutex_lock(&(dev->sync[priority]));

    if(dev->valid_bytes[priority] == MAX_STREAM_SIZE) {          // the stream is full
        mutex_unlock(&(dev->sync[priority]));
        return -ENOSPC; // no space left on device
    }
    
    if ((MAX_STREAM_SIZE - dev->valid_bytes[priority]) < count)  // write only in the free space
        count = (MAX_STREAM_SIZE - dev->valid_bytes[priority]);
    
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

    mutex_unlock(&(dev->sync[priority]));

    return (count - ret);
}

/* the actual driver */

static int dev_open(struct inode *inode, struct file *file)
{
    device_state *dev;
    int minor = get_minor(file);

    if (minor >= MINORS)
        return -ENODEV;

    dev = devices + minor;

    file->private_data = kzalloc(sizeof(device_data), GFP_KERNEL);
	if (!file->private_data) {
		return -ENOMEM;
	}
    ((device_data *)file->private_data)->current_priority = LOW_PRIORITY;   // default for new open

    if(!try_module_get(THIS_MODULE))
        return -ENODEV;

    printk("%s: device file successfully opened for object with minor %d\n", MODNAME, minor);

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
    short ret;
    short idx;
    short amount;
    device_state *dev;
    
    dev = devices + get_minor(file);
    printk("%s: somebody called a read on dev with [major,minor] number [%d,%d]\n",MODNAME,get_major(file),get_minor(file));

    idx = ((device_data *)file->private_data)->current_priority;

    mutex_lock(&(dev->sync[idx]));
    
    if (dev->valid_bytes[idx] < count)
        count = dev->valid_bytes[idx];

    if (count > (MAX_STREAM_SIZE - dev->start[idx])) {  // due to the circularity of the buffer
        amount = (MAX_STREAM_SIZE - dev->start[idx]);
        ret = copy_to_user(buf, dev->streams[idx] + dev->start[idx], amount);
        dev->start[idx] = (dev->start[idx] + (amount - ret)) % MAX_STREAM_SIZE;
        dev->valid_bytes[idx] -= (amount - ret);
        buf += (amount - ret);
        amount = (count - (amount - ret));
    } else {
        amount = count;
    }
    ret = copy_to_user(buf, dev->streams[idx] + dev->start[idx], amount);
    dev->start[idx] = (dev->start[idx] + (amount - ret)) % MAX_STREAM_SIZE;
    dev->valid_bytes[idx] -= (amount - ret);

    mutex_unlock(&(dev->sync[idx]));

    return (count - ret);
}

ssize_t dev_write(struct file *file, const char *buf, size_t count, loff_t *pos)
{
    short ret;
    short priority;
    packed_write *work_container;
    
    priority = ((device_data *)file->private_data)->current_priority;
    if (priority == HIGH_PRIORITY) {
        printk("%s: somebody called an high priority write on dev with [major,minor] number [%d,%d]\n",MODNAME,get_major(file),get_minor(file));
        return actual_write(priority, devices + get_minor(file), buf, count);
    }
    
    if(!try_module_get(THIS_MODULE))
        return -ENODEV;

    work_container = (packed_write *)kzalloc(sizeof(packed_write), GFP_ATOMIC);     // non blocking memory allocation
    if (!work_container) {
        module_put(THIS_MODULE);
        return -ENOMEM;
    }

    work_container->major = get_major(file);
    work_container->minor = get_minor(file);
    work_container->priority = priority;
    work_container->buf = (char *)get_zeroed_page(GFP_KERNEL | GFP_ATOMIC);
    if (!work_container->buf) {
        kfree((void *)work_container);
        module_put(THIS_MODULE);
        return -ENOMEM;
    }
    if (count > MAX_STREAM_SIZE)
        count = MAX_STREAM_SIZE;
    ret = copy_from_user(work_container->buf, buf, count);
    work_container->count = count - ret;
    work_container->real_write = actual_write;
    
    __INIT_WORK(&(work_container->the_work), (void *)deferred_write, (unsigned long)(&(work_container->the_work)));
    queue_work((devices + get_minor(file))->queue, &work_container->the_work);

    return (count - ret);
}

static long dev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    long ret;

    printk("%s: somebody called a ioctl on dev with [major,minor] number [%d,%d] and command %d\n",MODNAME,get_major(file),get_minor(file),cmd);

    switch (cmd)
    {
    case SWITCH_PRIORITY_IOCTL:
        if (((device_data *)file->private_data)->current_priority == LOW_PRIORITY)
            ret = ((device_data *)file->private_data)->current_priority = HIGH_PRIORITY;
        else
            ret = ((device_data *)file->private_data)->current_priority = LOW_PRIORITY;
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

int multi_flow_init(void)
{
    int i, j, k;
    char queue_name[WQ_NAME_LENGTH];

    // initialize the drive internal state
    for (i = 0; i < MINORS; i++) {
        snprintf(queue_name, sizeof(queue_name), "%s-%d", DEVICE_NAME, i);
        devices[i].queue = create_singlethread_workqueue(queue_name);
        if (!devices[i].queue) {
            if (i > 0) {
                j = 0;
                goto revert_allocation;
            }
            return -ENOMEM;
        }
        for (j = 0; j < PRIORITIES; j++) {
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
            for (k = 0; k < PRIORITIES; k++) {
                free_page((unsigned long)devices[i].streams[k]);
                if (i == 0 && k == j)
                    break;
            }
        }
        return -ENOMEM;
}

void multi_flow_cleanup(void) {

    int i, k;
    for(i = 0; i < MINORS; i++) {
        destroy_workqueue(devices[i].queue);
        for (k = 0; k < PRIORITIES; k++) {
                free_page((unsigned long)devices[i].streams[k]);
            }
    }

    unregister_chrdev(Major, DEVICE_NAME);

    printk(KERN_INFO "%s: new device unregistered, it was assigned major number %d\n",MODNAME, Major);

    return;
}

module_init(multi_flow_init);
module_exit(multi_flow_cleanup);