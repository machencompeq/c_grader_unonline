#include <stdio.h>
int main(void) {
    int n, i, x, sum = 0, max = 0;
    scanf("%d", &n);
    for (i = 0; i < n; i++) { scanf("%d", &x); sum += x; if (i == 0 || x > max) max = x; }
    printf("Sum: %d\nMax: %d\nAverage: %.2f", sum, max, (double)sum / n);
    return 0;
}
