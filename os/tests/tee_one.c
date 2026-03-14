#include <stdlib.h>
#include <unistd.h>

static void fail(const char *msg)
{
    int len = 0;
    while (msg[len] != '\0')
        len++;
    write(2, msg, len);
    exit(1);
}

int main(int argc, char **argv)
{
    int pid;
    int pd[2];
    int rc;
    char buf[256];

    if (argc != 2)
        fail("usage: tee_one <child_prog>\n");

    rc = pipe(pd);
    if (rc < 0)
        fail("pipe failed\n");

    pid = fork();
    if (pid < 0)
        fail("fork failed\n");

    if (pid == 0)
    {
        close(0);
        close(pd[1]);
        rc = dup(pd[0]);
        if (rc < 0)
            fail("dup failed\n");
        close(pd[0]);
        execve(argv[1], &argv[1], NULL);
        fail("execve failed\n");
    }

    close(pd[0]);

    while ((rc = read(0, buf, sizeof(buf))) > 0)
    {
        int sent = 0;
        while (sent < rc)
        {
            int wr = write(pd[1], buf + sent, rc - sent);
            if (wr <= 0)
            {
                perror("write");
                exit(1);
            }
            sent += wr;
        }
    }

    if (rc < 0)
        fail("read failed\n");

    close(pd[1]);
    wait(NULL);
    return 0;
}
