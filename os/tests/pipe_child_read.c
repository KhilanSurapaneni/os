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

int main(void)
{
    char buf[256];
    int rc;

    while ((rc = read(0, buf, sizeof(buf))) > 0)
    {
        write(1, buf, rc);
    }

    if (rc < 0)
        fail("child read failed\n");

    return 0;
}
