# Makefile to go with lx_timer_test_mod.c
obj-m += lx_timer_test_mod.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

