#include <linux/kernel.h>

int ignoreK = 0;
EXPORT_SYMBOL(ignoreK);

int ignoreR = 0;
EXPORT_SYMBOL(ignoreR);

int reconRead = 0;
EXPORT_SYMBOL(reconRead);


asmlinkage void sys_changeIgnoreK(int newval){
	ignoreK = newval;
	printk("Current value of ignoreK: %d\n",ignoreK);
}

asmlinkage void sys_changeIgnoreR(int newval){
	ignoreR = newval;
	printk("Current value of ignoreR: %d\n", ignoreR);
}

asmlinkage void sys_changeReconRead(int newval){
	reconRead = newval;
	if(reconRead)
		printk("Will reconstruct read\n");
	else
		printk("Will not reconstruct read.\n");
}


