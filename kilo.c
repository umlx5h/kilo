// MEMO: getline(3)で必要
#ifdef __linux__
#define _POSIX_C_SOURCE 200809L
#endif


#include <termios.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <signal.h>


// TODO: yet
struct editorSyntax {
    int dummy;
};


// TODO: yet
/* This structure represents a single line of the file we are editing. */
typedef struct erow {
    int dummy;
} erow;

// TODO: yet
struct editorConfig {
    int cx,cy;  /* Cursor x and y position in characters */
    int rowoff;     /* Offset of row displayed. */
    int coloff;     /* Offset of column displayed. */
    int screenrows; /* Number of rows that we can show */
    int screencols; /* Number of cols that we can show */
    int numrows;    /* Number of rows */
    //
    // MEMO: これは配列だが型を見るだけで区別することは不可能
    // MEMO: 区別方法: XXallocで確保している所を見る or 配列へのアクセスの仕方をみる (->ならポインタ、[]使ってたら配列)
    erow *row;      /* Rows */
    int dirty;      /* File modified but not saved. */
    char *filename; /* Currently open filename */
    // 
    
    struct editorSyntax *syntax; /* Current syntax highlight, or NULL. */
};

static struct editorConfig E;

/* Raw mode: 1960 magic shit. */
int enableRawMode(int fd) {
    struct termios raw;
    return 0;
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

/* Insert a row at the specified position, shifting the other rows on the bottom
 * if required. */
void editorInsertRow(int at, char *s, size_t len) {
    // printf("line: %.*s\n", len, s);
    // TODO: ? 引数と同じ値を比較している？？
    if (at > E.numrows) return;
    // MEMO: realloc(NULL, size)はmalloc(size)と等価になる (初回)
    // MEMO: reallocを使って配列を動的確保したい倍は要素数と1構造体当たりのサイズをかける
    E.row = realloc(E.row,sizeof(erow)*(E.numrows+1));
    // TODO: ? 引数と同じ値を比較している？？
    if (at != E.numrows) {
    }
    E.row->dummy;
    E.row[0].dummy;
}

/* Load the specified program in the editor memory and returns 0 on success
 * or 1 on error. */
int editorOpen(char *filename) {
    FILE *fp;

    E.dirty = 0;
    free(E.filename);
    size_t fnlen = strlen(filename)+1; // NULL文字は入らないので足す
    E.filename = malloc(fnlen);
    memcpy(E.filename,filename,fnlen);

    fp = fopen(filename,"r");
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
    while((linelen = getline(&line,&linecap,fp)) != -1) {
        // MEMO: 1行読み込めた場合, lineは改行文字分を含むがNULL文字は含まない
        if (linelen && (line[linelen-1] == '\n' || line[linelen-1] == '\r'))
            line[--linelen] = '\0'; // 末尾の改行文字を削除する
        editorInsertRow(E.numrows,line,linelen);
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
void editorRefreshScreen(void) {
    ;
}

// MEMO: complete
void updateWindowSize(void) {
    if (getWindowSize(STDIN_FILENO,STDOUT_FILENO,
                      &E.screenrows,&E.screencols) == -1) {
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
    setbuf(stdout, NULL);
    if (argc != 2) {
        fprintf(stderr,"Usage: kilo <filename>\n");
        exit(1);
    }
    initEditor();
    editorOpen(argv[1]);
    enableRawMode(STDIN_FILENO);

    // for (;;) {
        // pause();
    printf("rows: %d, column: %d\n", E.screenrows, E.screencols);
    // }

    return 0;
}
