/*** includes ***/

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
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

void sigpause(const char *msg) {
    if (msg != NULL)
        printf("Wait SIGUSR1: %s\r\n", msg);
    pause();
}


void debug() {
    // sigpause()でSIGUSR1が受けるまで止めるようにする
    signal(SIGUSR1, handleSIGUSR1);
}

/*** data ***/

struct editorConfig {
    int cx, cy;
    int screenrows;
    int screencols;
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

void editorDrawRows(struct abuf *ab) {
    int y;
    for (y = 0; y < E.screenrows; y++) {
        if (y == E.screenrows / 3) {
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

        // カーソルの右側を削除 : EL – Erase In Line
        abAppend(ab, "\x1b[K", 3);

        // 最後の行以外は改行を入れる
        if (y < E.screenrows - 1) {
            abAppend(ab, "\r\n", 2);
        }
    }
}

// @see: https://vt100.net/docs/vt100-ug/chapter3.html
void editorRefreshScreen() {
    struct abuf ab = ABUF_INIT;
    // struct abuf ab = { .b = NULL, .len = 0 };
    // The \x1b is the ASCII escape character (hexadecimal value 0x1b = ESC) 
    // \xXX で1バイト
    // カーソルを隠す : RM – Reset Mode
    // ESC [ Ps ; Ps ; . . . ; Ps l 	default value: none
    // 25 = cursor
    abAppend(&ab, "\x1b[?25l", 6);
    // // コンソール全体をクリア : ED – Erase In Display
    // abAppend(&ab, "\x1b[2J", 4);
    // カーソルを左上に移動 : CUP – Cursor Position
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);

    // カーソル位置を現在の位置に移動
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1); // VT100は1から始まる
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
        if (E.cx != E.screencols - 1)
            E.cx++;
        break;
    case ARROW_UP:
        if (E.cy != 0)
            E.cy--;
        break;
    case ARROW_DOWN:
        if (E.cy != E.screenrows - 1)
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

    if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

int main() {
    debug();
    enableRawMode();
    initEditor();

    while (1) {
        // スクリーンに文字を描画
        editorRefreshScreen();
        // キーを待ち受け
        editorProcessKeypress();
    }
    return 0;
}
