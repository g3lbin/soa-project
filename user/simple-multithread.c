#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <errno.h>
#include "../include/ioctl.h"

#define DATA_LOW "abcdefghijklmnopqrstuvwxyz"
#define SIZE_LOW strlen(DATA_LOW)
#define DATA_HIGH "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
#define SIZE_HIGH strlen(DATA_HIGH)

void * the_thread(void *path){
        char *device;
        int fd;
        int ret;
        char buffer[4096];
        pthread_t me = pthread_self();

        device = (char *)path;

        /* TEST OPEN */
        fd = open(device, O_RDWR);
        if(fd == -1) {
                printf("(%ld) open error on device '%s'\n", me, device);
                return NULL;
        }
        printf("(%ld) device '%s' successfully opened\n", me, device);
        /* Uncomment the following line if you want to set timeout for blocking ops to 2 secs */
        // ioctl(fd, IOC_SET_WAIT_TIMEINT, 2);

        /* Uncomment the following line if you want to set the O_NONBLOCK flag */
        // ioctl(fd, IOC_SWITCH_BLOCKING);

        /* TEST WRITE LOW_PRIORITY */
        ret = write(fd, DATA_LOW, SIZE_LOW);
        if (ret != SIZE_LOW)
                printf("(%ld) write on device '%s' failed\n", me, device);
        else
                printf("(%ld) written %d bytes on low priority stream of device '%s': %s\n", me, ret, device, DATA_LOW);

        /* TEST READ LOW_PRIORITY */
        ret = read(fd, buffer, SIZE_LOW);
        if (ret != SIZE_LOW)
                printf("(%ld) read on device '%s' failed\n", me, device);
        else
                printf("(%ld) read %d bytes on low priority stream of device '%s': %s\n", me, ret, device, buffer);

        /* TEST IOCTL */
        ret = ioctl(fd,IOC_SWITCH_PRIORITY);
        if (ret != 1)
                printf("(%ld) impossible to change priority level for device '%s'\n", me, device);
        else
                printf("(%ld) priority level of device '%s' currently set to HIGH_PRIORITY\n", me, device);
        
        /* TEST WRITE HIGH_PRIORITY */
        ret = write(fd, DATA_HIGH, SIZE_HIGH);
        if (ret != SIZE_HIGH)
                printf("(%ld) write on device '%s' failed\n", me, device);
        else
                printf("(%ld) written %d bytes on high priority stream of device '%s': %s\n", me, ret, device, DATA_HIGH);

        /* TEST READ HIGH_PRIORITY */
        ret = read(fd, buffer, SIZE_LOW);
        if (ret != SIZE_LOW)
                printf("(%ld) read on device '%s' failed\n", me, device);
        else
                printf("(%ld) read %d bytes on high priority stream of device '%s': %s\n", me, ret, device, buffer);

        memset(buffer, 0x0, strlen(buffer));
        sprintf(buffer, "rm %s\n", device);
        // system(buffer);

        return NULL;
}

int main(int argc, char** argv)
{
        int i;
        int ret;
        int major;
        int minor;
        int threads;
        char *path;
        pthread_t tid;
        char buff[512];

        if(argc < 5) {
                printf("Usage: %s <pathname> <major> <minor> <threads>\n", *argv);
                return -1;
        }

        path = argv[1];
        major = strtol(argv[2],NULL,10);
        minor = strtol(argv[3],NULL,10);
        threads = strtol(argv[4],NULL,10);

        snprintf(buff, 512, "mknod %s%d c %d %i\n", path, minor, major, minor);
        system(buff);
        snprintf(buff, 512, "%s%d", path, minor);

        for (i = 0; i < threads; i++){
        /* Uncomment the following lines if you want to work on different devices */
                // snprintf(buff, 512, "mknod %s%d c %d %i\n", path, i, major, minor);
                // system(buff);
                // snprintf(buff, 512, "%s%d", path, i);
                pthread_create(&tid, NULL, the_thread, strdup(buff));
    }
        pause();
        return 0;
}