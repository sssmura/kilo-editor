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

/*** defines ***/
// うまく設計されていて、各アルファベットの下５ケタはそのアルファベットに関連する制御文字に対応している。
#define CTRL_KEY(k) ((k) & 0x1f)
#define KILO_VERSION "0.0.1"

// data
// これは行のデータを表している
typedef struct erow {
  int size;
  char *chars;
} erow;
// キーの列挙型だね
enum editorkey {
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  PAGE_UP,
  PAGE_DOWN,
  HOME_KEY,
  END_KEY,
  DEL_KEY
};
// ここにエディタの設定
struct editorConfig {
  // カーソルの座標
  int cx;
  int cy;
  struct termios orig_termios;
  int screenrows;
  int screencols;
  int rowoff;
  int numrows;
  erow *row;
};
// editorの設定をグローバル変数にしてる。
struct editorConfig E;

/*** terminal ***/
// エラーハンドラ
void die(const char *s) {
  //
  write(STDOUT_FILENO, "\x1b[2J", 4); // 全画面消去
  write(STDOUT_FILENO, "\x1b[H", 3);  // 1,1に関数を移動
  perror(s);                          // エラーメッセージ
  exit(1);
}
// ターミナルのRawModeを無効化
void disableRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
    die("tcsetattr");
}
void enableRawMode() {
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
    die("tcgetattr");
  // 終了時にrawmodeを解除する設定
  atexit(disableRawMode);
  struct termios raw = E.orig_termios;
  // man 3 termiosを参照
  // ターミナルの設定をしている
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    die("tcsetattr");
}

int editorReadKey() {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN)
      die("read");
  }
  if (c == '\x1b') {
    char seq[3];
    if (read(STDIN_FILENO, &seq[0], 1) != 1)
      return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1)
      return '\x1b';
    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDOUT_FILENO, &seq[2], 1) != 1)
          return '\x1b';
        if (seq[2] == '~') {
          switch (seq[1]) {
          case '1':
            return HOME_KEY;
          case '3':
            return DEL_KEY;
          case '4':
            return END_KEY;
          case '5':
            return PAGE_UP;
          case '6':
            return PAGE_DOWN;
          case '7':
            return HOME_KEY;
          case '8':
            return END_KEY;
          }
        }
      } else {
        switch (seq[1]) {
        case 'A':
          return ARROW_UP;
        case 'B':
          return ARROW_DOWN;
        case 'C':
          return ARROW_RIGHT;
        case 'D':
          return ARROW_LEFT;
        case 'H':
          return HOME_KEY;
        case 'F':
          return END_KEY;
        }
      }
    } else if (seq[0] == 'O') {
      switch (seq[1]) {
      case 'H':
        return HOME_KEY;
      case 'F':
        return END_KEY;
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
  // CSI Ps n//Ps=6　CSl y;x R でカーソル位置を返す。
  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
    return -1;
  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1)
      break;
    if (buf[i] == 'R')
      break;
    i++;
  }
  // CSl y;x R
  buf[i] = '\0';
  // CSIかのチェック
  if (buf[0] != '\x1b' || buf[0] != '[')
    return -1;
  // scanf
  if (scanf(&buf[2], "%d;%d", rows, cols) != 2)
    return -1;
  return 0;
}

int getWindowsSize(int *rows, int *cols) {
  struct winsize ws;
  // ioctr terminal制御　windowszieの取得
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    // 標準出力に12バイト書き込んでいる。
    // 失敗した場合-1を返す

    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
      return -1;
    return getCursorPosition(rows, cols);
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}
void editorAppendRow(char *s, size_t len) {
  E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
  int at = E.numrows;
  // 最下の列の長さを取得している。// そもそもここには存在しないはずでは？
  E.row[at].size = len;
  // それにプラス1をしてメモリ確保？？--2
  E.row[at].chars = malloc(len + 1);
  // そこにsをコピー
  memcpy(E.row[at].chars, s, len);
  // これが--2の理由か。。。
  E.row[at].chars[len] = '\0';
  E.numrows++;
}
// file io
void editorOpen(char *filename) {
  FILE *fp = fopen(filename, "r");
  if (!fp)
    die("fopen");
  // nullポインタ
  char *line = NULL;
  // lineの長さを保持するための変数
  size_t linecap = 0;
  ssize_t linelen;
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    while (linelen > 0 &&
           // 改行の部分を覗いた長さを求めてる
           (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) {
      linelen--;
    }
    editorAppendRow(line, linelen);
  }
  free(line);
  fclose(fp);
}

// append buffer
struct abuf {
  /* data */
  char *b;
  int len;
};
#define ABUF_INIT                                                              \
  { NULL, 0 }
void abAppend(struct abuf *ab, const char *s, int len) {
  // 動的にメモリ確保
  char *new = realloc(ab->b, ab->len + len);
  if (new == NULL)
    return;
  // コピー
  memcpy(&new[ab->len], s, len);
  // 新しくメモリ確保、そこに一旦コピー、そしてab->bに代入
  // 長さも変更
  ab->b = new;
  ab->len += len;
}
void abFree(struct abuf *ab) { free(ab->b); }
/*** input ***/
void editorMoveCursor(int key) {
  switch (key) {
  case ARROW_LEFT:
    if (E.cx != 0) {
      E.cx--;
    }
    break;
  case ARROW_RIGHT:
    if (E.cx != E.screencols - 1) {
      E.cx++;
    }
    break;
  case ARROW_UP:
    if (E.cy != 0) {
      E.cy--;
    }
    break;
  case ARROW_DOWN:
    if (E.cy < E.numrows) {
      E.cy++;
    }
    break;
  }
}
void editorProcessKeyPress() {
  int c = editorReadKey();
  switch (c) {
  case CTRL_KEY('q'):
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    exit(0);
    break;
  case HOME_KEY:
    E.cx = 0;
    break;
  case END_KEY:
    E.cy = E.screencols - 1;
    break;
  case PAGE_UP:
  case PAGE_DOWN: {
    int times = E.screenrows;
    while (times--) {
      editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
    }
  } break;
  case ARROW_LEFT:
  case ARROW_RIGHT:
  case ARROW_UP:
  case ARROW_DOWN:
    editorMoveCursor(c);
    break;
  }
}
void ediotorScroll() {
  if (E.cy < E.rowoff) {
    E.rowoff = E.cy;
  }
  if (E.cy >= E.rowoff + E.screenrows) {
    E.rowoff = E.cy - E.screenrows + 1;
  }
}
// output
// 行を書いていく
// y:現在の行index
// 1
// E.rowoff ユーザーがスクロールした文を加味したoffset
// filerow 正味の先頭行
// E.numsrow:ファイルの行数

void editorDrawRows(struct abuf *ab) {
  int y;
  for (y = 0; y < E.screenrows; y++) { // 1スクリーンの最下部まで繰り返す
    // 実際のファイルの何行目かを表す
    int filerow = y + E.rowoff;
    // ファイルの最下部以下のとき
    if (filerow >= E.numrows) { // ファイルの最下部より下からの範囲
      //
      if (E.numrows == 0 &&
          y ==
              E.screenrows /
                  3) { // (ファイルの行数が0かつ=ファイルが存在しないor空ファイル)かつ(現在書き込みしようとしている
        // 行が全体の1/3場合)にwelocome messageを表示する。
        char welcome[80];
        int welcomelen = snprintf(welcome, sizeof(welcome),
                                  "Kilo editor -- version %s", KILO_VERSION);
        if (welcomelen > E.screencols) // 列が横幅を超える場合
          welcomelen = E.screencols;   // 長さを列の長さに落とす
        int padding = (E.screencols - welcomelen) /
                      2; // 列とmsgの長さの差をpaddingとする（左側だけなので1/2)
        if (padding) {          // paddingがあるなら
          abAppend(ab, "~", 1); // 左端にはチルダ（共通)
          padding--;
        }
        while (padding--)       // 左端を覗いたpaddingに空白
          abAppend(ab, " ", 1); // 空白で埋める
        abAppend(ab, welcome, welcomelen);
      } else { // ファイルが存在する、または、列の1/3の位置でないとき、左端にはチルダだけ表示
        abAppend(ab, "~", 1);
      }
    } else { // ファイルの最下部までの範囲
             // 単純にファイルを描画する
      int len = E.row[filerow].size;
      if (len > E.screencols)
        len = E.screencols;
      abAppend(ab, E.row[filerow].chars, len);
    }
    // カーソルの右側を削除
    abAppend(ab, "\x1b[K", 3);
    // In last line,new line
    if (y < E.screenrows - 1) {
      abAppend(ab, "\r\n", 2);
    }
  }
}

void editorRefreshScreen() {
  ediotorScroll();
  struct abuf ab = ABUF_INIT;
  //
  abAppend(&ab, "\x1b[?25l", 6); // カーソルを非表示を解除
  abAppend(&ab, "\x1b[H", 3);    // 左上にカーソルを移動する
  editorDrawRows(&ab);
  char buf[32];
  // CSI cy+1;cx+1 H
  // カーソルの位置にカーソルを表示
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
  abAppend(&ab, buf, strlen(buf));
  // CSI 25 h (カーソルを非表示)
  abAppend(&ab, "\x1b[?25h", 6);
  // ここで実際に描写
  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}
/*** init ***/
void initEditor() {
  E.cx = 0;
  E.cy = 0;
  E.rowoff = 0;
  E.numrows = 0;
  E.row = NULL;
  if (getWindowsSize(&E.screenrows, &E.screencols) == -1)
    die("getWindowSize");
}
int main(int argc, char *argv[]) {

  enableRawMode();
  initEditor();
  if (argc >= 2) {
    editorOpen(argv[1]);
  }
  while (1) {
    editorRefreshScreen();
    editorProcessKeyPress();
  }

  return 0;
}
