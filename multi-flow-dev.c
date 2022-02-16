
#define EXPORT_SYMTAB
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/sched.h>        
#include <linux/pid.h>          /* For pid types */
#include <linux/version.h>      /* For LINUX_VERSION_CODE */


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Cristiano Cuffaro");

#define MODNAME "MULTI-FLOW DEV"
#define DEVICE_NAME "multi-flow-dev"

#define LOW_PRIORITY (0)
#define HIGH_PRIORITY (1)

#define MFIOC_CHANGE_PRIORITY (0)

#define SWAP(x, y, type) do { type SWAP = x; x = y; y = SWAP; } while (0)

static int dev_open(struct inode *, struct file *);
static int dev_release(struct inode *, struct file *);
static ssize_t dev_read(struct file *, char *, size_t, loff_t *);
static ssize_t dev_write(struct file *, const char *, size_t, loff_t *);
static long dev_ioctl(struct file *, unsigned int, unsigned long);

static int Major;            /* Major number assigned to broadcast device driver */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 0, 0)
#define get_major(session)      MAJOR(session->f_inode->i_rdev)
#define get_minor(session)      MINOR(session->f_inode->i_rdev)
#else
#define get_major(session)      MAJOR(session->f_dentry->d_inode->i_rdev)
#define get_minor(session)      MINOR(session->f_dentry->d_inode->i_rdev)
#endif

#define PRIORITIES (2)

typedef struct _device_state {
    int disabled;
    int current_priority;
    loff_t old_pos;
    struct mutex operation_synchronizer;
    int valid_bytes[PRIORITIES];
    char *streams[PRIORITIES];
} device_state;

#define MINORS 128
device_state devices[MINORS];

#define STREAM_MAX_SIZE  (4096) // just one page: 4KB

/* the actual driver */

static int dev_open(struct inode *inode, struct file *file)
{
    device_state *dev;
    int minor = get_minor(file);

    if (minor >= MINORS)
        return -ENODEV;

    dev = devices + minor;

    if (dev->disabled)
        return -EACCES;
    
    // if(!try_module_get(THIS_MODULE))
    //     return -ENODEV;

    printk("%s: device file successfully opened for object with minor %d\n",MODNAME,minor);

    return 0;
}

static int dev_release(struct inode *inode, struct file *file)
{
    int minor;
    minor = get_minor(file);

    printk("%s: device file closed\n",MODNAME);
    // module_put(THIS_MODULE);

    return 0;
}

ssize_t dev_read(struct file *file, char *buf, size_t count, loff_t *pos)
{
    int ret;
    int idx;
    int minor = get_minor(file);

    device_state *dev;
    dev = devices + minor;
    printk("%s: somebody called a read on dev with [major,minor] number [%d,%d]\n",MODNAME,get_major(file),get_minor(file));

    mutex_lock(&(dev->operation_synchronizer));
    idx = dev->current_priority;
    if (*pos > dev->valid_bytes[idx]) {
        mutex_unlock(&(dev->operation_synchronizer));
        return 0;
    }
    
    if((dev->valid_bytes[idx] - *pos) < count) count = dev->valid_bytes[idx] - *pos;
        ret = copy_to_user(buf,&(dev->streams[idx][*pos]), count);
  
    *pos += (count - ret);
    mutex_unlock(&(dev->operation_synchronizer));

    return count - ret;
    printk("%s: somebody called a read on dev with [major,minor] number [%d,%d]\n",MODNAME,get_major(file),get_minor(file));

    return 0;
}

ssize_t dev_write(struct file *file, const char *buf, size_t count, loff_t *pos)
{
    int ret;
    int idx;
    int minor = get_minor(file);

    device_state *dev;
    dev = devices + minor;
    printk("%s: somebody called a write on dev with [major,minor] number [%d,%d]\n",MODNAME,get_major(file),get_minor(file));

    mutex_lock(&(dev->operation_synchronizer));
    idx = dev->current_priority;

    if(*pos >= STREAM_MAX_SIZE) {   //offset too large
        mutex_unlock(&(dev->operation_synchronizer));
        return -ENOSPC; //no space left on device
    }

    if(*pos > dev->valid_bytes[idx]) {   //offset beyond the current stream size
        mutex_unlock(&(dev->operation_synchronizer));
        return -ENOSR;  //out of stream resources
    } 
    
    if((STREAM_MAX_SIZE - *pos) < count) count = STREAM_MAX_SIZE - *pos;
        ret = copy_from_user(&(dev->streams[idx][*pos]), buf, count);
  
    *pos += (count - ret);
    dev->valid_bytes[idx] = *pos;
    mutex_unlock(&(dev->operation_synchronizer));

    return count - ret;
}

static long dev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    int minor = get_minor(file);

    device_state *dev;
    dev = devices + minor;
    printk("%s: somebody called a ioctl on dev with [major,minor] number [%d,%d]\n",MODNAME,get_major(file),get_minor(file));

    mutex_lock(&(dev->operation_synchronizer));
    switch (cmd)
    {
    case MFIOC_CHANGE_PRIORITY:
        dev->current_priority++;
        dev->current_priority %= PRIORITIES;
        SWAP(file->f_pos, dev->old_pos, loff_t);
        break;
    
    default:
        break;
    }
    mutex_unlock(&(dev->operation_synchronizer));

    return 0;
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
    //initialize the drive internal state
    for (i = 0; i < MINORS; i++){
        devices[i].current_priority = LOW_PRIORITY;
        devices[i].disabled = 0;
        devices[i].old_pos = 0;
        mutex_init(&(devices[i].operation_synchronizer));
        for (j = 0; j < PRIORITIES; j++) {
            devices[i].valid_bytes[j] = 0;
            devices[i].streams[j] = NULL;
            devices[i].streams[j] = (char *)__get_free_page(GFP_KERNEL);
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