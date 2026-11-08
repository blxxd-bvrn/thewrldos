/* fork(), execve() and wait4(): build with the rest of samples/linux and run
 * it as C:/Linux/usr/local/bin/procs. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>

int main(void) {
    printf("parent is pid %d, command %s\n", getpid(), "procs");

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) {
        printf("child: pid %d, parent %d\n", getpid(), getppid());
        _exit(42);
    }

    int status = 0;
    pid_t w = waitpid(pid, &status, 0);
    if (w < 0) { perror("waitpid"); return 1; }
    printf("reaped %d (fork returned %d), exit status %d\n", w, pid, WIFEXITED(status) ? WEXITSTATUS(status) : -1);

    /* exec replaces this image; with no argument we take the other path. */
    if (getenv("PROCS_EXEC")) {
        char *argv[] = { "/usr/local/bin/hello", NULL };
        execv(argv[0], argv);
        perror("execv");
        return 1;
    }

    /* a child that ends by itself, then WNOHANG */
    pid = fork();
    if (pid == 0) _exit(7);
    for (int i = 0; i < 100; i++) {
        w = waitpid(pid, &status, WNOHANG);
        if (w == pid) { printf("WNOHANG reaped %d with %d\n", pid, WEXITSTATUS(status)); break; }
        usleep(1000);
    }
    return 0;
}
