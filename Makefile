SHELL := /bin/bash

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

STATUS_ARRAY 	= /sys/module/multi_flow_dev/parameters/device_status
DEV		= 999999

ch_dev_status	= 											\
	@if ! [[ $(DEV) =~ ^[0-9]+$$ ]]; then								\
	    	echo "Not a number"; exit 1;								\
	elif [[ $(DEV) -ge 0 ]] && [[ $(DEV) -lt 128 ]]; then						\
	    	old=`cat $(STATUS_ARRAY)`;								\
		new=();											\
		c=0;											\
		for (( i=0; i<255; i++ )); do								\
			ch=$${old:$$i:1};								\
			if [[ $$c -eq $(DEV) ]] && [[ "$$ch" != ","  ]]; then				\
				new+=$(STATUS);								\
			else										\
				new+=$$ch;								\
			fi;										\
			if [[ "$$ch" == "," ]]; then							\
				c=$$((c+1));								\
			fi										\
		done;											\
		echo $$new > $(STATUS_ARRAY); exit 0;							\
	else												\
		echo "Set 'DEV=<minor>' (the minor must be between 0 and 127)"; exit 1;			\
	fi

disable:
	$(eval STATUS := 0)
	$(ch_dev_status)

enable:
	$(eval STATUS := 1)
	$(ch_dev_status)
