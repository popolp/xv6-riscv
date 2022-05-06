#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"


void main() {
    for (int i = 0; i < 5; i++) {
        int pid = fork();
        if (pid != 0) {
            printf("%d\n", pid);
            int pid2 = fork();
            if (pid2 != 0) {
                printf("%d\n", pid2);
                exit(0);
            }
            exit(0);
        }
    }
    while(wait(0) > 0);
    exit(0);
}