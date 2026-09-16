#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void trim_newline(char* text) {
    size_t length = strlen(text);
    while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r')) {
        text[--length] = '\0';
    }
}

static int read_int_in_range(int min_value, int max_value) {
    char line[64];

    while (fgets(line, sizeof line, stdin) != NULL) {
        char* end = NULL;
        long value = strtol(line, &end, 10);
        if (end != line && value >= min_value && value <= max_value) {
            return (int)value;
        }
        printf("Please enter a number between %d and %d: ", min_value, max_value);
        fflush(stdout);
    }

    return min_value;
}

int main(void) {
    char name[128];
    int count = 1;

    printf("=== daptor interactive demo (C) ===\n");
    printf("Focus the Console panel, then type your answers here.\n\n");

    printf("Your name: ");
    fflush(stdout);
    if (fgets(name, sizeof name, stdin) == NULL) {
        return 1;
    }
    trim_newline(name);
    if (name[0] == '\0') {
        strcpy(name, "debugger");
    }

    printf("How many greetings (1-5)? ");
    fflush(stdout);
    count = read_int_in_range(1, 5);

    for (int index = 0; index < count; ++index) {
        printf("Hello, %s! (%d/%d)\n", name, index + 1, count);
    }

    char answer[64];
    printf("Type 'quit' to exit early, or press Enter to finish: ");
    fflush(stdout);
    if (fgets(answer, sizeof answer, stdin) == NULL) {
        return 1;
    }
    trim_newline(answer);

    if (strcmp(answer, "quit") == 0) {
        printf("Goodbye!\n");
    } else {
        printf("Done.\n");
    }

    return 0;
}
