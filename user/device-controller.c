/**
 * @file device-controller.c
 * @brief This is a thin client that allows you to interact with the multi-flow
 *        devices implemented by the multi-flow-dev module.
 *
 * @author Cristiano Cuffaro
 * 
 * @date March 12, 2022
 */

#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/ioctl.h>

#include "../include/ioctl.h"

void int_to_priority(char *buf, int prio)
{
    memset(buf, 0x0, sizeof(buf));
    if (prio)
        sprintf(buf, "high");
    else
        sprintf(buf, "low");
}

void *the_thread(void *path)
{
	char *device;
	int fd = -1;
    int op;
	int ret;
    int num;
    int ch;
    long timeout;
	char command[256];
    char bytes[4096];
    char prio_str[8];
    int priority = 0;

	device = (char *)path;
    int_to_priority(prio_str, priority);

    while(1) {
        printf("\033[2J\033[H");
		printf("*** Main menu ***\n\n");
        printf("1) Open device\n");
        printf("2) Close device\n");
		printf("3) Write on current priority flow\n");
		printf("4) Read on current priority flow\n");
        printf("5) Switch to low or high priority flow for the device\n");
        printf("6) Switch to blocking or non-blocking read and write operations\n");
        printf("7) Set a new time interval for blocking read and write operations\n");
		printf("8) Quit\n\n");
        printf("Enter the code of a command and press 'Enter'\n");

        scanf("%d", &op);
        while (getchar() != '\n');

        memset(bytes, 0x0, sizeof(bytes));
        ret = 0;
        num = 0;
        switch (op)
        {
        case 1:
            fd = open(device, O_RDWR);
            if(fd < 0) {
                perror("open error");
            } else {
                printf("device '%s' opened with the following characteristics:\n", device);
                printf("- you are working on %s priority data stream\n", prio_str);
                printf("- you are working with blocking read and write operations\n");
                printf("- timeout for blocking operations is set to '10' secs\n\n");
            }
            break;
        case 2:
            ret = close(fd);
            if(ret < 0) {
                perror("close error");
            } else {
                printf("device '%s' successfully closed\n\n", device);
            }
            fd = -1;
            break;
        case 3:
            printf("enter up to 4095 bytes which should be written on %s priority data stream:\n",
                    prio_str);
            num = 0;
            while ((ch = getchar()) != '\n' && ch != EOF && num < 4096) {
                bytes[num++] = ch;
            }
            while (ch != '\n' || ch != EOF) {
                ch = getchar();
            }
            bytes[num] = '\0';
            ret = write(fd, bytes, num);
            if (ret < 0)
                perror("write error");
            else
                printf("writed %d bytes on %s priority data stream of device %s\n\n",
                        ret, prio_str, device);
            break;
        case 4:
            printf("enter the number (up to 4095) of bytes which should be read from %s priority data stream:\n",
                    prio_str);
            scanf("%d", &num);
            while (getchar() != '\n');
            if (num > 4095)
                num = 4095;
            ret = read(fd, bytes, num);
            if (ret < 0)
                perror("read error");
            else
                printf("read %d bytes:\n%s\n\n", ret, bytes);
            break;
        case 5:
            ret = ioctl(fd, IOC_SWITCH_PRIORITY);
            if (ret < 0) {
                perror("ioctl error - impossible to change priority level");
            } else {
                priority = ret;
                int_to_priority(prio_str, ret);
                printf("now you are working on %s priority data stream\n",
                       prio_str);
            }
            break;
        case 6:
            ret = ioctl(fd, IOC_SWITCH_BLOCKING);
            if (ret < 0)
                perror("ioctl error - impossible to switch blocking operations mode");
            else
                printf("now you are working with %s read and write operations\n",
                       (ret ? "non-blocking" : "blocking"));
            break;
        case 7:
            printf("enter the new timeout interval value (in seconds):\n");
            scanf("%ld", &timeout);
            while (getchar() != '\n');

            ret = ioctl(fd, IOC_SET_WAIT_TIMEINT, &timeout);
            if (ret < 0)
                perror("ioctl error - impossible to set wait timeout interval");
            else
                printf("now the maximum wait for blocking operations is '%ld' secs\n",
                       timeout);
            break;
        default:
            system("clear\n");
		    printf("Press 'ctrl + c' to exit. Bye!\n\n");
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
    char buff[512];

	if(argc < 4) {
		printf("Usage: %s <pathname> <major> <minor>\n", *argv);
		return -1;
	}

	path = argv[1];
	major = strtol(argv[2],NULL,10);
	minor = strtol(argv[3],NULL,10);

    sprintf(buff,"mknod %s%d c %d %d 2>/dev/null\n", path, minor, major, minor);
    system(buff);
    snprintf(buff, 512, "%s%d", path, minor);
    pthread_create(&tid, NULL, the_thread, strdup(buff));

	pause();
	return 0;
}