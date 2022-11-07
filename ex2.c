#include <stdio.h>

int
main(void)
{
    struct foo
    {
        int bar;
    } foo[] = {
        [0] = {.bar = 0},
        [1] = {.bar = 1}
    };

    struct foo *f = foo;

    printf("Here we go\n");

    f->bar++;
    f++;

    return 0;
}
    


