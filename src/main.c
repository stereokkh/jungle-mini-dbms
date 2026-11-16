#include <stdio.h>
#include <string.h>

/* 임시 REPL 뼈대: 한 줄씩 읽어서 .exit 이면 종료한다. */
int main(void) {
    char line[1024];

    for (;;) {
        printf("db > ");
        fflush(stdout);
        if (fgets(line, sizeof line, stdin) == NULL) {
            break;
        }
        line[strcspn(line, "\n")] = '\0';
        if (strcmp(line, ".exit") == 0) {
            break;
        }
        printf("Unrecognized command '%s'.\n", line);
    }
    return 0;
}
