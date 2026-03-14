#include <stdlib.h>
#include <unistd.h>

static void write_str(int fd, const char *s)
{
    int len = 0;
    while (s[len] != '\0')
        len++;
    write(fd, s, len);
}

static int append_str(char *buf, int pos, const char *s)
{
    int i = 0;
    while (s[i] != '\0')
    {
        buf[pos++] = s[i++];
    }
    return pos;
}

static int append_num(char *buf, int pos, int x)
{
    char tmp[32];
    int i = 0;
    int j;

    if (x == 0)
    {
        buf[pos++] = '0';
        return pos;
    }

    if (x < 0)
    {
        buf[pos++] = '-';
        x = -x;
    }

    while (x > 0)
    {
        tmp[i++] = (char)('0' + (x % 10));
        x /= 10;
    }

    for (j = i - 1; j >= 0; j--)
        buf[pos++] = tmp[j];

    return pos;
}

static void fail(const char *msg)
{
    write_str(2, msg);
    exit(1);
}

static void child_debug(int which, const char *msg, int fd)
{
    char buf[128];
    int pos = 0;

    pos = append_str(buf, pos, "child");
    pos = append_num(buf, pos, which);
    pos = append_str(buf, pos, ": ");
    pos = append_str(buf, pos, msg);
    pos = append_num(buf, pos, fd);
    pos = append_str(buf, pos, "\n");

    write(2, buf, pos);
}

static void parent_debug(const char *msg, int fd)
{
    char buf[128];
    int pos = 0;

    pos = append_str(buf, pos, "parent: ");
    pos = append_str(buf, pos, msg);
    pos = append_num(buf, pos, fd);
    pos = append_str(buf, pos, "\n");

    write(2, buf, pos);
}

int main(int argc, char **argv)
{
    int i;
    int j;
    int pid;
    int bytes;
    int sent;
    int rc;
    int pd[4];
    char *child_argv[2];
    char buf[256];

    child_argv[1] = NULL;

    if (argc != 3)
        fail("usage: tee_pipe_two_debug <child1> <child2>\n");

    rc = pipe(&pd[0]);
    if (rc < 0)
        fail("pipe 0 failed\n");

    rc = pipe(&pd[2]);
    if (rc < 0)
        fail("pipe 1 failed\n");

    for (i = 0; i < 2; i++)
    {
        pid = fork();
        if (pid < 0)
            fail("fork failed\n");

        if (pid == 0)
        {
            for (j = 0; j < 2; j++)
            {
                if (j != i)
                {
                    child_debug(i, "closing unrelated read fd ", pd[j * 2 + 0]);
                    close(pd[j * 2 + 0]);
                    child_debug(i, "closing unrelated write fd ", pd[j * 2 + 1]);
                    close(pd[j * 2 + 1]);
                }
            }

            child_debug(i, "closing stdin fd ", 0);
            close(0);

            child_debug(i, "closing own write fd ", pd[i * 2 + 1]);
            close(pd[i * 2 + 1]);

            child_debug(i, "dup from read fd ", pd[i * 2 + 0]);
            rc = dup(pd[i * 2 + 0]);
            if (rc < 0)
                fail("dup failed\n");

            child_debug(i, "dup returned fd ", rc);
            child_debug(i, "closing original read fd ", pd[i * 2 + 0]);
            close(pd[i * 2 + 0]);

            child_argv[0] = argv[i + 1];
            execve(child_argv[0], child_argv, NULL);
            fail("execve failed\n");
        }

        parent_debug("closing read fd ", pd[i * 2 + 0]);
        close(pd[i * 2 + 0]);
    }

    while ((bytes = read(0, buf, sizeof(buf))) > 0)
    {
        for (i = 0; i < 2; i++)
        {
            sent = 0;
            while (sent < bytes)
            {
                rc = write(pd[i * 2 + 1], buf + sent, bytes - sent);
                if (rc <= 0)
                    fail("parent write failed\n");
                sent += rc;
            }
        }
    }

    if (bytes < 0)
        fail("parent read failed\n");

    for (i = 0; i < 2; i++)
    {
        close(pd[i * 2 + 1]);
    }

    wait(NULL);
    wait(NULL);
    return 0;
}
