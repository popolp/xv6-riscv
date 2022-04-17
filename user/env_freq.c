#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"


void env(int size, int interval, char* env_name) {
    int is_main = 0;
    int result = 1;
    int loop_size = 10e6;
    int n_forks = 3;
    int pid;
    for (int i = 0; i < n_forks; i++) {
        pid = fork();
        if(i == 0 && pid != 0){ is_main = 1;}
        else if(pid == 0) {is_main = 0;}
    }
    for (int i = 0; i < loop_size; i++) {
        if (i % (int)(loop_size / 10e0) == 0) {
        	if (pid == 0) {
        		printf("%s %d/%d completed.\n", env_name, i, loop_size);
        	} else {
        		printf(" ");
        	}
        }
        if (i % interval == 0) {
            result = result * size;
        }
    }
    while(wait(0) > 0);         // wait for ALL children processes to exit
    if(!is_main){ exit(0);}
    printf("\n");
}

int main(int argc, char **argv) {
    print_stats();
    env(10e1, 10e1, "env_freq");
    print_stats();
    exit(0);
}