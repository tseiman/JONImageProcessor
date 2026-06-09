#include "Daemon.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

bool daemonizeProcess(std::string& error)
{
    const pid_t firstPid = fork();
    if (firstPid < 0) {
        error = std::strerror(errno);
        return false;
    }
    if (firstPid > 0) {
        _exit(0);
    }

    if (setsid() < 0) {
        error = std::strerror(errno);
        return false;
    }

    const pid_t secondPid = fork();
    if (secondPid < 0) {
        error = std::strerror(errno);
        return false;
    }
    if (secondPid > 0) {
        _exit(0);
    }

    umask(0);
    chdir("/");

    const int nullFd = open("/dev/null", O_RDWR);
    if (nullFd >= 0) {
        dup2(nullFd, STDIN_FILENO);
        dup2(nullFd, STDOUT_FILENO);
        dup2(nullFd, STDERR_FILENO);
        if (nullFd > STDERR_FILENO) {
            close(nullFd);
        }
    }
    return true;
}
