#ifndef __CSS_H__
#define __CSS_H__

/* css.h -- pretty print the content of style elements

  (c) 1998-2026 (W3C) MIT, ERCIM, Keio University
  See tidy.h for the copyright notice.

*/

#include "forward.h"
#include "tidybuffio.h"

/**
 *  Lays the CSS found in `css` out again, writing the result to `out` as a
 *  sequence of lines separated by newlines. Each line carries its own
 *  indentation as leading spaces, and the last line has no trailing newline.
 *
 *  `indent` is the column the style element itself was printed at, and thus
 *  the column the outermost rules are printed at; the indentation step and
 *  the wrap margin are taken from the document's configuration.
 *
 *  Returns `no` for anything the formatter is not completely sure about, e.g.
 *  unbalanced braces or legacy comment hiding, in which case `out` is left
 *  empty and the caller is expected to print the style content unchanged.
 */
TY_PRIVATE Bool TY_(FormatCSS)( TidyDocImpl* doc, ctmbstr css, uint len,
                                uint indent, TidyBuffer* out );

#endif /* __CSS_H__ */
