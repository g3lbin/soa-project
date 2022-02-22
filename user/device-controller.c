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
	int fd = -1;
    int op;
	int ret;
    int num;
    int ch;
	char command[256];
    char bytes[4100];

	device = (char *)path;
	sleep(1);

	memset(command, 0x0, strlen(command));
	sprintf(command, "echo '\n' && cat %s\n", device);

    while(1) {
        system("clear\n");
		printf("*** Main menu ***\n\n");
        printf("1) Open device\n");
        printf("2) Close device\n");
		printf("3) Write on current priority flow\n");
		printf("4) Read on current priority flow\n");
        printf("5) Switch to low or high priority flow for the device\n");
        printf("6) Switch to blocking or non-blocking read and write operations\n");
		printf("7) Quit\n");

        scanf("%d", &op);
        while (getchar() != '\n');

        memset(bytes, 0x0, sizeof(bytes));
        num = 0;
        switch (op)
        {
        case 1:
            printf("opening device %s\n", device);
            fd = open(device, O_RDWR);
            if(fd < 0) {
                printf("(%d) open error on device %s\n", fd, device);
                return NULL;
            }
            printf("device %s successfully opened\n\n", device);
            break;
        case 2:
            printf("device %s successfully closed\n\n", device);
            close(fd);
            fd = -1;
            break;
        case 3:
            printf("insert the bytes which should be written:\n");
            num = 0;
            while ((ch = getchar()) != '\n' && ch != EOF) {
                bytes[num++] = ch;
            }
            bytes[num] = '\0';
            ret = write(fd, bytes, num);
            if (ret < 0)
                printf("(%d) write on device '%s' failed\n\n", ret, device);
            else
                printf("writed %d bytes on high priority stream of device %s\n\n", ret, device);
            break;
        case 4:
            printf("insert the number of bytes which should be read:\n");
            scanf("%d", &num);
            while (getchar() != '\n');
            ret = read(fd, bytes, num);
            if (ret < 0)
                printf("(%d) read on device '%s' failed\n\n", ret, device);
            else
                printf("read bytes are the following:\n%s\n\n", bytes);

            break;
        case 5:
            ret = ioctl(fd,0);
            if (ret < 0)
                printf("(%d) impossible to change priority level for device '%s'\n", ret, device);
            break;
        case 6:
            ret = ioctl(fd,1);
            if (ret < 0)
                printf("(%d) impossible to switch blocking operations mode for device '%s'\n", ret, device);
            break;
        default:
            system("clear\n");
		    printf("uscita...\n\n");
            goto end;
        }
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
