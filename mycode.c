#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

/*** includes ***/
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#define STEX_VERSION "0.0.3"
#define CTRL_KEY(k) ((k) & 0x1F)

/*** COLOR STYLING (ANSI) ***/
#define COLOR_RESET   "\x1b[0m"
#define COLOR_LINENUM "\x1b[38;5;242m" // Dim Gray
#define COLOR_MARK    "\x1b[1;33m"     // Bold Yellow
#define COLOR_TILDE   "\x1b[38;5;238m" // Darker Gray

/*** DATA STRUCTURES ***/
typedef struct erow {
  int size;
  char* chars;
} erow;

struct editorConfiguration {
  int cx, cy;
  int rowOffset;
  
  int screencols;
  int screenrows;
  
  int numRows;
  erow *row; 
  char *filename; 

  /* Mark System */
  int markX, markY;
  int hasMark;

  /* Clipboard System */
  char *clipboard;

  struct termios original_termios;
};

struct editorConfiguration E;

enum editorKeys {
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN,
  BACKSPACE_KEY
};

/*** TERMINAL ***/
struct abuf {
  char *b;
  int len;
};

#define ABUF_INIT {NULL, 0}

void die(const char *s) {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);
  perror(s);
  exit(1);
}

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

int keyRead() {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) die("read");
  }

  if (c == '\x1b') {
    char seq[3];
    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
        if (seq[2] == '~') {
          switch (seq[1]) {
            case '1': return HOME_KEY;
            case '3': return DEL_KEY;
            case '4': return END_KEY;
            case '5': return PAGE_UP;
            case '6': return PAGE_DOWN;
            case '7': return HOME_KEY;
            case '8': return END_KEY;
          }
        }
      } else {
        switch (seq[1]) {
          case 'A': return ARROW_UP;
          case 'B': return ARROW_DOWN;
          case 'C': return ARROW_RIGHT;
          case 'D': return ARROW_LEFT;
          case 'H': return HOME_KEY;
          case 'F': return END_KEY;
        }
      }
    } else if (seq[0] == 'O') {
      switch (seq[1]) {
        case 'H': return HOME_KEY;
        case 'F': return END_KEY;
      }
    }
    return '\x1b';
  } else {
    if (c == 127 || c == 8) return BACKSPACE_KEY;
    return c;
  }
}

void exitRawMode() {
  write(STDOUT_FILENO, "\x1b[0 q", 5);
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.original_termios);
}

void enterRawMode() {
  if (tcgetattr(STDIN_FILENO, &E.original_termios) == -1) die("tcgetattr");
  atexit(exitRawMode);

  struct termios raw = E.original_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

int getCursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;
  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;
  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
    if (buf[i] == 'R') break;
    i++;
  }
  buf[i] = '\0';
  if (buf[0] != '\x1b' || buf[1] != '[') return -1;
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;
  return 0;
}

int getWindowSize(int *rows, int *cols) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
    return getCursorPosition(rows, cols);
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

/*** ROW OPERATIONS ***/
void editorAppendRow(char *s, size_t linelen) {
  E.row = realloc(E.row, sizeof(erow) * (E.numRows + 1));
  int at = E.numRows;
  E.row[at].size = linelen;
  E.row[at].chars = malloc(linelen + 1);
  memcpy(E.row[at].chars, s, linelen);
  E.row[at].chars[linelen] = '\0';
  E.numRows++;
}

void editorInsertRow(int at, char *s, size_t len) {
  if (at < 0 || at > E.numRows) return;
  E.row = realloc(E.row, sizeof(erow) * (E.numRows + 1));
  memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numRows - at));
  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';
  E.numRows++;
}

void editorRowAppendString(erow *row, char *s, size_t len) {
  row->chars = realloc(row->chars, row->size + len + 1);
  memcpy(&row->chars[row->size], s, len);
  row->size += len;
  row->chars[row->size] = '\0';
}

void editorDelRow(int at) {
  if (at < 0 || at >= E.numRows) return;
  free(E.row[at].chars);
  memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numRows - at - 1));
  E.numRows--;
}

void editorRowInsertChar(erow *row, int at, int c) {
  if (at < 0 || at > row->size) at = row->size;
  row->chars = realloc(row->chars, row->size + 2);
  memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
  row->size++;
  row->chars[at] = c;
}

void editorInsertNewLine() {
  if (E.cx == 0) {
    editorInsertRow(E.cy, "", 0);
  } else {
    erow *row = &E.row[E.cy];
    editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
    row = &E.row[E.cy];
    row->size = E.cx;
    row->chars[row->size] = '\0';
  }
  E.cy++;
  E.cx = 0;
}

void editorInsertChar(int c) {
  if (c == '\n') {
    editorInsertNewLine();
    return;
  }
  if (E.cy == E.numRows) {
    editorAppendRow("", 0);
  }
  editorRowInsertChar(&E.row[E.cy], E.cx, c);
  E.cx++;
}

void editorRowDelChar(erow *row, int at) {
  if (at < 0 || at >= row->size) return;
  memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
  row->size--;
}

void editorDelChar() {
  if (E.cy == E.numRows) return;
  if (E.cx == 0 && E.cy == 0) return;

  erow *row = &E.row[E.cy];
  if (E.cx > 0) {
    editorRowDelChar(row, E.cx - 1);
    E.cx--;
  } else {
    E.cx = E.row[E.cy - 1].size;
    editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
    editorDelRow(E.cy);
    E.cy--;
  }
}

/*** COPY, PASTE & SELECTION ***/
void editorGetSelectionBounds(int *sx, int *sy, int *ex, int *ey) {
  if (!E.hasMark) {
    *sx = E.cx; *sy = E.cy;
    *ex = E.cx; *ey = E.cy;
    return;
  }
  if (E.markY < E.cy || (E.markY == E.cy && E.markX < E.cx)) {
    *sx = E.markX; *sy = E.markY;
    *ex = E.cx;    *ey = E.cy;
  } else {
    *sx = E.cx;    *sy = E.cy;
    *ex = E.markX; *ey = E.markY;
  }
}

void editorCopy() {
  if (!E.hasMark) return;
  int sx, sy, ex, ey;
  editorGetSelectionBounds(&sx, &sy, &ex, &ey);

  free(E.clipboard);
  struct abuf ab = ABUF_INIT;

  for (int y = sy; y <= ey && y < E.numRows; y++) {
    int start_x = (y == sy) ? sx : 0;
    int end_x = (y == ey) ? ex : E.row[y].size;
    if (end_x > E.row[y].size) end_x = E.row[y].size;

    if (end_x > start_x) {
      abAppend(&ab, &E.row[y].chars[start_x], end_x - start_x);
    }
    if (y < ey) {
      abAppend(&ab, "\n", 1);
    }
  }

  E.clipboard = malloc(ab.len + 1);
  memcpy(E.clipboard, ab.b, ab.len);
  E.clipboard[ab.len] = '\0';
  abFree(&ab);
}

void editorCut() {
  if (!E.hasMark) return;
  editorCopy();

  int sx, sy, ex, ey;
  editorGetSelectionBounds(&sx, &sy, &ex, &ey);

  E.cy = ey;
  E.cx = ex;
  while (E.cy > sy || (E.cy == sy && E.cx > sx)) {
    editorDelChar();
  }
  E.hasMark = 0;
}

void editorPaste() {
  if (!E.clipboard) return;
  for (int i = 0; E.clipboard[i] != '\0'; i++) {
    editorInsertChar(E.clipboard[i]);
  }
}

/*** FILE I/O ***/
char *editorRowsToString(int *buflen) {
  int totlen = 0;
  for (int j = 0; j < E.numRows; j++)
    totlen += E.row[j].size + 1;
  *buflen = totlen;

  char *buf = malloc(totlen);
  char *p = buf;
  for (int j = 0; j < E.numRows; j++) {
    memcpy(p, E.row[j].chars, E.row[j].size);
    p += E.row[j].size;
    *p = '\n';
    p++;
  }
  return buf;
}

void editorSave(char *filename) {
  if (filename == NULL) return;
  int len;
  char *buf = editorRowsToString(&len);

  int fd = open(filename, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd != -1) {
    if (ftruncate(fd, len) != -1) {
      if (write(fd, buf, len) == len) {
        close(fd);
        free(buf);
        return;
      }
    }
    close(fd);
  }
  free(buf);
}

void editorFileOpen(char *filename) {
  FILE *f = fopen(filename, "r");
  if (!f) {
    editorAppendRow("", 0);
    return;
  }

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  while ((linelen = getline(&line, &linecap, f)) != -1) {
    while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
      linelen--;
    editorAppendRow(line, linelen);
  }
  free(line);
  fclose(f);
}

/*** INPUT ***/
void editorMoveCursor(int key) {
  erow *row = (E.cy >= E.numRows) ? NULL : &E.row[E.cy];

  switch (key) {
    case ARROW_LEFT:
      if (E.cx != 0) {
        E.cx--;
      } else if (E.cy > 0) {
        E.cy--;
        E.cx = E.row[E.cy].size;
      }
      break;
    case ARROW_RIGHT:
      if (row && E.cx < row->size) {
        E.cx++;
      } else if (row && E.cx == row->size) {
        E.cy++;
        E.cx = 0;
      }
      break;
    case ARROW_UP:
      if (E.cy != 0) E.cy--;
      break;
    case ARROW_DOWN:
      if (E.cy < E.numRows) E.cy++;
      break;
  }

  row = (E.cy >= E.numRows) ? NULL : &E.row[E.cy];
  int rowlen = row ? row->size : 0;
  if (E.cx > rowlen) {
    E.cx = rowlen;
  }
}

void editorKeyPress() {
  int c = keyRead();

  switch (c) {
    case CTRL_KEY('x'):
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3);
      exit(0);
      break;

    /* Marks & Clipboard System */
    case CTRL_KEY('s'):
      E.markX = E.cx;
      E.markY = E.cy;
      E.hasMark = 1;
      break;

    case CTRL_KEY('g'):
      if (E.hasMark && E.markY < E.numRows) {
        E.cy = E.markY;
        E.cx = E.markX;
      }
      break;

    case CTRL_KEY('c'):
      editorCopy();
      break;

    case CTRL_KEY('k'):
      editorCut();
      break;

    case CTRL_KEY('v'):
      editorPaste();
      break;

    case CTRL_KEY('f'):
    case ARROW_RIGHT:
      editorMoveCursor(ARROW_RIGHT);
      break;

    case CTRL_KEY('b'):
    case ARROW_LEFT:
      editorMoveCursor(ARROW_LEFT);
      break;

    case CTRL_KEY('p'):
    case ARROW_UP:
      editorMoveCursor(ARROW_UP);
      break;

    case CTRL_KEY('n'):
    case ARROW_DOWN:
      editorMoveCursor(ARROW_DOWN);
      break;

    case CTRL_KEY('a'):
    case HOME_KEY:
      E.cx = 0;
      break;

    case CTRL_KEY('e'):
    case END_KEY:
      if (E.cy < E.numRows) E.cx = E.row[E.cy].size;
      break;

    case BACKSPACE_KEY:
    case CTRL_KEY('h'):
      editorDelChar();
      break;

    case '\r':
      editorInsertNewLine();
      break;

    case CTRL_KEY('o'):
      editorSave(E.filename);
      break;

    default:
      editorInsertChar(c);
      break;
  }
}

/*** OUTPUT ***/
void verticalScroll() {
  if (E.cy < E.rowOffset) {
    E.rowOffset = E.cy;
  }
  if (E.cy >= E.rowOffset + E.screenrows) {
    E.rowOffset = E.cy - E.screenrows + 1;
  }
}

void editorDrawRows(struct abuf *ab) {
  int margin_width = 7;

  for (int y = 0; y < E.screenrows; y++) {
    int filerow = y + E.rowOffset;

    if (filerow >= E.numRows) {
      abAppend(ab, COLOR_TILDE "~" COLOR_RESET, strlen(COLOR_TILDE "~" COLOR_RESET));
    } else {
      char numbuf[64];
      snprintf(numbuf, sizeof(numbuf), COLOR_LINENUM "%4d | " COLOR_RESET, filerow + 1);
      abAppend(ab, numbuf, strlen(numbuf));

      int len = E.row[filerow].size;
      int avail_width = E.screencols - margin_width;
      if (len > avail_width) len = avail_width;

      for (int j = 0; j < len; j++) {
        if (E.hasMark && filerow == E.markY && j == E.markX) {
          abAppend(ab, COLOR_MARK, strlen(COLOR_MARK));
          abAppend(ab, &E.row[filerow].chars[j], 1);
          abAppend(ab, COLOR_RESET, strlen(COLOR_RESET));
        } else {
          abAppend(ab, &E.row[filerow].chars[j], 1);
        }
      }
    }

    abAppend(ab, "\x1b[K", 3);
    if (y < E.screenrows - 1) {
      abAppend(ab, "\r\n", 2);
    }
  }
}

void editorRefreshScreen() {
  verticalScroll();
  struct abuf ab = ABUF_INIT;

  abAppend(&ab, "\x1b[?25l", 6);
  abAppend(&ab, "\x1b[H", 3);

  abAppend(&ab, "\x1b[2 q", 5);

  editorDrawRows(&ab);

  char buf[32];
  int margin_offset = 8;
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowOffset) + 1, E.cx + margin_offset);
  abAppend(&ab, buf, strlen(buf));

  abAppend(&ab, "\x1b[?25h", 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

/*** INIT ***/
void initEditor() {
  E.cx = E.cy = 0;
  E.row = NULL;
  E.filename = NULL;
  E.rowOffset = 0;
  E.numRows = 0;
  E.hasMark = 0;
  E.markX = 0;
  E.markY = 0;
  E.clipboard = NULL;

  if (getWindowSize(&E.screenrows, &E.screencols) == -1)
    die("getWindowSize");
}

int main(int argc, char *argv[]) {
  enterRawMode();
  initEditor();
  if (argc >= 2) {
    E.filename = strdup(argv[1]);
    editorFileOpen(argv[1]);
  } else {
    E.filename = strdup("untitled.txt");
    editorAppendRow("", 0);
  }

  while (1) {
    editorRefreshScreen();
    editorKeyPress();
  }

  return 0;
}
