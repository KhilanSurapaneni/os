#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include "dllist.h"
#include "kt.h"
#include "simulator_lab2.h"
#include "console_buf.h"

// write semaphores
kt_sem writeok;
kt_sem writers;

// read semaphores
kt_sem consoleWait;
kt_sem nelem;
kt_sem nslots;

#define CBUF_SIZE 256
static int cbuf[CBUF_SIZE];
static int head = 0; // next item to remove
static int tail = 0; // next slot to insert

/*
    - reads console for input
    - pulls characters for console into a buffer
*/
void *ConsoleReader(void *arg)
{
    (void)arg;

    while (1)
    {
        // wait for hardware to say "a char is ready"
        P_kt_sem(consoleWait);

        // wait for free slot
        P_kt_sem(nslots);

        // get char from device
        int ch = console_read(); // returns int, -1 for EOF

        // put into circular buffer
        cbuf[tail] = ch;
        tail = (tail + 1) % CBUF_SIZE;

        // tells consumers there's more than 1 element
        V_kt_sem(nelem);
    }

    return NULL;
}

/*
    - debug function for the console buffer
*/
void DumpConsoleBuffer(void)
{
    printf("\n--- console buffer dump ---\n");

    // figure out how full the buffer is
    int free_slots = kt_getval(nslots);
    if (free_slots < 0)
        free_slots = 0;
    if (free_slots > CBUF_SIZE)
        free_slots = CBUF_SIZE;

    // computing how many chars are stored
    int count = CBUF_SIZE - free_slots; // how many chars were inserted
    if (count < 0)
        count = 0;
    if (count > CBUF_SIZE)
        count = CBUF_SIZE;

    // walk through buffer and print each char
    int idx = head;
    for (int i = 0; i < count; i++)
    {
        int ch = cbuf[idx];
        if (ch == -1)
        {
            printf("[EOF]");
        }
        else if (ch == '\n')
        {
            printf("\\n");
        }
        else if (ch == '\r')
        {
            printf("\\r");
        }
        else
        {
            putchar(ch);
        }
        idx = (idx + 1) % CBUF_SIZE;
    }

    printf("\n--- end dump ---\n");
}

/*
    - blocking function that returns one character from buffer
*/
int ConsoleBufGetChar(void)
{
    int ch;

    // wait until there is at least 1 element
    P_kt_sem(nelem);

    // pull from circular buffer
    ch = cbuf[head];
    head = (head + 1) % CBUF_SIZE;

    // free a slot for ConsoleReader
    V_kt_sem(nslots);

    return ch;
}