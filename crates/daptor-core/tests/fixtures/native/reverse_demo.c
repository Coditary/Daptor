#include <stdio.h>

static int add(int a, int b) {
    int sum = a + b;
    return sum;
}

int main(void) {
    int x = 10;
    int y = 32;
    int z = add(x, y);
    printf("result: %d\n", z);
    return 0;
}
