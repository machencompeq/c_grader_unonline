/* ¤¤¤åµù¸Ñ (Big5) */
#include <stdio.h>

int main(void)
{
    int n, i, x, sum = 0, max = 0;

    scanf("%d", &n);
    for (i = 0; i < n; i++) {
        scanf("%d", &x);
        sum += x;
        if (i == 0 || x > max)
            max = x;
    }
    printf("Sum: %d\n", sum);
    printf("Max: %d\n", max);
    printf("Average: %.2f\n", (double)sum / n);
    return 0;
}
