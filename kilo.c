/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#include <signal.h>
#include <assert.h>

/*** defines ***/

#define KILO_VERSION "0.0.1"

// Ctrl+X を制御文字に変換する 6, 7bitを落とすと変換できる
#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

/*** my ***/
void handleSIGUSR1(int unused __attribute__((unused))) {
    ;
}

void sigpause_dbg(const char *msg) {
    if (msg != NULL)
        printf("Wait SIGUSR1: %s\r\n", msg);
    pause();
}


void debug() {
    // sigpause()でSIGUSR1が受けるまで止めるようにする
    signal(SIGUSR1, handleSIGUSR1);
}

/*** data ***/

typedef struct erow {
    int size;    // 行の文字数 NULL文字も改行文字も入らない
    char *chars; // 行の文字列 NULL文字は入るが改行文字は入らない
} erow;

struct editorConfig {
    int cx, cy;     // テキストファイルに対してのカーソル位置, cx: 列, cy: 行
    int rowoff;     // テキストファイルの先頭行から何行スキップするか 垂直方向のスクロールで使用
    int coloff;
    int screenrows;
    int screencols;
    int numrows;    // ファイルの行数 (*row の要素数)
    // 構造体の配列で1要素がファイル行に該当する
    // 配列であるかどうかは型定義だけ見ても判別できない
    // 判別するためには mallocなどの確保の仕方を見るかアクセスの仕方を見る
    // 構造体へのポインタ      malloc(sizeof(struct 構造体))   , struct->member
    // 構造体配列へのポインタ  malloc(sizeof(struct 構造体) * 要素数), struct[0].member
    erow *row;
    struct termios orig_termios;
};

struct editorConfig E;

/*** terminal ***/

void die(const char *s) {
    // エラー出す時にターミナル上の表示を消してカーソルを左上に移動してから出力する
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    
    perror(s);
    exit(1);
}

void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
        die("tcsetattr");
}

void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
    atexit(disableRawMode);

    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

int editorReadKey() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read");
    }

    if (c == '\x1b') {
        // エスケープシーケンス(ESC)から始まる場合はさらに複数バイト読み取る
        char seq[3];

        // ESCの後に少なくとも2バイトあるはずで2バイト読む
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                // 3文字目を読む
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch(seq[1]) {
                        case '1': return HOME_KEY;     // ESC[1~ (in case of tmux)
                        case '3': return DEL_KEY;      // ESC[3~
                        case '4': return END_KEY;      // ESC[4~ (in case of tmux)
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
                

            } else {
                switch (seq[1]) {
                    case 'A': return ARROW_UP; // up      ESC[A
                    case 'B': return ARROW_DOWN; // down
                    case 'C': return ARROW_RIGHT; // right
                    case 'D': return ARROW_LEFT; // left
                    case 'H': return HOME_KEY;   // (windows terminal, vscode, tabby)
                    case 'F': return END_KEY;    // (windows terminal, vscode, tabby)
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return HOME_KEY; // (? enviornment)
                case 'F': return END_KEY;  // (? enviornment)
            }
        }

        return '\x1b';
    } else {
        return c;
    }
}

int getCursorPosition(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;
    // カーソル位置の取得要求
    // DSR – Device Status Report : ESC [ Ps n 	default value: 0
    // 6 	Command from host – Please report active position (using a CPR control sequence)
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    // bufに取り出す
    // src: ESC[30;120R, dst: ESC[30;120
    while (i < sizeof(buf) - 1) {
        // DSRをstdoutに投げるとstdinからカーソル位置を取得できる
        // CPR – Cursor Position Report : ESC [ Pn ; Pn R 	default value: 1
        //          30row 120column
        // e.g. - ESC[30;120R
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';

    // バリデーション
    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    // XX;YY の形式になっているのをパース
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        // ioctlでウィンドウサイズを取れなかった時にフォールバック版

        // スクリーンサイズを取得するため、まずカーソルをスクリーン右下に移動する
        // CUF – Cursor Forward  : ESC [ Pn C 	default value: 1    右に移動
        // CUD – Cursor Down     : ESC [ Pn B 	default value: 1    下に移動
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;

        // カーソル位置のrow, columnを取得
        return getCursorPosition(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** row operations ***/

void editorAppendRow(char *s, size_t len) {
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));

    int at = E.numrows;
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1); // 1バイトはnull文字
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';
    E.numrows++;
}

/*** file i/o ***/

void editorOpen(char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) die("fopen");

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        while (linelen > 0 && (line[linelen - 1] == '\n' ||
                            line[linelen - 1] == '\r')) {
            // 改行文字がlinelenに入っているので、改行があった場合に文字数をその分減らす
            // abc\r\n\0 (linelen = 5) -> linelen = 3になる
            linelen--;                        
        }
        editorAppendRow(line, linelen);
    }
    // MEMO: EOFとエラーを区別したいときはferror(3)かfeof(3)を使うらしい
    if (ferror(fp))
        die("Unable to read line from file");
    free(line);
    fclose(fp);
}

/*** append buffer ***/

struct abuf {
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
    char *new = realloc(ab->b, ab->len + len);

    if (new == NULL) return;
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}


void abFree(struct abuf *ab) {
    free(ab->b);
}

// writeFd にwrite(2)してバッファを再初期化する
void abFlush(struct abuf *ab, int writeFd) {
    write(writeFd, ab->b, ab->len);
    abFree(ab);
    ab->b = NULL;
    ab->len = 0;
}

/*** output ***/
void editorScroll() {
    /* # 垂直スクロール */
    // スクリーンより上にカーソル移動しようとしているので、オフセット位置を調整(減らす)
    if (E.cy < E.rowoff) {
        E.rowoff = E.cy;
    }

    // スクリーンより下にカーソル移動しようとしているので、オフセット位置を調整(増やす)
    if (E.cy >= E.rowoff + E.screenrows) {
        E.rowoff = E.cy - E.screenrows + 1;
    }

    /* # 水平スクロール */
    // スクリーンより左にカーソル移動しようとしているので、オフセット位置を調整(減らす)
    if (E.cx < E.coloff) {
        E.coloff = E.cx;
    }

    // スクリーンより右にカーソル移動しようとしているので、オフセット位置を調整(増やす)
    if (E.cx >= E.coloff + E.screencols) {
        E.coloff = E.cx - E.screencols + 1;
    }
}

void editorDrawRows(struct abuf *ab) {
    int y;
    for (y = 0; y < E.screenrows; y++) {
        int filerow = y + E.rowoff;
        if (filerow >= E.numrows) {
            // ファイル行数以上のターミナル行数があった場合は ~文字を左に出力
            if (E.numrows == 0 && y == E.screenrows / 3) {
                // ファイルを読み込まない場合は中心にWelcomeメッセージを表示する
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome),
                    "Kilo editor -- version %s", KILO_VERSION);
                // welcomeメッセージが横幅を超える場合はtruncate
                if (welcomelen > E.screencols) welcomelen = E.screencols;

                // welcomeメッセージ左の空白部分を作る
                int padding = (E.screencols - welcomelen) / 2;
                if (padding) {
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while (padding--) abAppend(ab, " ", 1);
                abAppend(ab, welcome, welcomelen);
            } else {
                abAppend(ab, "~", 1);
            }
        } else {
            // ファイル内容をスクリーンに出力
            int len = E.row[filerow].size - E.coloff; // 水平スクロールのため調整
            if (len < 0) len = 0;
            // 行の横幅がスクリーンを超えていたら切り詰める
            if (len > E.screenrows) len = E.screencols;
            abAppend(ab, &E.row[filerow].chars[E.coloff], len); // 水平スクロールのためcoloff文ずらして表示
        }

        // カーソルの右側を削除 : EL – Erase In Line
        abAppend(ab, "\x1b[K", 3);

        // 最後の行以外は改行を入れる
        if (y < E.screenrows - 1) {
            abAppend(ab, "\r\n", 2);
        }
        // TODO: delete
        abFlush(ab, 1);
    }
}

// @see: https://vt100.net/docs/vt100-ug/chapter3.html
void editorRefreshScreen() {
    editorScroll();

    struct abuf ab = ABUF_INIT;
    // struct abuf ab = { .b = NULL, .len = 0 };
    // The \x1b is the ASCII escape character (hexadecimal value 0x1b = ESC) 
    // \xXX で1バイト
    // カーソルを隠す : RM – Reset Mode
    // ESC [ Ps ; Ps ; . . . ; Ps l 	default value: none
    // 25 = cursor
    // TODO: 戻す
    // abAppend(&ab, "\x1b[?25l", 6);
    // // コンソール全体をクリア : ED – Erase In Display
    // abAppend(&ab, "\x1b[2J", 4);
    // カーソルを左上に移動 : CUP – Cursor Position
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);

    // カーソル位置を現在の位置に移動
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, 
                                              (E.cx - E.coloff) + 1); // VT100は1から始まる
    abAppend(&ab, buf, strlen(buf));

    // カーソルを表示 : SM – Set Mode
    // ESC [ Ps ; . . . ; Ps h 	default value: none
    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);

    abFree(&ab);
}


/*** input ***/

void editorMoveCursor(int key) {
    switch (key) {
    case ARROW_LEFT:
        if (E.cx != 0)
            E.cx--;
        break;
    case ARROW_RIGHT:
        E.cx++;
        break;
    case ARROW_UP:
        if (E.cy != 0)
            E.cy--;
        break;
    case ARROW_DOWN:
        // ファイルの末尾までスクロールを許可
        if (E.cy < E.numrows)
            E.cy++;
        break;
    }
}

void editorProcessKeypress() {
    int c = editorReadKey();

    switch (c) {
    case CTRL_KEY('q'):
        exit(0);
        break;

    case CTRL_KEY('\\'):
        errno = ERANGE;
        die("dummy exit");
        break;
    
    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
        editorMoveCursor(c);
        break;

    case HOME_KEY:
        E.cx = 0;
        break;

    case END_KEY:
        E.cx = E.screencols - 1;
        break;

    case PAGE_UP:
    case PAGE_DOWN:
        {
            int times = E.screenrows;
            while (times--)
                editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
        }
        break;
    }
}

/*** init ***/

void initEditor() {
    E.cx = 0;
    E.cy = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;

    if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

void debugScreen() {
    // fprintf(stderr, "cx: %d, cy: %d, rowoff: %d, srows: %d, frows: %d\n",
    //     E.cx, E.cy, E.rowoff, E.screenrows, E.numrows);
    fprintf(stderr, "cx: %d, cy: %d, coloff: %d, scols: %d\n",
        E.cx, E.cy, E.coloff, E.screencols);
}

int main(int argc, char *argv[]) {
    debug();
    enableRawMode();
    initEditor();
    if (argc >= 2) {
        editorOpen(argv[1]);
    }

    while (1) {
        // スクリーンに文字を描画
        editorRefreshScreen();
        debugScreen();
        // キーを待ち受け
        editorProcessKeypress();
    }
    return 0;
}
