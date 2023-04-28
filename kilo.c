// MEMO: getline(3)で必要
#ifdef __linux__
#define _POSIX_C_SOURCE 200809L
#endif

#include <termios.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdarg.h>
#include <signal.h>


// TODO: yet
struct editorSyntax {
    int dummy;
};


// MEMO: complete
/* This structure represents a single line of the file we are editing. */
typedef struct erow {
    int idx;            /* Row index in the file, zero-based. */
    int size;           /* Size of the row, excluding the null term. */
    int rsize;          /* Size of the rendered row. */
    char *chars;        /* Row content. */
    char *render;       /* Row content "rendered" for screen (for TABs). */
    unsigned char *hl;  /* Syntax highlight type for each character in render.*/
    int hl_oc;          /* Row had open comment at end in last syntax highlight
                           check. */
} erow;

// TODO: yet
struct editorConfig {
    int cx,cy;  /* Cursor x and y position in characters */
    int rowoff;     /* Offset of row displayed. */
    int coloff;     /* Offset of column displayed. */
    int screenrows; /* Number of rows that we can show */
    int screencols; /* Number of cols that we can show */
    int numrows;    /* Number of rows */
    int rawmode;    /* Is terminal raw mode enabled? */
    // MEMO: これは配列だが型を見るだけで区別することは不可能
    // MEMO: 区別方法: XXallocで確保している所を見る or 配列へのアクセスの仕方をみる (->ならポインタ、[]使ってたら配列)
    erow *row;      /* Rows */
    int dirty;      /* File modified but not saved. */
    char *filename; /* Currently open filename */
    char statusmsg[80];
    time_t statusmsg_time;
    
    // MEMO: こっちは配列じゃなくて構造体へのポインタだった
    struct editorSyntax *syntax; /* Current syntax highlight, or NULL. */
};

static struct editorConfig E;

void editorSetStatusMessage(const char *fmt, ...);

/* ======================= Low level terminal handling ====================== */

static struct termios orig_termios; /* In order to restore at exit.*/

// MEMO: complete
void disableRawMode(int fd) {
    /* Don't even check the return value as it's too late. */
    if (E.rawmode) {
        tcsetattr(fd, TCSAFLUSH, &orig_termios);
        E.rawmode = 0;
    }
}

// MEMO: complete
/* Called at exit to avoid remaining in raw mode. */
void editorAtExit(void) {
    disableRawMode(STDIN_FILENO);
}

// MEMO: complete
/* Raw mode: 1960 magic shit. */
int enableRawMode(int fd) {
    struct termios raw;

    if (E.rawmode) return 0; /* Already enabled. */
    if (!isatty(STDIN_FILENO)) goto fatal;
    atexit(editorAtExit);
    if (tcgetattr(fd, &orig_termios) == -1) goto fatal;

    raw = orig_termios; /* modify the original mode */
    /* input modes: no break, no CR to NL, no parity check, no strip char,
     * no start/stop output control. */
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    /* output modes - disable post processing */
    raw.c_oflag &= ~(OPOST);
    /* control modes - set 8 bit chars */
    raw.c_cflag |= (CS8);
    /* local modes - echoing off, canonical off, no extended functions,
     * no signal chars (^Z,^C) */
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    /* control chars - set return condition: min number of bytes and timer. */
    raw.c_cc[VMIN] = 0; /* Return each byte, or zero for timeout. */
    raw.c_cc[VTIME] = 1; /* 100 ms timeout (unit is tens of second). */

    /* put terminal in raw mode after flushing */
    if (tcsetattr(fd, TCSAFLUSH, &raw) < 0) goto fatal;
    E.rawmode = 1;
    return 0;

fatal:
    errno = ENOTTY;
    return -1;    
}

/* Try to get the number of columns in the current terminal. If the ioctl()
 * call fails the function will try to query the terminal itself.
 * Returns 0 on success, -1 on error. */
int getWindowSize(int ifd, int ofd, int *rows, int *cols) {
    struct winsize ws;

    if (ioctl(1, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        /* ioctl() failed. Try to query the terminal itself. */
        // TODO: yet
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/* ======================= Editor rows implementation ======================= */

/* Update the rendered version and the syntax highlight of a row. */
void editorUpdateRow(erow *row) {
}

/* Insert a row at the specified position, shifting the other rows on the bottom
 * if required. */
void editorInsertRow(int at, char *s, size_t len) {
    // printf("line: %.*s\n", len, s);
    // TODO: ? yet 引数と同じ値を比較している？？
    if (at > E.numrows) return;
    // MEMO: realloc(NULL, size)はmalloc(size)と等価になる (初回)
    // MEMO: reallocを使って配列を動的確保したい倍は要素数と1要素当たりのサイズをかける
    E.row = realloc(E.row, sizeof(erow)*(E.numrows+1));
    // TODO: ? yet 引数と同じ値を比較している？？
    if (at != E.numrows) {
    }
    E.row[at].size = len;
    // MEMO: このように配列へのポインタの場合はアロー演算子でアクセスできるが、コンパイルできてしまう
    // つまり機械的に見分けることはできない
    // E.row->size = 0;

    E.row[at].chars = malloc(len+1);
    memcpy(E.row[at].chars, s, len+1); // TODO: +1している理由を確認
    E.row[at].hl = NULL;
    E.row[at].hl_oc = 0;
    E.row[at].render = NULL;
    E.row[at].rsize = 0;
    E.row[at].idx = at;
    editorUpdateRow(E.row + at);
    // editorUpdateRow(&E.row[at]);
    E.numrows++;
    E.dirty++;
}

// MEMO: complete
/* Load the specified program in the editor memory and returns 0 on success
 * or 1 on error. */
int editorOpen(char *filename) {
    FILE *fp;

    E.dirty = 0;
    free(E.filename);
    size_t fnlen = strlen(filename)+1; // NULL文字は入らないので足す
    E.filename = malloc(fnlen);
    memcpy(E.filename, filename, fnlen);

    fp = fopen(filename, "r");
    if (!fp) {
        if (errno != ENOENT) {
            perror("Opening file");
            exit(1);
        }
        return 1;
    }

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    // MEMO: getlineはfgetsの動的確保版、必要に応じてreallocで再確保してくれる
    // MEMO: getlineはEOFの場合もエラーも場合も-1を返却する
    while((linelen = getline(&line, &linecap, fp)) != -1) {
        // MEMO: 1行読み込めた場合, lineは改行文字分を含むがNULL文字は含まない
        if (linelen && (line[linelen-1] == '\n' || line[linelen-1] == '\r'))
            line[--linelen] = '\0'; // 末尾の改行文字を削除する
        editorInsertRow(E.numrows, line, linelen);
    }
    // MEMO: EOFとエラーを区別したいときはferror(3)かfeof(3)を使うらしい
    if (ferror(fp)) {
        perror("Unable to read line from file");
        exit(1);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
    return 0;
}


// TODO: yet
/* This function writes the whole screen using VT100 escape characters
 * starting from the logical state of the editor in the global state 'E'. */
void editorRefreshScreen(void) {
    int y;
    erow *r;
    char buf[32];
}

// MEMO: complete
/* Set an editor status message for the second line of the status, at the
 * end of the screen. */
void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}


// TODO: yet
/* Process events arriving from the standard input, which is, the user
 * is typing stuff on the terminal. */
void editorProcessKeypress(int fd) {
}

// MEMO: complete
void updateWindowSize(void) {
    if (getWindowSize(STDIN_FILENO,STDOUT_FILENO,
                      &E.screenrows, &E.screencols) == -1) {
        perror("Unable to query the screen for size (columns / rows)");
        exit(1);
    }
    E.screenrows -= 2; /* Get room for status bar. */
}

// MEMO: complete
// MEMO:               コンパイルでunusedエラーが出るのを回避
void handleSigWinCh(int unused __attribute__((unused))) {
    updateWindowSize();
    // カーソル位置がウィンドウサイズを超えている場合は修正する
    if (E.cy > E.screenrows) E.cy = E.screenrows - 1;
    if (E.cx > E.screencols) E.cx = E.screencols - 1;
    editorRefreshScreen();
}

// MEMO: complete
void initEditor(void) {
    E.cx = 0;
    E.cy = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename = NULL;
    E.syntax = NULL;
    updateWindowSize();
    signal(SIGWINCH, handleSigWinCh);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: kilo <filename>\n");
        exit(1);
    }
    initEditor();
    editorOpen(argv[1]);
    enableRawMode(STDIN_FILENO);
    editorSetStatusMessage(
        "HELP: Ctrl-S = save | Ctrl-X = quit | Ctrl-F = find");

    printf("rows: %d, column: %d\n", E.screenrows, E.screencols);
    while (1) {
        editorRefreshScreen();
        editorProcessKeypress(STDIN_FILENO);
    }

    return 0;
}
