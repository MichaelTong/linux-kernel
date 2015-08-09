#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>
#include <string.h>
#define BLOCK_SIZE 3*4096 //bytes
#define BUFF_OFFSET 4096

int main(int argc, char *argv[]){
	char device[64];
	int fd;   
	void *buff;
	int size; //bytes
	int seek;
	int i;
	struct timeval start,end;
	long total = 0;

  	if(argc < 2){
  		printf("Please specify device name\n");
 		exit(1);
	}

	//fd = open(argv[1], O_DIRECT);
	fd = open(argv[1], O_DIRECT | O_RSYNC | O_RDONLY);
	if (fd < 0){
		printf("Cannot open %s\n",argv[1]);
		exit(1);
	}else{
		printf("Open return value: %d\n",fd);
	}
	//buff=malloc(BLOCK_SIZE);
	posix_memalign(&buff, BUFF_OFFSET, BLOCK_SIZE);
	for(i = 0; i < 1; i++){
		int load;

		seek = lseek(fd,0,SEEK_SET);
		if (seek < 0){
			printf("Cannot seek\n");
			continue;
		}else{
		//	printf("Seek return value: %d\n",seek);
		}

		gettimeofday(&start,NULL);
		load = read(fd,buff,BLOCK_SIZE);
		if (load < 0){
			printf("Cannot read\n");
			perror("because");
			break;
		}else{
			printf("load %d\n",load);
		}
		gettimeofday(&end,NULL);
		total += ((end.tv_sec * 1000000 + end.tv_usec) - (start.tv_sec * 1000000 + start.tv_usec));
		//printf("Time: %ld\n",(end.tv_sec * 1000000 + end.tv_usec) - (start.tv_sec * 1000000 + start.tv_usec));
		printf("%s\n",(char *)buff);
		sleep(0);  
	}

	printf("total: %ld\n", total);

	close(fd);
	return 0;
}
