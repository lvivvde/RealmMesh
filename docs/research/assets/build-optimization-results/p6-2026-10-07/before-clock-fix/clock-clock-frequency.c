#include <stdio.h>
#include <string.h>
#include <sys/timex.h>
int main(int argc, char **argv) {
    struct timex state = {0};
    if (adjtimex(&state) < 0) { perror("adjtimex read"); return 1; }
    printf("frequency_before_ppm=%.9f status=%d\n", state.freq / 65536.0, state.status);
    if (argc == 2 && strcmp(argv[1], "--zero-frequency") == 0) {
        struct timex update = {0};
        update.modes = ADJ_FREQUENCY;
        update.freq = 0;
        if (adjtimex(&update) < 0) { perror("adjtimex frequency reset"); return 1; }
        state = (struct timex){0};
        if (adjtimex(&state) < 0) { perror("adjtimex verify"); return 1; }
        printf("frequency_after_ppm=%.9f status=%d\n", state.freq / 65536.0, state.status);
        return state.freq != 0;
    }
    return argc != 1;
}
