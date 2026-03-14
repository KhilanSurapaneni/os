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

static void spawn_child(int keep_pd[2], int other_pd[2], const char *prog)
{
    int pid;
    int rc;

    pid = fork();
    if (pid < 0)
        fail("fork failed\n");

    if (pid == 0)
    {
        close(0);
        close(other_pd[0]);
        close(other_pd[1]);
        close(keep_pd[1]);
        rc = dup(keep_pd[0]);
        if (rc < 0)
            fail("dup failed\n");
        close(keep_pd[0]);
        execve((char *)prog, (char **)&prog, NULL);
        fail("execve failed\n");
    }
}

int main(int argc, char **argv)
{
    int pd1[2];
    int pd2[2];
    int rc;
    int sent;
    char buf[256];

    if (argc != 3)
        fail("usage: tee_two <child1> <child2>\n");

    if (pipe(pd1) < 0)
        fail("pipe 1 failed\n");
    if (pipe(pd2) < 0)
        fail("pipe 2 failed\n");

    spawn_child(pd1, pd2, argv[1]);
    spawn_child(pd2, pd1, argv[2]);

    close(pd1[0]);
    close(pd2[0]);

    while ((rc = read(0, buf, sizeof(buf))) > 0)
    {
        sent = 0;
        while (sent < rc)
        {
            int wr = write(pd1[1], buf + sent, rc - sent);
            if (wr <= 0)
                fail("write pd1 failed\n");
            sent += wr;
        }

        sent = 0;
        while (sent < rc)
        {
            int wr = write(pd2[1], buf + sent, rc - sent);
            if (wr <= 0)
                fail("write pd2 failed\n");
            sent += wr;
        }
    }

    if (rc < 0)
        fail("read failed\n");

    close(pd1[1]);
    close(pd2[1]);
    wait(NULL);
    wait(NULL);
    return 0;
}
