#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/ioctl.h>

char buff[4096];
#define DATA_LOW "abcdefghijklmnopqrstuvwxyz"
#define SIZE_LOW strlen(DATA_LOW)
#define DATA_HIGH "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
#define SIZE_HIGH strlen(DATA_HIGH)

void * the_thread(void *path){
	char *device;
	int fd;
    int op;
	int ret;
    int num;
	char command[256];
    char bytes[4100];

	device = (char *)path;
	sleep(1);

	memset(command, 0x0, strlen(command));
	sprintf(command, "echo '\n' && cat %s\n", device);

    while(1) {
        system("clear\n");
		printf("*** Main menu ***\n\n");
		printf("1) Write on low priority flow\n");
		printf("2) Read from low priority flow\n");
		printf("3) Write on high priority flow\n");
		printf("4) Read from high priority flow\n");
		printf("5) Quit\n");

        scanf("%d", &op);

        memset(bytes, 0x0, sizeof(bytes));
        num = 0;
        switch (op)
        {
        case 1:
            printf("opening device %s\n", device);
            fd = open(device, O_RDWR);
            if(fd == -1) {
                printf("open error on device %s\n", device);
                return NULL;
            }
            printf("device %s successfully opened\n\n", device);

            printf("insert the bytes which should be written:\n");
            scanf("%s", bytes);
            ret = write(fd, bytes, strlen(bytes));
            if (ret == -1)
                printf("write on device '%s' failed\n\n", device);
            else
                printf("writed %d bytes on low priority stream of device %s\n\n", ret, device);

            close(fd);
            break;
        case 2:
            printf("opening device %s\n", device);
            fd = open(device, O_RDWR);
            if(fd == -1) {
                printf("open error on device %s\n", device);
                return NULL;
            }
            printf("device %s successfully opened\n\n", device);

            printf("insert the number of bytes which should be read:\n");
            scanf("%d", &num);
            ret = read(fd, bytes, num);
            if (ret == -1)
                printf("read on device '%s' failed\n\n", device);
            else
                printf("read bytes are the following:\n%s\n\n", bytes);

            close(fd);
            break;
        case 3:
            printf("opening device %s\n", device);
            fd = open(device, O_RDWR);
            if(fd == -1) {
                printf("open error on device %s\n", device);
                return NULL;
            }
            printf("device %s successfully opened\n\n", device);

            ret = ioctl(fd,0);
            if (ret != 1)
                printf("impossible to change priority level for device '%s'\n", device);
                
            printf("insert the bytes which should be written:\n");
            scanf("%s", bytes);
            ret = write(fd, bytes, strlen(bytes));
            if (ret == -1)
                printf("write on device '%s' failed\n\n", device);
            else
                printf("writed %d bytes on high priority stream of device %s\n\n", ret, device);

            close(fd);
            break;
        case 4:
            printf("opening device %s\n", device);
            fd = open(device, O_RDWR);
            if(fd == -1) {
                printf("open error on device %s\n", device);
                return NULL;
            }
            printf("device %s successfully opened\n\n", device);

            ret = ioctl(fd,0);
            if (ret != 1)
                printf("impossible to change priority level for device '%s'\n", device);

            printf("insert the number of bytes which should be read:\n");
            scanf("%d", &num);
            ret = read(fd, bytes, num);
            if (ret == -1)
                printf("read on device '%s' failed\n\n", device);
            else
                printf("read bytes are the following:\n%s\n\n", bytes);

            close(fd);
            break;
        
        default:
            system("clear\n");
		    printf("uscita...\n\n");
            goto end;
        }
        while ( getchar() != '\n' );
        getchar();
    }
end:
	memset(command, 0x0, strlen(command));
	sprintf(command, "rm %s\n", device);
	system(command);

	return NULL;
}

int main(int argc, char** argv)
{
	int ret;
	int major;
	int minor;
	char *path;
	pthread_t tid;

	if(argc < 4) {
		printf("Usage: %s <pathname> <major> <minor>\n", *argv);
		return -1;
	}

	path = argv[1];
	major = strtol(argv[2],NULL,10);
	minor = strtol(argv[3],NULL,10);

    sprintf(buff,"mknod %s%d c %d %d\n", path, minor, major, minor);
    system(buff);
    sprintf(buff, "%s%d", path, minor);
    pthread_create(&tid, NULL, the_thread, strdup(buff));


	pause();
	return 0;
}
