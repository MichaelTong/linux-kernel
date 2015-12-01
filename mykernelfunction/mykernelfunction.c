#include <linux/kernel.h>

int ignoreK = 0;
EXPORT_SYMBOL(ignoreK);

int ignoreR = 0;
EXPORT_SYMBOL(ignoreR);

int readPolicy = 0;
EXPORT_SYMBOL(readPolicy);

int waitCounter = 0;
EXPORT_SYMBOL(waitCounter);

int enableCount = 0;
EXPORT_SYMBOL(enableCount);

long unsigned debuginfo = 0;
EXPORT_SYMBOL(debuginfo);


asmlinkage void sys_changeIgnoreK(int newval){
	ignoreK = newval;
	printk("Current value of ignoreK: %d\n",ignoreK);
}

asmlinkage void sys_changeIgnoreR(int newval){
	ignoreR = newval;
	printk("Current value of ignoreR: %d\n", ignoreR);
}

asmlinkage void sys_changeReadPolicy(int newval){
	readPolicy = newval;
	switch(readPolicy)
	{
    case 0:
        printk("Normal read\n");
        break;
    case 1:
        printk("Reactive read\n");
        break;
    case 2:
        printk("Proactive read\n");
        break;
    case 4:
        printk("Adaptive read\n");
        break;
    default:
        printk("Unknown Policy\n");
        break;
	}
}


asmlinkage void sys_changeEnableCount(int newval){
	if(newval==1)
	{
		enableCount = 1;
		printk("Enable wait counter\n");
	}
	else
	{
		enableCount = 0;
		printk("Disable wait counter, previous value of counter is %d\n", waitCounter);
		waitCounter = 0;
	}
}

asmlinkage void sys_changeDebuginfo(int bit, bool on)
{
	if(on)
	{
		debuginfo |= 1 << bit;
		printk("Debug Info bit %d, on\n", bit);
	}
	else
	{
		long unsigned uf = 0xffffffff;
		uf ^= 1 << bit;
		debuginfo &= uf;
	}
}
