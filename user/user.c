#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/ioctl.h>

int i;
char buff[4096];
#define DATA_LOW "ciao a tutti\n"
#define SIZE_LOW strlen(DATA_LOW)
#define DATA_HIGH "eccolo\n"
#define SIZE_HIGH strlen(DATA_HIGH)

void * the_thread(void *path){
	char *device;
	int fd;
	int ret;
	char command[4096];

	device = (char *)path;
	sleep(1);

	memset(command, 0x0, strlen(command));
	sprintf(command, "cat %s\n", device);

	/* TEST OPEN */
	printf("opening device %s\n", device);
	fd = open(device, O_RDWR | O_APPEND);
	if(fd == -1) {
		printf("open error on device %s\n", device);
		return NULL;
	}
	printf("device %s successfully opened\n", device);
	
	/* TEST WRITE LOW_PRIORITY */
	ret = write(fd, DATA_LOW, SIZE_LOW);
	if (ret != SIZE_LOW)
		printf("write on device '%s' failed\n", device);
	else
		printf("writed on low priority stream of device %s\n", device);

	/* TEST READ LOW_PRIORITY */
	printf("read on low priority stream of device %s:\n", device);
	system(command);

	/* TEST IOCTL */
	ret = ioctl(fd,0);
	if (ret != 1)
		printf("impossible to change priority level for device '%s'\n", device);
	else
		printf("priority level of device '%s' currently set to HIGH_PRIORITY\n", device);
	
	/* TEST WRITE HIGH_PRIORITY */
	ret = write(fd, DATA_HIGH, SIZE_HIGH);
	if (ret != SIZE_HIGH)
		printf("write on device '%s' failed\n", device);
	else
		printf("writed on high priority stream of device %s\n", device);

	/* TEST READ HIGH_PRIORITY */
	printf("read on high priority stream of device %s:\n", device);
	system(command);

	/* TEST IOCTL */
	ret = ioctl(fd,0);
	if (ret != 0)
		printf("impossible to change priority level for device '%s'\n", device);
	else
		printf("priority level of device '%s' currently set to LOW_PRIORITY\n", device);

	/* TEST READ LOW_PRIORITY */
	printf("read on low priority stream of device %s:\n", device);
	system(command);

	memset(command, 0x0, strlen(command));
	sprintf(command, "rm %s\n", device);
	system(command);

	return NULL;
}

int main(int argc, char** argv)
{
	int ret;
	int major;
	int minors;
	char *path;
	pthread_t tid;

	if(argc < 4) {
		printf("Usage: %s <pathname> <major> <minors>\n", *argv);
		return -1;
	}

	path = argv[1];
	major = strtol(argv[2],NULL,10);
	minors = strtol(argv[3],NULL,10);
	printf("creating %d minors for device %s with major %d\n",minors,path,major);

	for (i = 0; i < minors; i++){
		sprintf(buff,"mknod %s%d c %d %i\n", path, i, major, i);
		system(buff);
		sprintf(buff, "%s%d", path, i);
		pthread_create(&tid, NULL, the_thread, strdup(buff));
    }

	pause();
	return 0;
}
