#define _XOPEN_SOURCE 700

#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern int errno;

int visited_files;
int bytes_processed;
int write_pipe;
int infile;

sigjmp_buf env;

int grep_cmd(int grep_in, int grep_out, char* pattern) {
    if (dup2(grep_in, STDIN_FILENO) < 0) {
        fprintf(stderr, "Could not complete dup2 for stdin to grep: %s\n",
                strerror(errno));
        exit(1);
    }
    if (dup2(grep_out, STDOUT_FILENO) < 0) {
        fprintf(stderr, "Could not complete dup2 for stdout to grep: %s\n",
                strerror(errno));
        exit(1);
    }

    int i = execlp("grep", "grep", pattern, NULL);
    if (i < 0) {
        fprintf(stderr, "Exec call failed:  %s\n", strerror(errno));
        exit(1);
    }
    return 0;
}

int more_cmd(int more_in) {
    if (dup2(more_in, STDIN_FILENO) < 0) {
        fprintf(stderr, "Could not complete dup2 for stdin to more: %s\n",
                strerror(errno));
        exit(1);
    }
    int i = execlp("more", "more", NULL);
    if (i < 0) {
        fprintf(stderr, "Exec call failed:  %s\n", strerror(errno));
        exit(1);
    }
    return 0;
}

void sigusr1(int signo) {
    fprintf(stderr,
            "Number of Files Visited: %d \t Number of Bytes Seen: %d \n",
            visited_files, bytes_processed);
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGUSR1);
    sa.sa_handler = sigusr1;
    sigaction(SIGUSR1, &sa, 0);
}

void sigusr2(int signo) {
    if (signo == SIGPIPE) {
        fprintf(stderr, "Broken Pipe: ");
    } else {
        fprintf(stderr, "SIGUSR2 recieved: ");
    }
    fprintf(stderr, "moving on to file #%d\n", visited_files + 1);
    siglongjmp(env, 1);
}

int main(int argc, char** argv) {
    // Pattern is the second argument
    errno = 0;
    // char* pattern = argv[1];

    // Declaring these here for better memory management
    char buffer[4096];
    int bufsize = 4096;
    int grep_p[2];
    int more_p[2];

    int grep;
    int more;

    visited_files = 0;
    bytes_processed = 0;

    struct sigaction restart = {0};
    sigset_t set_u1 = {0};
    sigset_t set_u2 = {0};

    sigaddset(&set_u2, SIGUSR2);
    sigaddset(&set_u1, SIGUSR1);
    sigemptyset(&restart.sa_mask);
    restart.sa_flags = SA_RESTART;

    signal(SIGUSR1, sigusr1);
    signal(SIGUSR2, sigusr2);
    signal(SIGPIPE, sigusr2);

    for (int i = 2; i < argc; i++) {
        // jump here if sigusr2 is sent
        int jmp = sigsetjmp(env, 0);
        if (jmp != 0) {
            close(more_p[0]);
            close(more_p[1]);
            close(grep_p[0]);
            // Kill more
            kill(more, SIGINT);
        } else {
            infile = open(argv[i], O_RDONLY);
            visited_files++;
            if (errno < 0) {
                fprintf(stderr, "Failed to open file: Reason %s\n",
                        strerror(errno));
                exit(1);
            }

            if (pipe(grep_p) < 0 || pipe(more_p) < 0) {
                fprintf(stderr, "Failed to create pipes: %s\n", strerror(errno));
                exit(1);
            }

            grep = fork();
            switch (grep) {
                case 0:
                    // Clean stdin/stdout
                    close(infile);
                    close(grep_p[1]);
                    close(more_p[0]);
                    grep_cmd(grep_p[0], more_p[1], argv[1]);
                    exit(0);
                    break;
                case -1:
                    fprintf(stderr, "Failed to create grep child: %s\n",
                            strerror(errno));
                    exit(1);
                    break;
            }
            // grep will never reach here courtesy of the exit
            // Not to mention that the process is no longer attached to this program
            more = fork();

            // These halves of the pipe is no longer necessary
            close(grep_p[0]);
            close(more_p[1]);

            switch (more) {
                case 0:
                    // block u1 & u2 in more
                    sigprocmask(SIG_BLOCK, &set_u1, NULL);
                    sigprocmask(SIG_BLOCK, &set_u2, NULL);
                    // Clean stdin/stdout
                    close(infile);
                    close(grep_p[1]);
                    more_cmd(more_p[0]);
                    exit(0);
                    break;
                case -1:
                    fprintf(stderr, "Failed to create more child: %s\n",
                            strerror(errno));
                    exit(1);
                    break;
            }
            close(more_p[0]);

            write_pipe = grep_p[1];
            // fprintf(stderr, "Starting to read from infile\n");
            int r = read(infile, buffer, bufsize);
            while (r > 0) {
                // fprintf(stderr, "Read %d chars \n", r);

                // Should not be reading the number of written bites as bites are being written
                sigprocmask(SIG_BLOCK, &set_u1, NULL);

                int written = write(write_pipe, buffer, r);
                // fprintf(stderr, "Written %d chars \n", written);

                // Keep writing while the number of written bits isn't satisfactory
                while (written != r) {
                    written += write(write_pipe, buffer + written, r - written);
                    // fprintf(stderr, "Written a total of %d chars \n", written);
                }
                bytes_processed += written;
                sigprocmask(SIG_UNBLOCK, &set_u1, NULL);
                r = read(infile, buffer, bufsize);
            }
            // Block sigusr2 here so that wait & closing can happen
        }
        close(infile);
        close(write_pipe);
        int status_1;
        waitpid(grep, &status_1, 0);
        int status_2;
        waitpid(more, &status_2, 0);
        // fprintf(stderr, "Moving onto the next file: \n");
    }
    return 0;
}