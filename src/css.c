/* css.c -- pretty print the content of style elements

  (c) 1998-2026 (W3C) MIT, ERCIM, Keio University
  See tidy.h for the copyright notice.

  The formatter below lays out a style sheet one statement at a time. Each
  statement is first read into a small list of tokens, and only then written
  out, because how a statement is laid out depends on how it ends: a `{` makes
  it a selector or an at-rule prelude, while a `;` makes it a declaration.

  Everything the formatter is not completely sure about makes it give up, in
  which case the caller prints the style content unchanged. Whitespace is the
  only thing rewritten, and never inside a string or an url, so the meaning of
  the style sheet is never altered. Comments keep their text, but they are
  re-indented, and one inside a value is brought onto a single line.

*/

#include "css.h"
#include "tidy-int.h"


/****************************************************************************//*
 ** MARK: - Tokens
 ***************************************************************************/


/**
 *  The kinds of token the formatter distinguishes. Anything that is neither
 *  punctuation nor a construct with its own nesting rules is a run of plain
 *  text, which the formatter only ever moves about as a whole.
 */
typedef enum
{
    CssTokText,     /**< identifier, number, hash, at-keyword, delimiter, ... */
    CssTokString,   /**< quoted string, or a complete url(...) */
    CssTokGroup,    /**< balanced ( ... ) or [ ... ] */
    CssTokComment,  /**< comment */
    CssTokColon,    /**< : */
    CssTokComma,    /**< , */
    CssTokCombine   /**< selector combinator: > + ~ */
} CssTokenKind;


/**
 *  A single token of the statement currently being laid out. The text lives in
 *  the formatter's pool rather than in the source, because groups have their
 *  whitespace collapsed as they are read.
 */
typedef struct
{
    uint kind;     /**< one of the CssTokenKind values */
    uint start;    /**< offset of the token's text within the pool */
    uint len;      /**< length of the token's text, in bytes */
    Bool space;    /**< the source had whitespace in front of this token */
    Bool sep;      /**< a space is wanted in front of this token */
} CssToken;


/**
 *  The number of tokens the statement token list starts out with. Statements
 *  longer than this are rare enough, and the list grows as needed anyway.
 */
#define CSS_TOKENS 32


/****************************************************************************//*
 ** MARK: - Formatter State
 ***************************************************************************/


/**
 *  Everything the formatter needs while it works its way through a style
 *  sheet. The output is built up one line at a time: `line` holds the line
 *  being written, and is flushed to `out` when the next line is started.
 */
typedef struct
{
    TidyDocImpl* doc;         /**< the document being printed */

    ctmbstr      css;         /**< the style content, as found in the source */
    uint         len;         /**< length of the style content, in bytes */
    uint         pos;         /**< how far the formatter has read */

    TidyBuffer   pool;        /**< the text of the current statement's tokens */
    CssToken*    toks;        /**< the current statement's tokens */
    uint         ntoks;       /**< number of tokens in the current statement */
    uint         maxtoks;     /**< number of tokens there is room for */

    TidyBuffer*  out;         /**< the lines written so far */
    TidyBuffer   line;        /**< the line being written, without its indent */
    Bool         open;        /**< a line is waiting to be flushed */
    uint         indent;      /**< the indent of the line being written */
    uint         linelen;     /**< width of the line being written, indent included */

    uint         base;        /**< the column the outermost rules go at */
    uint         spaces;      /**< how much one level of nesting indents by */
    uint         wrap;        /**< the wrap margin, or 0 for no wrapping */
    uint         depth;       /**< how deeply nested in braces we are */
    Bool         bail;        /**< the style sheet cannot be laid out safely */
} CssFmt;


/**
 *  How many levels of nesting are worth indenting. A style sheet nested any
 *  deeper than this has to make do with the indentation it has got, rather
 *  than have Tidy run off to the right for ever.
 */
#define CSS_LEVELS 64


/**
 *  The column the current level of nesting is printed at.
 */
static uint CurIndent( CssFmt* fmt )
{
    uint levels = fmt->depth < CSS_LEVELS ? fmt->depth : CSS_LEVELS;

    return fmt->base + ( levels * fmt->spaces );
}


/****************************************************************************//*
 ** MARK: - Character Classes
 ***************************************************************************/


/**
 *  Indicates whether or not the given character is CSS whitespace.
 */
static Bool IsCssWS( uint c )
{
    return ( c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' );
}


/**
 *  Indicates whether or not the given character ends a run of plain text.
 *  Note that `/` is included so that comments can be spotted, and that the
 *  combinators are included so that they can be spaced out in selectors;
 *  a run of text is never broken anywhere else.
 */
static Bool IsCssPunct( uint c )
{
    switch ( c )
    {
    case '{': case '}': case ';': case ':': case ',':
    case '(': case ')': case '[': case ']':
    case '"': case '\'': case '/': case '!':
    case '>': case '+': case '~':
        return yes;
    }
    return no;
}


/**
 *  The width the given text takes up on a line. Continuation bytes of a
 *  multibyte character don't count towards it.
 */
static uint DisplayLen( ctmbstr text, uint len )
{
    uint ix, width = 0;
    for ( ix = 0; ix < len; ++ix )
        if ( ( (byte) text[ix] & 0xC0 ) != 0x80 )
            ++width;
    return width;
}


/**
 *  Indicates whether or not the given text is the `url` function name.
 */
static Bool IsUrlName( ctmbstr text, uint len )
{
    return ( len == 3 &&
             ( text[0] == 'u' || text[0] == 'U' ) &&
             ( text[1] == 'r' || text[1] == 'R' ) &&
             ( text[2] == 'l' || text[2] == 'L' ) );
}


/**
 *  Indicates whether or not the style content holds markup that Tidy has no
 *  business rearranging: the legacy comment hiding older documents wrap their
 *  style sheets in, and CDATA section markers.
 */
static Bool HasMarkup( ctmbstr css, uint len )
{
    uint ix;
    for ( ix = 0; ix + 1 < len; ++ix )
    {
        if ( css[ix] == '<' && css[ix+1] == '!' )
            return yes;

        if ( ix + 2 < len && css[ix+2] == '>' &&
             ( ( css[ix] == '-' && css[ix+1] == '-' ) ||
               ( css[ix] == ']' && css[ix+1] == ']' ) ) )
            return yes;
    }
    return no;
}


/****************************************************************************//*
 ** MARK: - Scanning
 ***************************************************************************/


/**
 *  Reads past any whitespace, reporting how many newlines it held. Returns
 *  the number of characters skipped.
 */
static uint SkipCssWS( CssFmt* fmt, uint* newlines )
{
    uint start = fmt->pos;

    *newlines = 0;
    while ( fmt->pos < fmt->len && IsCssWS( (byte) fmt->css[fmt->pos] ) )
    {
        if ( fmt->css[fmt->pos] == '\n' )
            ++(*newlines);
        ++fmt->pos;
    }
    return fmt->pos - start;
}


/**
 *  Indicates whether or not the source between `start` and `end` is spread
 *  over more than one line.
 */
static Bool HasNewline( CssFmt* fmt, uint start, uint end )
{
    uint ix;
    for ( ix = start; ix < end; ++ix )
        if ( fmt->css[ix] == '\n' )
            return yes;
    return no;
}


/**
 *  Finds the end of the quoted string starting at `start`, which is reported
 *  as the position just past the closing quote. Returns `no` for a string that
 *  is never closed, and for one carried over a newline by a backslash: a
 *  string has to stay on one line for the formatter to move it about, and its
 *  contents are not the formatter's to rewrite.
 */
static Bool ScanString( CssFmt* fmt, uint start, uint* end )
{
    uint quote = (byte) fmt->css[start];
    uint ix = start + 1;

    while ( ix < fmt->len )
    {
        uint c = (byte) fmt->css[ix];

        if ( c == '\\' && ix + 1 < fmt->len )
        {
            if ( fmt->css[ix+1] == '\n' || fmt->css[ix+1] == '\r' )
                return no;
            ix += 2;
            continue;
        }

        if ( c == '\n' )
            return no;

        ++ix;
        if ( c == quote )
        {
            *end = ix;
            return yes;
        }
    }
    return no;
}


/**
 *  Finds the end of the comment starting at `start`, which is reported as the
 *  position just past the closing `*` and `/`. Returns `no` for a comment
 *  that is never closed.
 */
static Bool ScanComment( CssFmt* fmt, uint start, uint* end )
{
    uint ix = start + 2;

    while ( ix + 1 < fmt->len )
    {
        if ( fmt->css[ix] == '*' && fmt->css[ix+1] == '/' )
        {
            *end = ix + 2;
            return yes;
        }
        ++ix;
    }
    return no;
}


/**
 *  Finds the end of the url starting at the `(` at `start`. Urls are scanned
 *  on their own because an unquoted one may hold characters that mean
 *  something everywhere else, as data urls generally do.
 */
static Bool ScanUrl( CssFmt* fmt, uint start, uint* end )
{
    uint ix = start + 1;

    while ( ix < fmt->len && IsCssWS( (byte) fmt->css[ix] ) )
        ++ix;

    if ( ix < fmt->len && ( fmt->css[ix] == '"' || fmt->css[ix] == '\'' ) )
    {
        if ( !ScanString( fmt, ix, &ix ) )
            return no;
    }
    else
    {
        while ( ix < fmt->len && fmt->css[ix] != ')' && fmt->css[ix] != '\n' )
            ++ix;
    }

    while ( ix < fmt->len && IsCssWS( (byte) fmt->css[ix] ) )
        ++ix;

    if ( ix < fmt->len && fmt->css[ix] == ')' )
    {
        *end = ix + 1;
        return yes;
    }
    return no;
}


/**
 *  Finds the end of the balanced group starting at `start`, which is either a
 *  `(` or a `[`. Returns `no` if the group is never closed, or if a brace
 *  turns up inside it, which would mean the group isn't one after all.
 */
static Bool ScanGroup( CssFmt* fmt, uint start, uint* end )
{
    uint ix = start;
    uint depth = 0;

    while ( ix < fmt->len )
    {
        uint c = (byte) fmt->css[ix];

        if ( c == '(' || c == '[' )
        {
            ++depth;
            ++ix;
            continue;
        }

        if ( c == ')' || c == ']' )
        {
            ++ix;
            if ( --depth == 0 )
            {
                *end = ix;
                return yes;
            }
            continue;
        }

        if ( c == '"' || c == '\'' )
        {
            if ( !ScanString( fmt, ix, &ix ) )
                return no;
            continue;
        }

        if ( c == '/' && ix + 1 < fmt->len && fmt->css[ix+1] == '*' )
        {
            if ( !ScanComment( fmt, ix, &ix ) )
                return no;
            continue;
        }

        if ( c == '{' || c == '}' )
            return no;

        ++ix;
    }
    return no;
}


/****************************************************************************//*
 ** MARK: - Reading Statements
 ***************************************************************************/


/**
 *  Adds text to the pool the current statement's tokens draw on.
 */
static void PoolAdd( CssFmt* fmt, ctmbstr text, uint len )
{
    if ( len > 0 )
        tidyBufAppend( &fmt->pool, (char*) text, len );
}


/**
 *  Adds the source between `start` and `end` to the pool, with every run of
 *  whitespace collapsed to a single space. Since CSS treats any run of
 *  whitespace as one space, this is only a change of appearance.
 */
static void PoolAddCollapsed( CssFmt* fmt, uint start, uint end )
{
    uint ix = start;

    while ( ix < end )
    {
        if ( IsCssWS( (byte) fmt->css[ix] ) )
        {
            uint next = ix;
            while ( next < end && IsCssWS( (byte) fmt->css[next] ) )
                ++next;

            /* whitespace against either end of the group just goes away */
            if ( ix > start && next < end )
                PoolAdd( fmt, " ", 1 );
            ix = next;
            continue;
        }
        PoolAdd( fmt, fmt->css + ix, 1 );
        ++ix;
    }
}


/**
 *  Adds the source between `start` and `end` to the pool as one token, which
 *  is how both a balanced group and the value of a custom property are taken
 *  in. Whitespace, and the space after a comma, is all that is rewritten:
 *  strings and urls keep their contents to the character, and comments only
 *  get brought onto one line.
 *
 *  Nothing else in here is spaced out, because whether a colon belongs to a
 *  media feature or to a pseudo class cannot be told apart at this point, and
 *  getting it wrong would change which elements a selector matches.
 */
static void PoolAddSpan( CssFmt* fmt, uint start, uint end )
{
    uint ix = start;
    Bool space = no;   /* a space is owed to whatever comes next */

    while ( ix < end )
    {
        uint c = (byte) fmt->css[ix];
        uint stop, from;

        if ( IsCssWS( c ) )
        {
            while ( ix < end && IsCssWS( (byte) fmt->css[ix] ) )
                ++ix;
            space = yes;
            continue;
        }

        if ( c == ',' )
        {
            PoolAdd( fmt, ",", 1 );
            ++ix;
            space = yes;
            continue;
        }

        /* no space is wanted against a bracket, whichever kind it is */
        if ( space )
        {
            byte last = fmt->pool.size > 0 ? fmt->pool.bp[ fmt->pool.size - 1 ] : 0;

            if ( c != ')' && c != ']' && c != '}' &&
                 last != '(' && last != '[' && last != '{' )
                PoolAdd( fmt, " ", 1 );
            space = no;
        }

        if ( c == '"' || c == '\'' )
        {
            if ( !ScanString( fmt, ix, &stop ) || stop > end )
            {
                fmt->bail = yes;
                return;
            }
            PoolAdd( fmt, fmt->css + ix, stop - ix );
            ix = stop;
            continue;
        }

        if ( c == '/' && ix + 1 < end && fmt->css[ix+1] == '*' )
        {
            if ( !ScanComment( fmt, ix, &stop ) || stop > end )
            {
                fmt->bail = yes;
                return;
            }
            PoolAddCollapsed( fmt, ix, stop );
            ix = stop;
            continue;
        }

        if ( IsCssPunct( c ) )
        {
            PoolAdd( fmt, fmt->css + ix, 1 );
            ++ix;
            continue;
        }

        /* a run of plain text, which may turn out to name an url */
        from = ix;
        while ( ix < end && !IsCssWS( (byte) fmt->css[ix] ) &&
                !IsCssPunct( (byte) fmt->css[ix] ) )
            ++ix;

        if ( ix < end && fmt->css[ix] == '(' && IsUrlName( fmt->css + from, ix - from ) )
        {
            if ( !ScanUrl( fmt, ix, &stop ) || stop > end )
            {
                fmt->bail = yes;
                return;
            }
            ix = stop;
        }
        PoolAdd( fmt, fmt->css + from, ix - from );
    }
}


/**
 *  Adds a token to the current statement, and returns it so that its text can
 *  be filled in.
 */
static CssToken* PushToken( CssFmt* fmt, uint kind, Bool space )
{
    CssToken* tok;

    if ( fmt->ntoks == fmt->maxtoks )
    {
        fmt->maxtoks = fmt->maxtoks ? fmt->maxtoks * 2 : CSS_TOKENS;
        fmt->toks = (CssToken*) TidyDocRealloc( fmt->doc, fmt->toks,
                                                fmt->maxtoks * sizeof(CssToken) );
    }

    tok = &fmt->toks[ fmt->ntoks++ ];
    tok->kind = kind;
    tok->start = fmt->pool.size;
    tok->len = 0;
    tok->space = space;
    tok->sep = no;
    return tok;
}


/**
 *  Records how much text the token just pushed ended up with.
 */
static void EndToken( CssFmt* fmt, CssToken* tok )
{
    tok->len = fmt->pool.size - tok->start;
}


/**
 *  Indicates whether or not the statement read so far declares a custom
 *  property, whose value is an arbitrary run of tokens rather than something
 *  the formatter can make sense of.
 */
static Bool IsCustomProperty( CssFmt* fmt )
{
    CssToken* prop;

    if ( fmt->ntoks < 2 )
        return no;

    prop = &fmt->toks[0];
    return ( prop->kind == CssTokText && prop->len > 2 &&
             fmt->pool.bp[ prop->start ] == '-' &&
             fmt->pool.bp[ prop->start + 1 ] == '-' &&
             fmt->toks[1].kind == CssTokColon );
}


/**
 *  Reads the value of a custom property as a single token, since a custom
 *  property may be given any run of tokens at all, braces included, and none
 *  of it means to the formatter what the same run would mean elsewhere.
 *
 *  Returns the character that ended the declaration, in the same way that
 *  ReadStatement() does.
 */
static uint ReadCustomValue( CssFmt* fmt, Bool space )
{
    uint start = fmt->pos;
    uint depth = 0;
    uint stop;

    while ( fmt->pos < fmt->len )
    {
        uint c = (byte) fmt->css[ fmt->pos ];

        if ( c == '"' || c == '\'' )
        {
            if ( !ScanString( fmt, fmt->pos, &fmt->pos ) )
            {
                fmt->bail = yes;
                return 0;
            }
            continue;
        }

        if ( c == '/' && fmt->pos + 1 < fmt->len && fmt->css[fmt->pos+1] == '*' )
        {
            if ( !ScanComment( fmt, fmt->pos, &fmt->pos ) )
            {
                fmt->bail = yes;
                return 0;
            }
            continue;
        }

        if ( c == '(' || c == '[' || c == '{' )
        {
            ++depth;
            ++fmt->pos;
            continue;
        }

        if ( c == ')' || c == ']' || c == '}' )
        {
            if ( depth == 0 )
                break;
            --depth;
            ++fmt->pos;
            continue;
        }

        if ( c == ';' && depth == 0 )
            break;

        ++fmt->pos;
    }

    stop = fmt->pos;
    while ( stop > start && IsCssWS( (byte) fmt->css[stop - 1] ) )
        --stop;

    if ( stop > start )
    {
        CssToken* tok = PushToken( fmt, CssTokString, space );
        PoolAddSpan( fmt, start, stop );
        EndToken( fmt, tok );
    }

    if ( fmt->pos >= fmt->len )
        return 0;

    if ( fmt->css[ fmt->pos ] == ';' )
    {
        ++fmt->pos;
        return ';';
    }

    if ( fmt->css[ fmt->pos ] == '}' )
        return '}';

    fmt->bail = yes;
    return 0;
}


/**
 *  Reads the tokens of the next statement, which is expected to start at the
 *  current position. Returns the character that ended it: `{` and `;` are
 *  read past, a `}` is left where it is for the caller to deal with, and 0
 *  means the end of the style sheet was reached.
 */
static uint ReadStatement( CssFmt* fmt )
{
    Bool space = no;

    fmt->ntoks = 0;
    tidyBufClear( &fmt->pool );

    while ( !fmt->bail )
    {
        CssToken* tok;
        uint c, stop, newlines;

        if ( SkipCssWS( fmt, &newlines ) > 0 )
            space = yes;

        if ( fmt->pos >= fmt->len )
            return 0;

        c = (byte) fmt->css[ fmt->pos ];

        if ( c == '}' )
            return '}';

        if ( c == '{' || c == ';' )
        {
            ++fmt->pos;
            return c;
        }

        if ( c == ')' || c == ']' )
        {
            fmt->bail = yes;
            break;
        }

        /* comments are kept, wherever they turn up */
        if ( c == '/' && fmt->pos + 1 < fmt->len && fmt->css[fmt->pos+1] == '*' )
        {
            if ( !ScanComment( fmt, fmt->pos, &stop ) )
            {
                fmt->bail = yes;
                break;
            }

            /* a comment spread over several lines cannot share a line with
               the rest of the statement, so leave the style sheet alone */
            if ( HasNewline( fmt, fmt->pos, stop ) )
            {
                fmt->bail = yes;
                break;
            }

            tok = PushToken( fmt, CssTokComment, space );
            PoolAdd( fmt, fmt->css + fmt->pos, stop - fmt->pos );
            EndToken( fmt, tok );
            fmt->pos = stop;
            space = no;
            continue;
        }

        if ( c == '"' || c == '\'' )
        {
            if ( !ScanString( fmt, fmt->pos, &stop ) )
            {
                fmt->bail = yes;
                break;
            }
            tok = PushToken( fmt, CssTokString, space );
            PoolAdd( fmt, fmt->css + fmt->pos, stop - fmt->pos );
            EndToken( fmt, tok );
            fmt->pos = stop;
            space = no;
            continue;
        }

        if ( c == '(' || c == '[' )
        {
            if ( !ScanGroup( fmt, fmt->pos, &stop ) )
            {
                fmt->bail = yes;
                break;
            }
            tok = PushToken( fmt, CssTokGroup, space );
            PoolAddSpan( fmt, fmt->pos, stop );
            EndToken( fmt, tok );
            fmt->pos = stop;
            space = no;
            continue;
        }

        if ( c == ':' || c == ',' )
        {
            tok = PushToken( fmt, c == ':' ? CssTokColon : CssTokComma, space );
            PoolAdd( fmt, fmt->css + fmt->pos, 1 );
            EndToken( fmt, tok );
            ++fmt->pos;
            space = no;

            if ( c == ':' && fmt->ntoks == 2 && IsCustomProperty( fmt ) )
            {
                uint skipped = SkipCssWS( fmt, &newlines );
                return ReadCustomValue( fmt, skipped > 0 );
            }
            continue;
        }

        if ( c == '>' || c == '+' || c == '~' )
        {
            tok = PushToken( fmt, CssTokCombine, space );
            PoolAdd( fmt, fmt->css + fmt->pos, 1 );
            EndToken( fmt, tok );
            ++fmt->pos;
            space = no;
            continue;
        }

        /* a run of plain text. The first character is taken whatever it is, so
           that the punctuation that belongs with the text after it, such as
           the `!` of `!important` and the `/` of a shorthand value, comes
           along with it rather than standing on its own */
        {
            uint start = fmt->pos;

            for ( ++fmt->pos; fmt->pos < fmt->len; ++fmt->pos )
                if ( IsCssWS( (byte) fmt->css[fmt->pos] ) ||
                     IsCssPunct( (byte) fmt->css[fmt->pos] ) )
                    break;

            tok = PushToken( fmt, CssTokText, space );
            PoolAdd( fmt, fmt->css + start, fmt->pos - start );
            EndToken( fmt, tok );

            /* an url is part of the name in front of it, so that whatever it
               holds is left exactly as it was written */
            if ( fmt->pos < fmt->len && fmt->css[fmt->pos] == '(' &&
                 IsUrlName( (ctmbstr) fmt->pool.bp + tok->start, tok->len ) )
            {
                if ( !ScanUrl( fmt, fmt->pos, &stop ) ||
                     HasNewline( fmt, fmt->pos, stop ) )
                {
                    fmt->bail = yes;
                    break;
                }
                PoolAdd( fmt, fmt->css + fmt->pos, stop - fmt->pos );
                EndToken( fmt, tok );
                tok->kind = CssTokString;
                fmt->pos = stop;
            }

            space = no;
        }
    }
    return 0;
}


/****************************************************************************//*
 ** MARK: - Writing Lines
 ***************************************************************************/


/**
 *  Writes the line that has been built up to the output, and forgets about
 *  it. Lines are separated by newlines rather than ended by one, so that the
 *  formatter's output never ends with one.
 */
static void FlushLine( CssFmt* fmt )
{
    uint ix;

    if ( !fmt->open )
        return;

    if ( fmt->out->size > 0 )
        tidyBufAppend( fmt->out, "\n", 1 );

    for ( ix = 0; ix < fmt->indent; ++ix )
        tidyBufAppend( fmt->out, " ", 1 );

    if ( fmt->line.size > 0 )
        tidyBufAppend( fmt->out, fmt->line.bp, fmt->line.size );

    tidyBufClear( &fmt->line );
    fmt->open = no;
}


/**
 *  Starts a new line at the given indent, writing out the one before it.
 */
static void StartLine( CssFmt* fmt, uint indent )
{
    FlushLine( fmt );
    fmt->open = yes;
    fmt->indent = indent;
    fmt->linelen = indent;
}


/**
 *  Adds text to the line being built up.
 */
static void AddText( CssFmt* fmt, ctmbstr text, uint len )
{
    if ( len == 0 )
        return;

    tidyBufAppend( &fmt->line, (char*) text, len );
    fmt->linelen += DisplayLen( text, len );
}


/**
 *  Adds the given token's text to the line being built up.
 */
static void AddToken( CssFmt* fmt, CssToken* tok )
{
    AddText( fmt, (ctmbstr) fmt->pool.bp + tok->start, tok->len );
}


/**
 *  Leaves a blank line behind, which is how a blank line in the source is
 *  passed on. A style sheet never starts with one.
 */
static void AddBlankLine( CssFmt* fmt )
{
    FlushLine( fmt );
    if ( fmt->out->size > 0 )
        tidyBufAppend( fmt->out, "\n", 1 );
}


/****************************************************************************//*
 ** MARK: - Writing Statements
 ***************************************************************************/


/**
 *  Works out where a space is wanted in front of each token of a selector or
 *  an at-rule prelude.
 *
 *  Whitespace is left as the source had it apart from around a comma and a
 *  combinator, where a selector cannot tell the difference. It matters
 *  everywhere else: `a :hover` and `a:hover` are both valid and they match
 *  different elements, so neither may be turned into the other.
 */
static void SpacePrelude( CssFmt* fmt )
{
    uint ix;

    for ( ix = 0; ix < fmt->ntoks; ++ix )
    {
        CssToken* tok = &fmt->toks[ix];
        CssToken* prev = &fmt->toks[ ix > 0 ? ix - 1 : 0 ];

        if ( ix == 0 || tok->kind == CssTokComma )
            tok->sep = no;
        else if ( prev->kind == CssTokComma )
            tok->sep = yes;
        else if ( tok->kind == CssTokCombine || prev->kind == CssTokCombine )
            tok->sep = yes;
        else
            tok->sep = tok->space;
    }
}


/**
 *  Works out where a space is wanted in front of each token of a declaration.
 *  `colon` is the token index of the colon that separates the property from
 *  its value, or the token count when the statement has no colon of its own,
 *  as `@charset "utf-8"` doesn't.
 */
static void SpaceDeclaration( CssFmt* fmt, uint colon )
{
    uint ix;

    for ( ix = 0; ix < fmt->ntoks; ++ix )
    {
        CssToken* tok = &fmt->toks[ix];

        if ( ix == 0 || ix == colon )
            tok->sep = no;
        else if ( ix == colon + 1 )
            tok->sep = yes;
        else if ( ix > colon && fmt->pool.bp[ tok->start ] == '!' )
            tok->sep = yes;   /* as in `red !important` */
        else if ( tok->kind == CssTokComma )
            tok->sep = no;
        else if ( fmt->toks[ix - 1].kind == CssTokComma )
            tok->sep = yes;
        else
            tok->sep = tok->space;
    }
}


/**
 *  The index of the colon that makes the statement a declaration, or the
 *  token count when there is none.
 */
static uint FindColon( CssFmt* fmt )
{
    uint ix;

    for ( ix = 1; ix < fmt->ntoks; ++ix )
        if ( fmt->toks[ix].kind == CssTokColon )
            return ix;

    return fmt->ntoks;
}


/**
 *  The width the given tokens take up when written on one line.
 */
static uint TokensWidth( CssFmt* fmt, uint from, uint to )
{
    uint ix, width = 0;

    for ( ix = from; ix < to; ++ix )
    {
        if ( ix > from && fmt->toks[ix].sep )
            ++width;
        width += DisplayLen( (ctmbstr) fmt->pool.bp + fmt->toks[ix].start,
                             fmt->toks[ix].len );
    }
    return width;
}


/**
 *  Writes the given tokens, starting a new line at `cont` whenever the wrap
 *  margin is reached. Only the places the source itself had whitespace at are
 *  broken, since a newline elsewhere would add whitespace where CSS gives it
 *  a meaning of its own.
 */
static void AddTokens( CssFmt* fmt, uint from, uint to, uint cont )
{
    uint ix;

    for ( ix = from; ix < to; ++ix )
    {
        CssToken* tok = &fmt->toks[ix];

        if ( ix > from && tok->sep )
        {
            uint width = DisplayLen( (ctmbstr) fmt->pool.bp + tok->start, tok->len );

            if ( fmt->wrap && fmt->linelen + 1 + width >= fmt->wrap &&
                 fmt->linelen > fmt->indent )
                StartLine( fmt, cont );
            else
                AddText( fmt, " ", 1 );
        }
        AddToken( fmt, tok );
    }
}


/**
 *  Counts the comma separated items of the value starting at `from`, and
 *  reports whether any of them is made of more than one word, and whether any
 *  of them is empty.
 */
static uint CountItems( CssFmt* fmt, uint from, Bool* multiword, Bool* empty )
{
    uint ix, items = 1, words = 0;

    *multiword = no;
    *empty = no;

    for ( ix = from; ix < fmt->ntoks; ++ix )
    {
        if ( fmt->toks[ix].kind == CssTokComma )
        {
            if ( words == 0 )
                *empty = yes;
            words = 0;
            ++items;
            continue;
        }

        if ( words > 0 && fmt->toks[ix].sep )
            *multiword = yes;
        ++words;
    }

    if ( words == 0 )
        *empty = yes;

    return items;
}


/**
 *  Writes a selector or an at-rule prelude, along with the brace that opens
 *  its block. A list of selectors too long for one line gets a line each.
 */
static void WritePrelude( CssFmt* fmt, uint indent )
{
    uint ix, from, width;
    Bool commas = no;

    SpacePrelude( fmt );

    for ( ix = 0; ix < fmt->ntoks; ++ix )
        if ( fmt->toks[ix].kind == CssTokComma )
            commas = yes;

    width = indent + TokensWidth( fmt, 0, fmt->ntoks ) + 2;

    if ( !commas || !fmt->wrap || width < fmt->wrap )
    {
        StartLine( fmt, indent );
        AddTokens( fmt, 0, fmt->ntoks, indent + fmt->spaces );
        AddText( fmt, " {", 2 );
        return;
    }

    for ( ix = 0, from = 0; ix <= fmt->ntoks; ++ix )
    {
        if ( ix < fmt->ntoks && fmt->toks[ix].kind != CssTokComma )
            continue;

        StartLine( fmt, indent );
        AddTokens( fmt, from, ix, indent + fmt->spaces );
        AddText( fmt, ix < fmt->ntoks ? "," : " {", ix < fmt->ntoks ? 1 : 2 );
        from = ix + 1;
    }
}


/**
 *  Writes a declaration, following it with a semicolon if `semicolon` says the
 *  statement is one that ought to end in one.
 *
 *  A value made of several comma separated items is given a line per item,
 *  which is what makes the likes of a font stack or a list of transitions
 *  readable; a value whose items are single words, as most of them are, is
 *  left on one line.
 */
static void WriteDeclaration( CssFmt* fmt, uint indent, Bool semicolon )
{
    uint colon = FindColon( fmt );
    uint ix, from, width, items = 0;
    Bool multiword = no, empty = no, split = no;

    SpaceDeclaration( fmt, colon );

    width = indent + TokensWidth( fmt, 0, fmt->ntoks ) + ( semicolon ? 1 : 0 );

    if ( colon < fmt->ntoks )
    {
        items = CountItems( fmt, colon + 1, &multiword, &empty );
        split = ( items > 1 && !empty &&
                  ( multiword || ( fmt->wrap && width >= fmt->wrap ) ) );
    }

    if ( !split )
    {
        StartLine( fmt, indent );
        AddTokens( fmt, 0, fmt->ntoks, indent + fmt->spaces );
        if ( semicolon )
            AddText( fmt, ";", 1 );
        return;
    }

    StartLine( fmt, indent );
    AddTokens( fmt, 0, colon, indent + fmt->spaces );
    AddText( fmt, ":", 1 );

    for ( ix = colon + 1, from = ix; ix <= fmt->ntoks; ++ix )
    {
        if ( ix < fmt->ntoks && fmt->toks[ix].kind != CssTokComma )
            continue;

        StartLine( fmt, indent + fmt->spaces );
        AddTokens( fmt, from, ix, indent + ( 2 * fmt->spaces ) );
        if ( ix < fmt->ntoks )
            AddText( fmt, ",", 1 );
        else if ( semicolon )
            AddText( fmt, ";", 1 );
        from = ix + 1;
    }
}


/**
 *  Writes the comment between `start` and `end`, which is on a line of its
 *  own. The lines of a comment that spans several of them keep their relative
 *  indentation, so that any layout of its own survives; `col` is the column
 *  the comment started at in the source.
 */
static void WriteComment( CssFmt* fmt, uint start, uint end, uint indent, uint col )
{
    uint ix = start;
    Bool first = yes;

    while ( ix <= end )
    {
        uint eol = ix, stop, ws = 0;

        while ( eol < end && fmt->css[eol] != '\n' )
            ++eol;

        stop = eol;
        while ( stop > ix && IsCssWS( (byte) fmt->css[stop - 1] ) )
            --stop;

        if ( first )
        {
            StartLine( fmt, indent );
            AddText( fmt, fmt->css + ix, stop - ix );
            first = no;
        }
        else
        {
            while ( ix + ws < stop &&
                    ( fmt->css[ix + ws] == ' ' || fmt->css[ix + ws] == '\t' ) )
                ++ws;

            if ( ix + ws >= stop )
                StartLine( fmt, 0 );
            else
            {
                StartLine( fmt, indent + ( ws > col ? ws - col : 0 ) );
                AddText( fmt, fmt->css + ix + ws, stop - ix - ws );
            }
        }

        if ( eol >= end )
            break;
        ix = eol + 1;
    }
}


/**
 *  The column the given position sits at in the source.
 */
static uint SourceColumn( CssFmt* fmt, uint pos )
{
    uint start = pos;

    while ( start > 0 && fmt->css[start - 1] != '\n' )
        --start;

    return pos - start;
}


/****************************************************************************//*
 ** MARK: - Formatting
 ***************************************************************************/


/**
 *  Works through the style sheet, writing out one statement, comment or
 *  closing brace at a time.
 */
static void FormatStyleSheet( CssFmt* fmt )
{
    while ( !fmt->bail )
    {
        uint c, stop, newlines, term;

        SkipCssWS( fmt, &newlines );
        if ( fmt->pos >= fmt->len )
            break;

        /* a single blank line is worth keeping, any more of them are not */
        if ( newlines > 1 )
            AddBlankLine( fmt );

        c = (byte) fmt->css[ fmt->pos ];

        if ( c == '/' && fmt->pos + 1 < fmt->len && fmt->css[fmt->pos+1] == '*' )
        {
            if ( !ScanComment( fmt, fmt->pos, &stop ) )
            {
                fmt->bail = yes;
                break;
            }

            /* a comment that followed something on its line stays with it */
            if ( newlines == 0 && fmt->open &&
                 !HasNewline( fmt, fmt->pos, stop ) )
            {
                AddText( fmt, " ", 1 );
                AddText( fmt, fmt->css + fmt->pos, stop - fmt->pos );
            }
            else
                WriteComment( fmt, fmt->pos, stop, CurIndent( fmt ),
                              SourceColumn( fmt, fmt->pos ) );

            fmt->pos = stop;
            continue;
        }

        if ( c == '}' )
        {
            ++fmt->pos;
            if ( fmt->depth == 0 )
            {
                fmt->bail = yes;
                break;
            }
            --fmt->depth;
            StartLine( fmt, CurIndent( fmt ) );
            AddText( fmt, "}", 1 );
            continue;
        }

        term = ReadStatement( fmt );
        if ( fmt->bail )
            break;

        if ( term == '{' )
        {
            WritePrelude( fmt, CurIndent( fmt ) );
            ++fmt->depth;
        }
        else if ( fmt->ntoks > 0 )
        {
            /* the last declaration of a block gets the semicolon the source
               left off; anything else is written just as it was found, so
               that nothing is added to a style sheet that breaks off in the
               middle of a statement */
            WriteDeclaration( fmt, CurIndent( fmt ),
                              term == ';' ||
                              ( term == '}' && FindColon( fmt ) < fmt->ntoks ) );
        }
        else if ( term == ';' )
        {
            /* an empty statement says nothing, but throwing it away could
               change how a parser picks itself up after a mistake in front of
               it, so it stays where it was found */
            if ( !fmt->open )
                StartLine( fmt, CurIndent( fmt ) );
            AddText( fmt, ";", 1 );
        }

        if ( term == 0 )
            break;
    }

    if ( fmt->depth != 0 )
        fmt->bail = yes;
}


/**
 *  See css.h for documentation.
 */
TY_PRIVATE Bool TY_(FormatCSS)( TidyDocImpl* doc, ctmbstr css, uint len,
                                uint indent, TidyBuffer* out )
{
    CssFmt fmt;
    Bool ok;

    if ( css == NULL || len == 0 || HasMarkup( css, len ) )
        return no;

    TidyClearMemory( &fmt, sizeof(fmt) );
    fmt.doc = doc;
    fmt.css = css;
    fmt.len = len;
    fmt.out = out;
    fmt.base = indent;
    fmt.spaces = cfg( doc, TidyIndentSpaces );
    fmt.wrap = cfg( doc, TidyWrapLen );

    /* the wrap margin is of no use once it is narrower than the indent */
    if ( fmt.wrap && fmt.wrap <= indent + fmt.spaces )
        fmt.wrap = 0;

    tidyBufInitWithAllocator( &fmt.pool, doc->allocator );
    tidyBufInitWithAllocator( &fmt.line, doc->allocator );

    FormatStyleSheet( &fmt );

    if ( !fmt.bail )
        FlushLine( &fmt );

    ok = ( !fmt.bail && out->size > 0 );
    if ( !ok )
        tidyBufClear( out );

    tidyBufFree( &fmt.pool );
    tidyBufFree( &fmt.line );
    if ( fmt.toks )
        TidyDocFree( doc, fmt.toks );

    return ok;
}
