#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "kernel/syscall.h"
#include "kernel/memlayout.h"
#include "kernel/riscv.h"

void env(int size, int interval, char* env_name) {
    int result = 1;
    int loop_size = 1000000000;
    for (int i = 0; i < loop_size; i++) {
        if (i % interval == 0) {
            result = result * size;
        }
    }
}

void env_large() {
    env(100000000, 1000000000, "env_large");
}

void env_freq() {
    env(100, 100, "env_freq");
}

int
main(int argc, char *argv[])
{
    int n_forks = 2;
    int pid = getpid();
    for (int i = 0; i < n_forks; i++) {
        fork();
    }
    int larges = 0;
    int freqs = 0;
    int n_experiments = 3;
    for (int i = 0; i < n_experiments; i++) {
        env_large(10, 3, 100);
        if (pid == getpid()) {
            printf("experiment %d/%d\n", i + 1, n_experiments);
            larges += get_utilization();
        }
        sleep(10);
        env_freq(10, 100);
        if (pid == getpid()) {
            freqs += get_utilization();
        }
    }
    if (pid == getpid()) {
        printf("larges = %d\nfreqs = %d\n", larges / n_experiments, freqs / n_experiments);
    }
    exit(0);
}