// Native, provider-free workload for process-tree cleanup regression tests.
#include <cerrno>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

int main() {
    const pid_t child = fork();
    if (child < 0)
        return 2;
    if (child == 0) {
        // A new process group defeats cleanup that only signals the root PGID.
        if (setsid() < 0)
            _exit(3);
        while (true)
            pause();
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    return 0;
}
