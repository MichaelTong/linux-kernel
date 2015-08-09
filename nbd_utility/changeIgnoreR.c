#include <stdio.h>
#include <linux/kernel.h>
#include <sys/syscall.h>
#include <unistd.h>
int main()
{
    syscall(324, 1);
    syscall(323, 1);
    printf("Change reconRead to 1\n");
    return 0;
}
