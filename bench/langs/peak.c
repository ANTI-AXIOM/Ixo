/* Runs a program and reports the most memory it used (Linux, macOS).
 *
 * A child's maximum resident set also counts the memory of the process that
 * forked it, before exec: measured from Python, every small program would
 * look as big as the Python interpreter. This launcher is tiny, so what it
 * reports is the program's own peak. Usage: peak <file> <program> [args...]
 * writes the peak in bytes to <file> and exits with the program's status. */
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    pid_t pid = fork();
    if (pid < 0) return 2;
    if (pid == 0) {
        execvp(argv[2], argv + 2);
        _exit(127);
    }
    int status = 0;
    struct rusage usage;
    if (wait4(pid, &status, 0, &usage) < 0) return 2;
#ifdef __APPLE__
    long long bytes = (long long)usage.ru_maxrss;
#else
    long long bytes = (long long)usage.ru_maxrss * 1024;
#endif
    FILE* f = fopen(argv[1], "w");
    if (f) {
        fprintf(f, "%lld\n", bytes);
        fclose(f);
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
