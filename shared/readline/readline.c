/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2013, 2014 Damien P. George
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "py/mpstate.h"
#include "py/repl.h"
#include "py/mphal.h"
#include "shared/readline/readline.h"

#if 0 // print debugging info
#define DEBUG_PRINT (1)
#define DEBUG_printf printf
#else // don't print debugging info
#define DEBUG_printf(...) (void)0
#endif

// flags for readline_t.auto_indent_state
#define AUTO_INDENT_ENABLED (0x01)
#define AUTO_INDENT_JUST_ADDED (0x02)

enum { ESEQ_NONE, ESEQ_ESC, ESEQ_ESC_BRACKET, ESEQ_ESC_BRACKET_DIGIT, ESEQ_ESC_O };

#ifdef _MSC_VER
// work around MSVC compiler bug: https://stackoverflow.com/q/62259834/1976323
#pragma warning(disable : 4090)
#endif

void readline_init0(void) {
    memset(MP_STATE_PORT(readline_hist), 0, MICROPY_READLINE_HISTORY_SIZE * sizeof(const char*));
}

static char *str_dup_maybe(const char *str) {
    uint32_t len = strlen(str);
    char *s2 = m_new_maybe(char, len + 1);
    if (s2 == NULL) {
        return NULL;
    }
    memcpy(s2, str, len + 1);
    return s2;
}

// By default assume terminal which implements VT100 commands...
#ifndef MICROPY_HAL_HAS_VT100
#define MICROPY_HAL_HAS_VT100 (1)
#endif

// ...and provide the implementation using them
#if MICROPY_HAL_HAS_VT100
static void mp_hal_move_cursor_back(uint pos) {
    if (pos <= 4) {
        // fast path for most common case of 1 step back
        mp_hal_stdout_tx_strn("\b\b\b\b", pos);
    } else {
        char vt100_command[6];
        // snprintf needs space for the terminating null character
        int n = snprintf(&vt100_command[0], sizeof(vt100_command), "\x1b[%u", pos);
        if (n > 0) {
            assert((unsigned)n < sizeof(vt100_command));
            vt100_command[n] = 'D'; // replace null char
            mp_hal_stdout_tx_strn(vt100_command, n + 1);
        }
    }
}

static void mp_hal_erase_line_from_cursor(uint n_chars_to_erase) {
    (void)n_chars_to_erase;
    mp_hal_stdout_tx_strn("\x1b[K", 3);
}
#endif

typedef struct _readline_t {
    vstr_t *line;
    size_t orig_line_len;
    int escape_seq;
    int hist_cur;
    size_t cursor_pos;
    char escape_seq_buf[1];
    char last_nl;
    #if MICROPY_REPL_AUTO_INDENT
    uint8_t auto_indent_state;
    #endif
    const char *prompt;
    // TULIP_READLINE_UTF8: a multibyte character arrives one byte per call, and
    // is held here until it is complete so that it can be inserted and echoed in
    // one piece. See tulip/shared/patches/micropython-readline-utf8.patch.
    char utf8_buf[4];
    uint8_t utf8_len;
    uint8_t utf8_need;
} readline_t;

static readline_t rl;

/* TULIP_READLINE_UTF8
 *
 * Upstream readline is bytes all the way through: it accepts only 32..126 as
 * printable, moves the cursor one byte at a time, and passes byte counts to
 * mp_hal_move_cursor_back() as though a byte were a column. None of that holds
 * for Japanese, where a character is three bytes and two columns, so the REPL
 * simply dropped it -- which is what stopped Tulip's IME from typing at the
 * prompt (tulip/shared/py/ime.py).
 *
 * What is added: cursor_pos stays a byte index, but a step is now a whole
 * character, and a line that contains any byte over 0x7f is redrawn whole rather
 * than by byte arithmetic. A pure ASCII line takes the original path unchanged,
 * byte for byte, so the REPL everyone already uses is not touched.
 */
static bool rl_utf8_cont(byte c) {
    return (c & 0xc0) == 0x80;
}

// East Asian Wide and Fullwidth: the characters that take two columns.
static bool rl_utf8_wide(uint32_t cp) {
    return (cp >= 0x1100 && cp <= 0x115f)
           || (cp >= 0x2e80 && cp <= 0xa4cf)
           || (cp >= 0xac00 && cp <= 0xd7a3)
           || (cp >= 0xf900 && cp <= 0xfaff)
           || (cp >= 0xfe30 && cp <= 0xfe6f)
           || (cp >= 0xff00 && cp <= 0xff60)
           || (cp >= 0xffe0 && cp <= 0xffe6);
}

// Bytes in the character ending at `pos`, not going below `floor`.
static size_t rl_utf8_back(size_t pos, size_t floor) {
    size_t n = 1;
    while (pos - n > floor && rl_utf8_cont((byte)rl.line->buf[pos - n])) {
        n++;
    }
    return n;
}

// Bytes in the character starting at `pos`.
static size_t rl_utf8_forward(size_t pos) {
    size_t len = rl.line->len;
    if (pos >= len) {
        return 0;
    }
    size_t n = 1;
    while (pos + n < len && rl_utf8_cont((byte)rl.line->buf[pos + n])) {
        n++;
    }
    return n;
}

// Display columns occupied by the bytes in [from, to).
static uint rl_utf8_cols(size_t from, size_t to) {
    uint cols = 0;
    size_t i = from;
    while (i < to) {
        byte c = (byte)rl.line->buf[i];
        size_t n = 1;
        while (i + n < to && rl_utf8_cont((byte)rl.line->buf[i + n])) {
            n++;
        }
        if (n == 3) {
            uint32_t cp = ((uint32_t)(c & 0x0f) << 12)
                | ((uint32_t)((byte)rl.line->buf[i + 1] & 0x3f) << 6)
                | ((uint32_t)((byte)rl.line->buf[i + 2] & 0x3f));
            cols += rl_utf8_wide(cp) ? 2 : 1;
        } else {
            cols += 1;
        }
        i += n;
    }
    return cols;
}

static bool rl_utf8_dirty(void) {
    for (size_t i = rl.orig_line_len; i < rl.line->len; i++) {
        if ((byte)rl.line->buf[i] >= 0x80) {
            return true;
        }
    }
    return false;
}

#if MICROPY_REPL_EMACS_WORDS_MOVE
static size_t cursor_count_word(int forward) {
    const char *line_buf = vstr_str(rl.line);
    size_t pos = rl.cursor_pos;
    bool in_word = false;

    for (;;) {
        // if moving backwards and we've reached 0... break
        if (!forward && pos == 0) {
            break;
        }
        // or if moving forwards and we've reached to the end of line... break
        else if (forward && pos == vstr_len(rl.line)) {
            break;
        }

        if (unichar_isalnum(line_buf[pos + (forward - 1)])) {
            in_word = true;
        } else if (in_word) {
            break;
        }

        pos += forward ? forward : -1;
    }

    return forward ? pos - rl.cursor_pos : rl.cursor_pos - pos;
}
#endif

// Function returns true if newline character shall be processed.
static bool process_nl(int c) {
    if ((c == '\r' || c == '\n') && (rl.last_nl == 0 || rl.last_nl == c)) {
        rl.last_nl = c;
        return true;
    }

    rl.last_nl = 0;
    return false;
}

int readline_process_char(int c) {
    size_t last_line_len = rl.line->len;
    int redraw_step_back = 0;
    bool redraw_from_cursor = false;
    int redraw_step_forward = 0;
    // TULIP_READLINE_UTF8: captured before any edit, because an edit destroys the
    // bytes whose width the redraw would otherwise have to measure.
    bool utf8_was_dirty = rl_utf8_dirty();
    uint utf8_old_cols = utf8_was_dirty ? rl_utf8_cols(rl.orig_line_len, rl.cursor_pos) : 0;
    if (rl.escape_seq == ESEQ_NONE) {
        if (CHAR_CTRL_A <= c && c <= CHAR_CTRL_E && vstr_len(rl.line) == rl.orig_line_len) {
            // control character with empty line
            return c;
        } else if (c == CHAR_CTRL_A) {
            // CTRL-A with non-empty line is go-to-start-of-line
            goto home_key;
        #if MICROPY_REPL_EMACS_KEYS
        } else if (c == CHAR_CTRL_B) {
            // CTRL-B with non-empty line is go-back-one-char
            goto left_arrow_key;
        #endif
        } else if (c == CHAR_CTRL_C) {
            // CTRL-C with non-empty line is cancel
            return c;
        #if MICROPY_REPL_EMACS_KEYS
        } else if (c == CHAR_CTRL_D) {
            // CTRL-D with non-empty line is delete-at-cursor
            goto delete_key;
        #endif
        } else if (c == CHAR_CTRL_E) {
            // CTRL-E is go-to-end-of-line
            goto end_key;
        #if MICROPY_REPL_EMACS_KEYS
        } else if (c == CHAR_CTRL_F) {
            // CTRL-F with non-empty line is go-forward-one-char
            goto right_arrow_key;
        } else if (c == CHAR_CTRL_K) {
            // CTRL-K is kill from cursor to end-of-line, inclusive
            vstr_cut_tail_bytes(rl.line, last_line_len - rl.cursor_pos);
            // set redraw parameters
            redraw_from_cursor = true;
        } else if (c == CHAR_CTRL_N) {
            // CTRL-N is go to next line in history
            goto down_arrow_key;
        } else if (c == CHAR_CTRL_P) {
            // CTRL-P is go to previous line in history
            goto up_arrow_key;
        } else if (c == CHAR_CTRL_U) {
            // CTRL-U is kill from beginning-of-line up to cursor
            vstr_cut_out_bytes(rl.line, rl.orig_line_len, rl.cursor_pos - rl.orig_line_len);
            // set redraw parameters
            redraw_step_back = rl.cursor_pos - rl.orig_line_len;
            redraw_from_cursor = true;
        #endif
        #if MICROPY_REPL_EMACS_EXTRA_WORDS_MOVE
        } else if (c == CHAR_CTRL_W) {
            goto backward_kill_word;
        #endif
        } else if (process_nl(c)) {
            // newline
            mp_hal_stdout_tx_str("\r\n");
            readline_push_history(vstr_null_terminated_str(rl.line) + rl.orig_line_len);
            return 0;
        } else if (c == 27) {
            // escape sequence
            rl.escape_seq = ESEQ_ESC;
        } else if (c == 8 || c == 127) {
            // backspace/delete
            if (rl.cursor_pos > rl.orig_line_len) {
                // work out how many chars to backspace
                #if MICROPY_REPL_AUTO_INDENT
                int nspace = 0;
                for (size_t i = rl.orig_line_len; i < rl.cursor_pos; i++) {
                    if (rl.line->buf[i] != ' ') {
                        nspace = 0;
                        break;
                    }
                    nspace += 1;
                }
                if (nspace < 4) {
                    nspace = 1;
                } else {
                    nspace = 4;
                }
                #else
                int nspace = 1;
                #endif
                if (nspace == 1) {
                    // TULIP_READLINE_UTF8: a whole character, or the rest of it
                    // would be left behind as an invalid sequence.
                    nspace = rl_utf8_back(rl.cursor_pos, rl.orig_line_len);
                }

                // do the backspace
                vstr_cut_out_bytes(rl.line, rl.cursor_pos - nspace, nspace);
                // set redraw parameters
                redraw_step_back = nspace;
                redraw_from_cursor = true;
            }
        #if MICROPY_REPL_AUTO_INDENT
        } else if ((rl.auto_indent_state & AUTO_INDENT_JUST_ADDED) && (c == 9 || c == ' ')) {
            // tab/space after auto-indent: disable auto-indent
            //  - if it's a tab then leave existing indent
            //  - if it's a space then remove 3 spaces from existing indent
            rl.auto_indent_state = 0;
            if (c == ' ') {
                redraw_step_back = 3;
                vstr_cut_tail_bytes(rl.line, 3);
            }
        #endif
        #if MICROPY_HELPER_REPL
        } else if (c == 9) {
            // tab magic
            const char *compl_str;
            size_t compl_len;
            if (vstr_len(rl.line) != 0 && unichar_isspace(vstr_str(rl.line)[rl.cursor_pos - 1])) {
                // expand tab to 4 spaces if it follows whitespace:
                //  - includes the case of additional indenting
                //  - includes the case of indenting the start of a line that's not the first line,
                //    because a newline will be the previous character
                //  - doesn't include the case when at the start of the first line, because we still
                //    want to use auto-complete there
                compl_str = "    ";
                compl_len = 4;
            } else {
                // try to auto-complete a word
                const char *cur_line_buf = vstr_str(rl.line) + rl.orig_line_len;
                size_t cur_line_len = rl.cursor_pos - rl.orig_line_len;
                compl_len = mp_repl_autocomplete(cur_line_buf, cur_line_len, &mp_plat_print, &compl_str);
            }
            if (compl_len == 0) {
                // no match
            } else if (compl_len == (size_t)(-1)) {
                // many matches
                mp_hal_stdout_tx_str(rl.prompt);
                mp_hal_stdout_tx_strn(rl.line->buf + rl.orig_line_len, rl.cursor_pos - rl.orig_line_len);
                redraw_from_cursor = true;
            } else {
                // one match
                for (size_t i = 0; i < compl_len; ++i) {
                    vstr_ins_byte(rl.line, rl.cursor_pos + i, *compl_str++);
                }
                // set redraw parameters
                redraw_from_cursor = true;
                redraw_step_forward = compl_len;
            }
        #endif
        } else if (c >= 32 && c != 127) {
            // printable character. TULIP_READLINE_UTF8: 127 is excluded rather
            // than everything over 126, so the bytes of a multibyte character get
            // in. They are gathered until the sequence is complete and inserted
            // together -- one at a time would echo a half-decoded character and
            // redraw against a line that is briefly not valid UTF-8.
            if (c >= 0x80) {
                if (rl.utf8_need == 0) {
                    if ((c & 0xe0) == 0xc0) {
                        rl.utf8_need = 2;
                    } else if ((c & 0xf0) == 0xe0) {
                        rl.utf8_need = 3;
                    } else if ((c & 0xf8) == 0xf0) {
                        rl.utf8_need = 4;
                    } else {
                        // A continuation byte with no lead byte in front of it.
                        return -1;
                    }
                    rl.utf8_len = 0;
                }
                rl.utf8_buf[rl.utf8_len++] = (char)c;
                if (rl.utf8_len < rl.utf8_need) {
                    return -1;
                }
                for (uint8_t i = 0; i < rl.utf8_len; i++) {
                    vstr_ins_byte(rl.line, rl.cursor_pos + i, rl.utf8_buf[i]);
                }
                redraw_step_forward = rl.utf8_len;
                rl.utf8_len = 0;
                rl.utf8_need = 0;
            } else {
                vstr_ins_char(rl.line, rl.cursor_pos, c);
                redraw_step_forward = 1;
            }
            // set redraw parameters
            redraw_from_cursor = true;
        }
    } else if (rl.escape_seq == ESEQ_ESC) {
        switch (c) {
            case '[':
                rl.escape_seq = ESEQ_ESC_BRACKET;
                break;
            case 'O':
                rl.escape_seq = ESEQ_ESC_O;
                break;
            #if MICROPY_REPL_EMACS_WORDS_MOVE
            case 'b':
#if MICROPY_REPL_EMACS_EXTRA_WORDS_MOVE
backward_word:
#endif
                redraw_step_back = cursor_count_word(0);
                rl.escape_seq = ESEQ_NONE;
                break;
            case 'f':
#if MICROPY_REPL_EMACS_EXTRA_WORDS_MOVE
forward_word:
#endif
                redraw_step_forward = cursor_count_word(1);
                rl.escape_seq = ESEQ_NONE;
                break;
            case 'd':
                vstr_cut_out_bytes(rl.line, rl.cursor_pos, cursor_count_word(1));
                redraw_from_cursor = true;
                rl.escape_seq = ESEQ_NONE;
                break;
            case 127:
#if MICROPY_REPL_EMACS_EXTRA_WORDS_MOVE
backward_kill_word:
#endif
                redraw_step_back = cursor_count_word(0);
                vstr_cut_out_bytes(rl.line, rl.cursor_pos - redraw_step_back, redraw_step_back);
                redraw_from_cursor = true;
                rl.escape_seq = ESEQ_NONE;
                break;
            #endif
            default:
                DEBUG_printf("(ESC %d)", c);
                rl.escape_seq = ESEQ_NONE;
                break;
        }
    } else if (rl.escape_seq == ESEQ_ESC_BRACKET) {
        if ('0' <= c && c <= '9') {
            rl.escape_seq = ESEQ_ESC_BRACKET_DIGIT;
            rl.escape_seq_buf[0] = c;
        } else {
            rl.escape_seq = ESEQ_NONE;
            if (c == 'A') {
#if MICROPY_REPL_EMACS_KEYS
up_arrow_key:
#endif
                // up arrow
                if (rl.hist_cur + 1 < MICROPY_READLINE_HISTORY_SIZE && MP_STATE_PORT(readline_hist)[rl.hist_cur + 1] != NULL) {
                    // increase hist num
                    rl.hist_cur += 1;
                    // set line to history
                    rl.line->len = rl.orig_line_len;
                    vstr_add_str(rl.line, MP_STATE_PORT(readline_hist)[rl.hist_cur]);
                    // set redraw parameters
                    redraw_step_back = rl.cursor_pos - rl.orig_line_len;
                    redraw_from_cursor = true;
                    redraw_step_forward = rl.line->len - rl.orig_line_len;
                }
            } else if (c == 'B') {
#if MICROPY_REPL_EMACS_KEYS
down_arrow_key:
#endif
                // down arrow
                if (rl.hist_cur >= 0) {
                    // decrease hist num
                    rl.hist_cur -= 1;
                    // set line to history
                    vstr_cut_tail_bytes(rl.line, rl.line->len - rl.orig_line_len);
                    if (rl.hist_cur >= 0) {
                        vstr_add_str(rl.line, MP_STATE_PORT(readline_hist)[rl.hist_cur]);
                    }
                    // set redraw parameters
                    redraw_step_back = rl.cursor_pos - rl.orig_line_len;
                    redraw_from_cursor = true;
                    redraw_step_forward = rl.line->len - rl.orig_line_len;
                }
            } else if (c == 'C') {
#if MICROPY_REPL_EMACS_KEYS
right_arrow_key:
#endif
                // right arrow
                if (rl.cursor_pos < rl.line->len) {
                    // TULIP_READLINE_UTF8: a character, not a byte.
                    redraw_step_forward = rl_utf8_forward(rl.cursor_pos);
                }
            } else if (c == 'D') {
#if MICROPY_REPL_EMACS_KEYS
left_arrow_key:
#endif
                // left arrow
                if (rl.cursor_pos > rl.orig_line_len) {
                    // TULIP_READLINE_UTF8: a character, not a byte.
                    redraw_step_back = rl_utf8_back(rl.cursor_pos, rl.orig_line_len);
                }
            } else if (c == 'H') {
                // home
                goto home_key;
            } else if (c == 'F') {
                // end
                goto end_key;
            } else {
                DEBUG_printf("(ESC [ %d)", c);
            }
        }
    } else if (rl.escape_seq == ESEQ_ESC_BRACKET_DIGIT) {
        if (c == '~') {
            if (rl.escape_seq_buf[0] == '1' || rl.escape_seq_buf[0] == '7') {
home_key:
                redraw_step_back = rl.cursor_pos - rl.orig_line_len;
            } else if (rl.escape_seq_buf[0] == '4' || rl.escape_seq_buf[0] == '8') {
end_key:
                redraw_step_forward = rl.line->len - rl.cursor_pos;
            } else if (rl.escape_seq_buf[0] == '3') {
                // delete
#if MICROPY_REPL_EMACS_KEYS
delete_key:
#endif
                if (rl.cursor_pos < rl.line->len) {
                    // TULIP_READLINE_UTF8: a whole character, or the rest of it
                    // would be left behind as an invalid sequence.
                    vstr_cut_out_bytes(rl.line, rl.cursor_pos, rl_utf8_forward(rl.cursor_pos));
                    redraw_from_cursor = true;
                }
            } else {
                DEBUG_printf("(ESC [ %c %d)", rl.escape_seq_buf[0], c);
            }
        #if MICROPY_REPL_EMACS_EXTRA_WORDS_MOVE
        } else if (c == ';' && rl.escape_seq_buf[0] == '1') {
            // ';' is used to separate parameters. so first parameter was '1',
            // that's used for sequences like ctrl+left, which we will try to parse.
            // escape_seq state is reset back to ESEQ_ESC_BRACKET, as if we've just received
            // the opening bracket, because more parameters are to come.
            // we don't track the parameters themselves to keep low on logic and code size. that
            // might be required in the future if more complex sequences are added.
            rl.escape_seq = ESEQ_ESC_BRACKET;
            // goto away from the state-machine, as rl.escape_seq will be overridden.
            goto redraw;
        } else if (rl.escape_seq_buf[0] == '5' && c == 'C') {
            // ctrl+right
            goto forward_word;
        } else if (rl.escape_seq_buf[0] == '5' && c == 'D') {
            // ctrl+left
            goto backward_word;
        #endif
        } else {
            DEBUG_printf("(ESC [ %c %d)", rl.escape_seq_buf[0], c);
        }
        rl.escape_seq = ESEQ_NONE;
    } else if (rl.escape_seq == ESEQ_ESC_O) {
        switch (c) {
            case 'H':
                goto home_key;
            case 'F':
                goto end_key;
            default:
                DEBUG_printf("(ESC O %d)", c);
                rl.escape_seq = ESEQ_NONE;
        }
    } else {
        rl.escape_seq = ESEQ_NONE;
    }

#if MICROPY_REPL_EMACS_EXTRA_WORDS_MOVE
redraw:
#endif

    // TULIP_READLINE_UTF8: the byte arithmetic below treats a byte as a column,
    // which a multibyte character is not -- and after an edit the bytes it used to
    // occupy are gone, so their width is no longer measurable. So a line holding
    // any non-ASCII byte is redrawn whole, walking back to the start of the
    // editable region by the column count captured before the edit. A pure ASCII
    // line takes the original path unchanged.
    if (utf8_was_dirty || rl_utf8_dirty()) {
        size_t new_pos = rl.cursor_pos - redraw_step_back + redraw_step_forward;
        uint back = utf8_was_dirty ? utf8_old_cols : (uint)(rl.cursor_pos - rl.orig_line_len);
        if (back > 0) {
            mp_hal_move_cursor_back(back);
        }
        mp_hal_erase_line_from_cursor(0);
        mp_hal_stdout_tx_strn(rl.line->buf + rl.orig_line_len,
                              rl.line->len - rl.orig_line_len);
        rl.cursor_pos = new_pos;
        uint tail = rl_utf8_cols(new_pos, rl.line->len);
        if (tail > 0) {
            mp_hal_move_cursor_back(tail);
        }
    } else {
        // redraw command prompt, efficiently
        if (redraw_step_back > 0) {
            mp_hal_move_cursor_back(redraw_step_back);
            rl.cursor_pos -= redraw_step_back;
        }
        if (redraw_from_cursor) {
            if (rl.line->len < last_line_len) {
                // erase old chars
                mp_hal_erase_line_from_cursor(last_line_len - rl.cursor_pos);
            }
            // draw new chars
            mp_hal_stdout_tx_strn(rl.line->buf + rl.cursor_pos, rl.line->len - rl.cursor_pos);
            // move cursor forward if needed (already moved forward by length of line, so move it back)
            mp_hal_move_cursor_back(rl.line->len - (rl.cursor_pos + redraw_step_forward));
            rl.cursor_pos += redraw_step_forward;
        } else if (redraw_step_forward > 0) {
            // draw over old chars to move cursor forwards
            mp_hal_stdout_tx_strn(rl.line->buf + rl.cursor_pos, redraw_step_forward);
            rl.cursor_pos += redraw_step_forward;
        }
    }

    #if MICROPY_REPL_AUTO_INDENT
    rl.auto_indent_state &= ~AUTO_INDENT_JUST_ADDED;
    #endif

    return -1;
}

#if MICROPY_REPL_AUTO_INDENT
static void readline_auto_indent(void) {
    if (!(rl.auto_indent_state & AUTO_INDENT_ENABLED)) {
        return;
    }
    vstr_t *line = rl.line;
    if (line->len > 1 && line->buf[line->len - 1] == '\n') {
        int i;
        for (i = line->len - 1; i > 0; i--) {
            if (line->buf[i - 1] == '\n') {
                break;
            }
        }
        size_t j;
        for (j = i; j < line->len; j++) {
            if (line->buf[j] != ' ') {
                break;
            }
        }
        // i=start of line; j=first non-space
        if (i > 0 && j + 1 == line->len) {
            // previous line is not first line and is all spaces
            for (size_t k = i - 1; k > 0; --k) {
                if (line->buf[k - 1] == '\n') {
                    // don't auto-indent if last 2 lines are all spaces
                    return;
                } else if (line->buf[k - 1] != ' ') {
                    // 2nd previous line is not all spaces
                    break;
                }
            }
        }
        int n = (j - i) / 4;
        if (line->buf[line->len - 2] == ':') {
            n += 1;
        }
        while (n-- > 0) {
            vstr_add_strn(line, "    ", 4);
            mp_hal_stdout_tx_strn("    ", 4);
            rl.cursor_pos += 4;
            rl.auto_indent_state |= AUTO_INDENT_JUST_ADDED;
        }
    }
}
#endif

void readline_note_newline(const char *prompt) {
    rl.orig_line_len = rl.line->len;
    rl.cursor_pos = rl.orig_line_len;
    rl.prompt = prompt;
    mp_hal_stdout_tx_str(prompt);
    #if MICROPY_REPL_AUTO_INDENT
    readline_auto_indent();
    #endif
}

void readline_init(vstr_t *line, const char *prompt) {
    rl.line = line;
    rl.orig_line_len = line->len;
    rl.escape_seq = ESEQ_NONE;
    rl.escape_seq_buf[0] = 0;
    // TULIP_READLINE_UTF8: a sequence cut short by Ctrl-C or Enter must not leak
    // its first bytes into the next line.
    rl.utf8_len = 0;
    rl.utf8_need = 0;
    rl.hist_cur = -1;
    rl.cursor_pos = rl.orig_line_len;
    rl.prompt = prompt;
    mp_hal_stdout_tx_str(prompt);
    #if MICROPY_REPL_AUTO_INDENT
    if (vstr_len(line) == 0) {
        // start with auto-indent enabled
        rl.auto_indent_state = AUTO_INDENT_ENABLED;
    }
    readline_auto_indent();
    #endif
}

int readline(vstr_t *line, const char *prompt) {
    readline_init(line, prompt);
    for (;;) {
        int c = mp_hal_stdin_rx_chr();
        int r = readline_process_char(c);
        if (r >= 0) {
            return r;
        }
    }
}

void readline_push_history(const char *line) {
    if (line[0] != '\0'
        && (MP_STATE_PORT(readline_hist)[0] == NULL
            || strcmp(MP_STATE_PORT(readline_hist)[0], line) != 0)) {
        // a line which is not empty and different from the last one
        // so update the history
        char *most_recent_hist = str_dup_maybe(line);
        if (most_recent_hist != NULL) {
            for (int i = MICROPY_READLINE_HISTORY_SIZE - 1; i > 0; i--) {
                MP_STATE_PORT(readline_hist)[i] = MP_STATE_PORT(readline_hist)[i - 1];
            }
            MP_STATE_PORT(readline_hist)[0] = most_recent_hist;
        }
    }
}

MP_REGISTER_ROOT_POINTER(const char *readline_hist[MICROPY_READLINE_HISTORY_SIZE]);
