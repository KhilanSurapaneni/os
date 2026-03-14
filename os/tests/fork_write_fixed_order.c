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
    int i;
    int n;
    int rc;
    static char *msgs[] = {
        "child A\n",
        "child B\n",
        "child C\n",
        "child D\n",
        "child E\n",
        "child F\n",
        "child G\n",
        "child H\n"
    };

    if (argc != 2)
        fail("usage: fork_write_fixed_order <num_procs>\n");

    n = atoi(argv[1]);
    if (n <= 0 || n > 8)
        fail("num_procs must be between 1 and 8\n");

    for (i = 0; i < n; i++)
    {
        rc = fork();
        if (rc < 0)
            fail("fork failed\n");

        if (rc == 0)
        {
            char *msg = msgs[i];
            int len = 0;
            while (msg[len] != '\0')
                len++;
            write(1, msg, len);
            exit(0);
        }
    }

    for (i = 0; i < n; i++)
        wait(NULL);

    return 0;
}
