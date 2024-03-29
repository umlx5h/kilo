/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <signal.h>

/*** defines ***/

#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8
#define KILO_QUIT_TIMES 1

// Ctrl+X を制御文字に変換する 6, 7bitを落とすと変換できる
#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
    BACKSPACE = 127,
    // ここから番号は適当に割り当て
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

enum editorHighlight {
    HL_NORMAL = 0,
    HL_NUMBER,
    HL_MATCH
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
    int size;    // 行の文字数 NULL文字も改行文字も入らない charsのサイズ
    int rsize;   // タブなど特殊文字を含めた文字数 renderとhlのサイズ
    char *chars; // 行の文字列 NULL文字は入るが改行文字は入らない
    char *render;

    // hl is an array of unsigned char values, meaning integers in the range of
    // 0 to 255. Each value in the array will correspond to a character in
    // render, and will tell you whether that character is part of a string, or
    // a comment, or a number, and so on.
    unsigned char *hl; // highlight
} erow;

struct editorConfig {
    int cx, cy;     // テキストファイルに対してのカーソル位置, cx: 列, cy: 行
    int rx;         // 画面描画上のファイルに対してのカーソル位置, conputed valueでcxから算出されるので更新不要
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
    int dirty; // ファイルが編集されたかどうか
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
    struct termios orig_termios;
};

struct editorConfig E;

/*** prototypes ***/
void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt, void (*callback)(char *, int));


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

/*** syntax highlighting ***/

int is_separator(int c) {
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

void editorUpdateSyntax(erow *row) {
    row->hl = realloc(row->hl, row->rsize);
    memset(row->hl, HL_NORMAL, row->rsize);

    // 変数名の数値などを除外しつつハイライトを設定
    int prev_sep = 1;

    int i = 0;
    while (i < row->rsize) {
        char c = row->render[i];
        unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;

        if ((isdigit(c) && (prev_sep || prev_hl == HL_NUMBER)) ||
            (c == '.' && prev_hl == HL_NUMBER)) {
            row->hl[i] = HL_NUMBER;
            i++;
            prev_sep = 0;
            continue;
        }

        prev_sep = is_separator(c);
        i++;
    }
}

// row->hlをANSI colorに変換する
// ref: https://en.wikipedia.org/wiki/ANSI_escape_code#SGR_(Select_Graphic_Rendition)_parameters
int editorSyntaxToColor(int hl) {
    switch (hl) {
        case HL_NUMBER: return 31; // red
        case HL_MATCH: return 34; // blue
        default: return 37; // white
    }
}

/*** row operations ***/

int editorRowCxToRx(erow *row, int cx) {
    int rx = 0;
    int j;
    for (j = 0; j < cx; j++) {
        if (row->chars[j] == '\t')
            rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
        rx++;
    }
    return rx;
}

int editorRowRxToCx(erow *row, int rx) {
    int cur_rx = 0;
    int cx;
    // rxからcxに戻すため、rx -> cxから同じ計算を行い同じrxになった地点でのcxを返す
    for (cx = 0; cx < row->size; cx++) {
        if (row->chars[cx] == '\t')
            cur_rx += (KILO_TAB_STOP - 1) - (cur_rx % KILO_TAB_STOP);
        cur_rx++;

        if (cur_rx > rx) return cx;
    }
    // 想定される以上の不正なrxを指定された場合はここに来る
    return cx;
}

void editorUpdateRow(erow *row) {
    int tabs = 0;
    int j;
    for (j = 0; j < row->size; j++)
        if (row->chars[j] == '\t') tabs++;

    free(row->render);
    row->render = malloc(row->size + tabs*(KILO_TAB_STOP - 1) + 1); // タブ文字を考えて最大限の文字数を確保する

    int idx = 0;
    for (j = 0; j < row->size; j++) {
        // TAB文字をスペース8つに変換
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % KILO_TAB_STOP != 0) row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx; // タブの文字数とかの分がsizeより増えることになる

    // syntax highlight (色づけ)のためのデータを更新する
    editorUpdateSyntax(row);
}

void editorInsertRow(int at, char *s, size_t len) {
    if (at < 0 || at > E.numrows)
        return;

    // 行をappendする
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1); // 1バイトはnull文字
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    E.row[at].hl = NULL;
    editorUpdateRow(&E.row[at]);

    E.numrows++;
    E.dirty++;
}

void editorFreeRow(erow *row) {
    free(row->render);
    free(row->chars);
    free(row->hl);
}

void editorDelRow(int at) {
    if (at < 0 || at >= E.numrows)
        return;
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
    E.numrows--;
    E.dirty++;
}

// ここの *rowは配列ではなく構造体へのポインタ
void editorRowInsertChar(erow *row, int at, int c) {
    if (at < 0 || at > row->size)
        at = row->size;
    // memmoveを使って文字列に文字を挿入する、memcpyだとoverlapしているので問題になるのでmemmoveを使う
    row->chars = realloc(row->chars, row->size + 2); // 1文字+NULLバイト
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowDelChar(erow *row, int at) {
    if (at < 0 || at >= row->size)
        return;
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

/*** editor operations ***/

void editorInsertChar(int c) {
    if (E.cy == E.numrows) {
        // 最後の行の場合は空白を挿入
        editorInsertRow(E.numrows, "", 0);
    }
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

void editorInsertNewLine() {
    if (E.cx == 0) {
        // 行頭の場合空行をinsert
        editorInsertRow(E.cy, "", 0);
    } else {
        erow *row = &E.row[E.cy];
        // 現在のカーソル位置から右側を取り出し下の行に挿入する
        editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);

        // カーソル位置の行を切り詰める
        row = &E.row[E.cy]; // reallocでアドレスが変わっている可能性があるので再代入
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    E.cy++;
    E.cx = 0;
}

void editorDelChar() {
    if (E.cy == E.numrows) // 末尾の場合は行がまだないのでスキップ
        return;
    if (E.cx == 0 && E.cy == 0) // 一番上の場合は上の行がないのでスキップ
        return;

    erow *row = &E.row[E.cy];
    if (E.cx > 0) {
        // カーソル位置の左の文字を消すので-1している
        editorRowDelChar(row, E.cx - 1);
        E.cx--;
    } else {
        // 行頭の場合は上の行にコピーしつつ行を削除
        E.cx = E.row[E.cy - 1].size; // 上の行の末尾に移動
        editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size); // 上の行の末尾に今の行をコピー
        editorDelRow(E.cy);
        E.cy--;
    }
}

/*** file i/o ***/

// 構造体に保存している*rowから1つの大きな文字列を作って返す
char *editorRowsToString(int *buflen) {
    int totlen = 0;
    int j;
    for (j = 0; j < E.numrows; j++)
        totlen += E.row[j].size + 1; // 改行文字
    *buflen = totlen;

    char *buf = malloc(totlen);
    char *p = buf;
    for (j = 0; j < E.numrows; j++) {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        // 改行を挿入
        *p = '\n';
        p++;
    }

    return buf;
}

void editorOpen(char *filename) {
    // これ必要か？
    free(E.filename);
    E.filename = strdup(filename);
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
        editorInsertRow(E.numrows, line, linelen);
    }
    // MEMO: EOFとエラーを区別したいときはferror(3)かfeof(3)を使うらしい
    if (ferror(fp))
        die("Unable to read line from file");
    free(line);
    fclose(fp);
    E.dirty = 0;
}

void editorSave() {
    if (E.filename == NULL) {
        E.filename = editorPrompt("Save as: %s (ESC to cancel)", NULL);
        if (E.filename == NULL) {
            editorSetStatusMessage("Save aborted");
            return;
        }
    }

    // 1つの大きなバッファに文字列を作りファイルにそのまま書き込む
    int len;
    char *buf = editorRowsToString(&len);

    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1) {
        if (ftruncate(fd, len) != -1) {
            if (write(fd, buf, len) == len) {
                close(fd);
                free(buf);
                E.dirty = 0;
                editorSetStatusMessage("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }
    free(buf);
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/** find ***/

void editorFindCallback(char *query, int key) {
    static int last_match = -1; // the index of the row that the last match was on
    static int direction = 1; // 1: forward, -1: backward

    // 以前マッチした箇所のマッチ前のハイライト
    static int saved_hl_line;
    static char *saved_hl = NULL;

    // 以前マッチしたものが存在すれば、その箇所のハイライトを元のものに復元する
    if (saved_hl) {
        memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].rsize);
        free(saved_hl);
        saved_hl = NULL;
    }

    if (key == '\r' || key == '\x1b') {
        // reset state
        last_match = -1;
        direction = 1;
        return;
    } else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
        // search forward
        direction = 1;
    } else if (key == ARROW_LEFT || key == ARROW_UP) {
        // search backward
        direction = -1;
    } else {
        // 検索ワードが変化した場合は前方検索に戻しつつstateをreset
        last_match = -1;
        direction = 1;
    }

    if (last_match == -1) direction = 1;
    int current = last_match;
    int i;
    for (i = 0; i < E.numrows; i++) {
        current += direction;

        // search wrap
        if (current == -1) current = E.numrows - 1; // 一番上にいったので一番下に移動
        else if (current == E.numrows) current = 0; // 一番下にいったので一番上に移動

        erow *row = &E.row[current];
        char *match = strstr(row->render, query);
        if (match) {
            last_match = current;
            E.cy = current;
            // E.cy = i;
            E.cx = editorRowRxToCx(row, match - row->render);
            // 検索結果が画面の一番上になるように設定する
            E.rowoff = E.numrows;

            // ハイライト書き換える前の状態をstatic変数に保存しておく
            saved_hl_line = current;
            saved_hl = malloc(row->rsize);
            memcpy(saved_hl, row->hl, row->rsize);

            // マッチ箇所に色をつける
            memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
            break;
        }
    }
}

void editorFind() {
    int saved_cx = E.cx;
    int saved_cy = E.cy;
    int saved_coloff = E.coloff;
    int saved_rowoff = E.rowoff;

    char *query = editorPrompt("Search: %s (Use ESC/Arrows/Enter)", editorFindCallback);

    if (query) {
        free(query);
    } else {
        // ESCで抜けた場合は、カーソル位置などを復元
        E.cx = saved_cx;
        E.cy = saved_cy;
        E.coloff = saved_coloff;
        E.rowoff = saved_rowoff;
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

void abAppendStr(struct abuf *ab, const char *s) {
    int len;
    len = strlen(s);
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
    E.rx = 0;
    if (E.cy < E.numrows) {
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx); // タブ文字などを考慮したカーソル位置をcxから作る
    }

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
    if (E.rx < E.coloff) {
        E.coloff = E.rx;
    }

    // スクリーンより右にカーソル移動しようとしているので、オフセット位置を調整(増やす)
    if (E.rx >= E.coloff + E.screencols) {
        E.coloff = E.rx - E.screencols + 1;
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
            int len = E.row[filerow].rsize - E.coloff; // 水平スクロールのため調整
            if (len < 0) len = 0;
            // 行の横幅がスクリーンを超えていたら切り詰める
            if (len > E.screencols) len = E.screencols;

            char *c = &E.row[filerow].render[E.coloff]; // 水平スクロールのためcoloff文ずらして表示
            unsigned char *hl = &E.row[filerow].hl[E.coloff];
            int current_color = -1; // 現在の色のステート -1は色未設定
            int j;
            for (j = 0; j < len; j++) {
                if (hl[j] == HL_NORMAL) {
                    if (current_color != -1) {
                        // 色が設定されていたらreset
                        abAppend(ab, "\x1b[39m", 5); // reset to normal color: ESC[39m
                        current_color = -1;
                    }
                    abAppend(ab, &c[j], 1);
                } else {
                    // ANSIエスケープシーケンスでテキストに色を付ける
                    // ref: https://en.wikipedia.org/wiki/ANSI_escape_code#SGR_(Select_Graphic_Rendition)_parameters
                    int color = editorSyntaxToColor(hl[j]);
                    if (color != current_color) {
                        // 色が違う時だけエスケープシーケンスを送る
                        current_color = color;
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color); // 8 color: ESC3[0-7]m
                        abAppend(ab, buf, clen);
                        // abAppendStr(ab, "\x1b[38;5;219m"); // 256colorは ESC[38;5;Nm で Nのところに0-255の数値を入れる
                    }
                    abAppend(ab, &c[j], 1);
                }
            }
            abAppend(ab, "\x1b[39m", 5); // reset color
        }

        // カーソルの右側を削除 : EL – Erase In Line
        abAppend(ab, "\x1b[K", 3);

        // 最後の行以外は改行を入れる
        abAppend(ab, "\r\n", 2);
        // TODO: delete
        // abFlush(ab, 1);
    }
}

void editorDrawStatusBar(struct abuf *ab) {
    // 色を反転 (背景)
    // SGR – Select Graphic Rendition
    // 7 	Negative (reverse) image
    abAppend(ab, "\x1b[7m", 4);
    char status[80], rstatus[80];
    // ファイル名と行数を描画
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
        E.filename ? E.filename : "[No Name]", E.numrows,
        E.dirty ? "(modified)" : "");

    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",
        E.cy + 1, E.numrows);
    if (len > E.screencols)
        len = E.screenrows;
    abAppend(ab, status, len);

    // 残りのスペースを埋める
    while (len < E.screencols) {
        // 右端に位置していた場合に編集行の情報を描画
        if (E.screencols - len == rlen) {
            abAppend(ab, rstatus, rlen);
            break;
        } else {
            abAppend(ab, " ", 1);
            len++;
        }
    }
    // 戻す
    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab) {
    abAppend(ab, "\x1b[K", 3);
    int msglen = strlen(E.statusmsg);
    if (msglen > E.screencols)
        msglen = E.screencols;
    if (msglen && time(NULL) - E.statusmsg_time < 5) // msgが入ってから5秒未満しか経過してないなら描画する
        abAppend(ab, E.statusmsg, msglen);
}

// @see: https://vt100.net/docs/vt100-ug/chapter3.html
// https://en.wikipedia.org/wiki/ANSI_escape_code
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
    abAppend(&ab, "\x1b[?25l", 6);
    // // コンソール全体をクリア : ED – Erase In Display
    // abAppend(&ab, "\x1b[2J", 4);
    // カーソルを左上に移動 : CUP – Cursor Position
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    // カーソル位置を現在の位置に移動
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1,
                                              (E.rx - E.coloff) + 1); // VT100は1から始まる
    abAppend(&ab, buf, strlen(buf));

    // カーソルを表示 : SM – Set Mode
    // ESC [ Ps ; . . . ; Ps h 	default value: none
    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);

    abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);

    E.statusmsg_time = time(NULL);
}

/*** input ***/

// ファイル保存用にステータスバーでファイル名を受け付ける
char *editorPrompt(char *prompt, void (*callback)(char *, int)) {
    size_t bufsize = 128;
    char *buf = malloc(bufsize);
    size_t buflen = 0;
    buf[0] = '\0';

    while (1) {
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int c = editorReadKey();
        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
            // 消せるようにする
            if (buflen != 0) buf[--buflen] = '\0';
        } else if (c == '\x1b') {
            // ESCでキャンセル
            editorSetStatusMessage("");
            if (callback) callback(buf, c);
            free(buf);
            return NULL;
        } else if (c == '\r') {
            // ENTERで確定
            if (buflen != 0) {
                editorSetStatusMessage("");
                if (callback) callback(buf, c);
                return buf;
            }
        } else if (!iscntrl(c) && c < 128) { // if ASCII character (except control character)
            // ASCII文字ならバッファに追加
            if (buflen == bufsize - 1) { // バッファが足らなかったら再確保
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }

        if (callback) callback(buf, c);
    }
}

void editorMoveCursor(int key) {
    // カーソル位置にある行を表す構造体を取得 (末尾の場合はNULL)
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    switch (key) {
    case ARROW_LEFT:
    case CTRL_KEY('b'):
        if (E.cx != 0) {
            E.cx--;
        } else if (E.cy > 0) { // カーソルが行頭かつ先頭行以外の場合は前の行の末尾に移動
            E.cy--;
            E.cx = E.row[E.cy].size;
        }
        break;
    case ARROW_RIGHT:
    case CTRL_KEY('f'):
        if (row && E.cx < row->size) {
            E.cx++;
        } else if (row && E.cx ==  row->size) { // カーソルが末尾の場合は右で次の行の先頭に移動
            E.cy++;
            E.cx = 0;
        }
        break;
    case ARROW_UP:
    case CTRL_KEY('p'):
        if (E.cy != 0)
            E.cy--;
        break;
    case ARROW_DOWN:
    case CTRL_KEY('n'):
        // ファイルの末尾までスクロールを許可
        if (E.cy < E.numrows)
            E.cy++;
        break;
    }


    // 長い行から上下にスクロールされた時にカーソル位置が行のサイズを超えることがあるので、その場合に行の末尾に補正する
    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0; // ファイル末尾の場合一番左にカーソルを強制的に移動
    if (E.cx > rowlen) {
        E.cx = rowlen;
    }
}

void editorProcessKeypress() {
    static int quit_times = KILO_QUIT_TIMES;

    int c = editorReadKey();

    switch (c) {
    case '\r': // Enter
        editorInsertNewLine();
        break;
    case CTRL_KEY('q'):
        if (E.dirty && quit_times > 0) {
            editorSetStatusMessage("WARNING!!! File has unsaved changes. "
                "Press Ctrl-Q %d more times to quit.", quit_times);
            quit_times--;
            return;
        }
        exit(0);
        break;

    case CTRL_KEY('s'):
        editorSave();
        break;

    case ARROW_UP:
    case CTRL_KEY('p'):
    case ARROW_DOWN:
    case CTRL_KEY('n'):
    case ARROW_LEFT:
    case CTRL_KEY('b'):
    case ARROW_RIGHT:
    case CTRL_KEY('f'):
        editorMoveCursor(c);
        break;

    case HOME_KEY:
    case CTRL_KEY('a'):
        E.cx = 0;
        break;

    case END_KEY:
    case CTRL_KEY('e'):
        if (E.cy < E.numrows)
            E.cx = E.row[E.cy].size;
        break;

    case CTRL_KEY('_'): // CTRL-fだとカーソルと被るので"/"に変更
        editorFind();
        break;

    case BACKSPACE:
    case CTRL_KEY('h'):
    case DEL_KEY:
    case CTRL_KEY('d'):
        if (c == DEL_KEY || c == CTRL_KEY('d')) editorMoveCursor(ARROW_RIGHT);
        editorDelChar();
        break;

    case PAGE_UP:
    case PAGE_DOWN:
        {
            if (c == PAGE_UP) {
                E.cy = E.rowoff;
            } else if (c == PAGE_DOWN) {
                E.cy = E.rowoff + E.screenrows - 1;
                if (E.cy > E.numrows) E.cy = E.numrows;
            }

            int times = E.screenrows;
            while (times--)
                editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
        }
        break;

    case CTRL_KEY('l'):
    case '\x1b':
        break;

    default:
        editorInsertChar(c);
        break;
    }

    quit_times = KILO_QUIT_TIMES;
}

/*** init ***/

void initEditor() {
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;

    if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
    E.screenrows -= 2;
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

    editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-/ = find");

    while (1) {
        // スクリーンに文字を描画
        editorRefreshScreen();
        // debugScreen();
        // キーを待ち受け
        editorProcessKeypress();
    }
    return 0;
}

// vim:norelativenumber:
