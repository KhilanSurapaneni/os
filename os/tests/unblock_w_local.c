#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BURN 1000
#define FULL (8 * 1024)

int main(int argc, char *argv[])
{
    int i, rc, num_procs, fd[2];
    char wr_buf[FULL];

    if (argc < 2 || (num_procs = atoi(argv[1])) <= 0)
    {
        printf("Usage: ./unblock_w_local <num_procs>\n");
        exit(0);
    }

    rc = pipe(fd);
    if (rc != 0)
    {
        perror("pipe() failed");
        exit(0);
    }

    write(fd[1], wr_buf, FULL);

    for (i = 0; i < num_procs; ++i)
    {
        rc = fork();
        if (rc < 0)
        {
            perror("fork() failed");
            exit(0);
        }
        else if (rc == 0)
        {
            close(fd[0]);
            printf("(%d) trying to write to pipe...\n", getpid());
            rc = write(fd[1], wr_buf, 1);
            if (rc < 0)
                perror("write to pipe");
            printf("(%d) write returned %d\n", getpid(), rc);
            exit(0);
        }
    }

    for (i = 0; i < BURN; ++i)
        close(100);

    printf("Closing pipe...\n");
    close(fd[0]);

    for (i = 0; i < num_procs; ++i)
        wait(NULL);

    return 0;
}
