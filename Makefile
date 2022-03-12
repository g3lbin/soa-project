obj-m += multi-flow-dev.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	make -C $(KDIR) M=$(PWD) modules 

debug:
	KCFLAGS="-DDEBUG" make -C $(KDIR) M=$(PWD) modules

single-session:
	KCFLAGS="-DSINGLE_SESSION_OBJECT" make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean
