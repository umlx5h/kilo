#include <termios.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

struct editorConfig {

};

static struct editorConfig E;

/* Raw mode: 1960 magic shit. */
int enableRawMode(int fd) {
    struct termios raw;
}

void initEditor(void) {

}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr,"Usage: kilo <filename>\n");
        exit(1);
    }

    initEditor();
    enableRawMode(STDIN_FILENO);

    return 0;
}
