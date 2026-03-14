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

static int append_str(char *buf, int pos, const char *s)
{
    int i = 0;
    while (s[i] != '\0')
        buf[pos++] = s[i++];
    return pos;
}

static int append_num(char *buf, int pos, int x)
{
    char tmp[32];
    int n = 0;
    int i;

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
        tmp[n++] = (char)('0' + (x % 10));
        x /= 10;
    }

    for (i = n - 1; i >= 0; i--)
        buf[pos++] = tmp[i];

    return pos;
}

static void write_reap_line(int pid, int status)
{
    char buf[128];
    int pos = 0;

    pos = append_str(buf, pos, "reaped pid ");
    pos = append_num(buf, pos, pid);
    pos = append_str(buf, pos, " status ");
    pos = append_num(buf, pos, status);
    pos = append_str(buf, pos, "\n");
    write(1, buf, pos);
}

int main(int argc, char **argv)
{
    int i;
    int n;
    int rc;
    int status;

    if (argc != 2)
        fail("usage: fork_exit_order <num_procs>\n");

    n = atoi(argv[1]);
    if (n <= 0)
        fail("num_procs must be > 0\n");

    for (i = 0; i < n; i++)
    {
        rc = fork();
        if (rc < 0)
            fail("fork failed\n");

        if (rc == 0)
            exit(i + 1);
    }

    for (i = 0; i < n; i++)
    {
        rc = wait(&status);
        if (rc < 0)
            fail("wait failed\n");
        write_reap_line(rc, status >> 8);
    }

    return 0;
}
