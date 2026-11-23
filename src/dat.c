/* libSoX text format file.  Tom Littlejohn, March 93.
 *
 * Reads/writes sound files as text.
 *
 * Copyright 1998-2006 Chris Bagwell and SoX Contributors
 * This source code is freely redistributable and may be used for
 * any purpose.  This copyright notice must be maintained.
 * Lance Norskog And Sundry Contributors are not responsible for
 * the consequences of using this software.
 */

#include "sox_i.h"

/* Maximum length of a header comment line */
#define LINEWIDTH (size_t)256

/* Maximum length of a floating point number.
 * Tempo is written "%15.8g" and samples "%15.11g"
 * In practice, it can output both
 * -0.0012345678901 and
 * -1.2345678901e-10 which are 16 and 17 characters.
 * A 64-bit float has about 16 decimal digits of precision
 * and sox only goes down to e-10 before outputting 0 (32-bit integer)
 * but it could be e-315 if they generate a file with something else.
 * 
 * Use something much bigger - it's only extra stack space so costs no speed.
 */
#define MAX_NUM_LEN 31 /* so text[32] */

/* Private data for dat file */
typedef struct {
    double timevalue, deltat;  /* Only used when writing */
    unsigned line_no;  /* Line number in the input file that we're reading */
} priv_t;

static int sox_datstartread(sox_format_t * ft)
{
    priv_t * p = (priv_t *) ft->priv;
    long rate;
    unsigned int chan;

    p->line_no = 1;

    /* Read up to the first non-comment line */

    /*
     * Whitespace is allowed before the ; and blank lines before, between and
     * after the initial comment block.
     */
    do {
      sox_uint8_t sc;

      if (lsx_readb(ft, &sc)) goto got_eof;
      switch (sc) {
      case ' ': case '\t': case '\r':
          continue;
      case '\n':
          p->line_no++;
          /* Ignore blank lines */
          continue;
      case ';':
          {
            /* We can tolerate having the same maximum length for comment lines
             * as old sox */
            char inpstr[LINEWIDTH];

            if (lsx_reads(ft, inpstr, LINEWIDTH-1) == SOX_EOF)
              /* EOF in the middle of a header comment?! It'll never work. */
              return SOX_EOF;
            p->line_no++;
            if (sscanf(inpstr," Sample Rate %ld", &rate)) {
              ft->signal.rate=rate;
            } else if (sscanf(inpstr," Channels %d", &chan)) {
              ft->signal.channels=chan;
            }
        }
        break;
      default:
        lsx_unreadb(ft, sc);
        goto do_break; /* out of the while loop */
      }
    } while(1);
do_break:

    /* Default channels to 1 if not found or given with -r */
    if (ft->signal.channels == 0) {
       lsx_warn("Channels not given in `%s', assuming mono", ft->filename);
       ft->signal.channels = 1;
    }

    ft->encoding.encoding = SOX_ENCODING_FLOAT_TEXT;
    ft->data_start = lsx_tell(ft);

    return (SOX_SUCCESS);

got_eof:
    lsx_fail_errno(ft, SOX_EOF, "no data");
    return (SOX_EOF);
}

static const char write_error_msg[] = "write error";
#define write_error() { \
    lsx_fail_errno(ft, SOX_EOF, write_error_msg); \
    return (SOX_EOF); \
}

static int sox_datstartwrite(sox_format_t * ft)
{
    priv_t * dat = (priv_t *) ft->priv;
    char s[LINEWIDTH];

    dat->timevalue = 0.0;
    dat->deltat = 1.0 / (double)ft->signal.rate;
    /* Write format comments to start of file */
    sprintf(s,"; Sample Rate %ld\015\n", (long)ft->signal.rate);
    if (lsx_writes(ft, s))
        write_error();
    sprintf(s,"; Channels %d\015\n", (int)ft->signal.channels);
    if (lsx_writes(ft, s))
        write_error();

    return (SOX_SUCCESS);
}

/* Read a textual number and deposit it in *dp.
 *
 * The number may be preceded by whitespace and is terminated by
 * a whitespace character or a \r\n sequence.
 * We leave the terminating character in the input stream.
 *
 * Returns SOX_SUCCESS if we got a number followed by whitespace or [\r]\n
 * SOX_EOF if EOF, if we get nonnumeric chars or if [\r]\n is found before
 * a number is found..
 */
static int read_number(sox_format_t * ft, double *dp)
{
    priv_t * p = (priv_t *) ft->priv;
    sox_uint8_t sc;
    int nchars = 0;  /* How many characters are in the text of this number? */
    int nconsumed;   /* How many chars did the sscanf recognize? */
    char text[MAX_NUM_LEN + 1];

    /* Copy a number into text[] */

    /* This is where we should normally find EOF, when expecting a tempo.
     * lsx_readb() records it as an error: clear it, it is not one. */
    if (lsx_readb(ft, &sc)) {
      ft->sox_errno = SOX_SUCCESS;
      return SOX_EOF;
    }

    /* Skip leading whitespace, the same as sscanf() except for \n */
    do {
      switch (sc) {
      case ' ': case '\t': case '\f': case '\v': case '\r':
        goto do_continue;
      case '\n':
        /* Old sox doesn't tolerate blank lines between sample data lines
         * so don't let people create .dat files that not all SoXen can read */
        lsx_fail_errno(ft, SOX_EINVAL, "is missing a number at line %u",
                       p->line_no);
        return SOX_EOF;
      case '0': case '1': case '2': case '3': case '4':
      case '5': case '6': case '7': case '8': case '9':
      case '.': case '-':
        goto got_start_of_number; /* Alas, poor "break" */
      default:
        lsx_fail("couldn't find a number on line %u of `%s'",
                 p->line_no, ft->filename);
        return SOX_EOF;
      }
      if (lsx_readb(ft, &sc)) {
        /* End of file should only come after a newline */
        lsx_warn("Data file `%s' seems truncated", ft->filename);
        return SOX_EOF;
      }
do_continue:
      if (lsx_readb(ft, &sc)) {
        /* Shouldn't get EOF in the middle of a line */
        goto got_eof;
      }
    } while(1);  /* It breaks out of the middle of the switch */

got_start_of_number:

    text[nchars++] = sc;
    do {
      if (lsx_readb(ft, &sc)) {
        lsx_fail_errno(ft, SOX_EOF, "found end of data file when expecting a number");
        return SOX_EOF;
      }
      switch (sc) {

      /* Look for terminator char */
      case ' ': case '\t': case '\r': case '\f': case '\v': case '\n':
        /* and pretend we didn't read it */
        lsx_unreadb(ft, sc);
        goto got_end_of_number; /* Alas, poor "break" */

      case '0': case '1': case '2': case '3': case '4':
      case '5': case '6': case '7': case '8': case '9':
      case '.': case 'e': case 'E': case '+': case '-':
        if (nchars > MAX_NUM_LEN) {
          lsx_fail_errno(ft, SOX_EINVAL, "overlong number on line %u of `%s'",
                   p->line_no, ft->filename);
          return SOX_EOF;
        }
        text[nchars++] = sc;
        continue;

      default:
        lsx_fail_errno(ft, EINVAL, "non-numeric character on line %u of `%s'",
                       p->line_no, ft->filename);
        return SOX_EOF;
      }
    } while(1); /* It breaks out in the middle of the case */

got_end_of_number:
    text[nchars] = '\0';

    if (lsx_sscanf(text, "%lg%n", dp, &nconsumed) != 1 || nconsumed != nchars ||
        !isfinite(*dp)) {
      lsx_fail_errno(ft, SOX_EINVAL, "bad number at line %d of `%s'",
                     p->line_no, ft->filename);
      return SOX_EOF;
    }
    return SOX_SUCCESS;

got_eof:
    lsx_warn("the file seems truncated");
    return SOX_EOF;
}

static size_t sox_datread(sox_format_t * ft, sox_sample_t *buf, size_t nsamp)
{
    priv_t * p = (priv_t *) ft->priv;
    sox_uint8_t sc;
    double sampval;
    size_t done = 0;
    unsigned int chan;  /* The channel we are reading in */

    /* Always read a complete set of channels */
    nsamp -= (nsamp % ft->signal.channels);

    while (done < nsamp) {
      if (lsx_readb(ft, &sc)) {
        /* Getting EOF after reading a complete line is the normal case,
         * not the error lsx_readb() recorded */
        ft->sox_errno = SOX_SUCCESS;
        return done;
      }
      /* Skip over comments - ie. 0 or more whitespace
       * then ';' up to and including '\n' */
      do {
        switch (sc) {
        /* Ignore the same initial whitespace as sscanf(), same as isspace() */
        case ' ': case '\t': case '\f': case '\v': case '\r':
          /* Not \n as old sox doesn't allow blank lines in the sample data */
          goto do_continue;
        case ';':
          do {
            if (lsx_readb(ft, &sc))
              goto got_eof;
          } while (sc != '\n');
          break; /* the switch */
        default:
          /* It's not a comment line or we hit \n */
          lsx_unreadb(ft, sc);
          goto read_frame; /* Alas, poor "break" */
        }
do_continue:
        if (lsx_readb(ft, &sc))
          goto got_eof;
      } while(1);

read_frame:

      /* Ignore the tempo string */
      ft->sox_errno = 0;
      if (read_number(ft, &sampval) == SOX_EOF) {
        /* This is where we should normally encounter EOF but it also
         * signals invalid text in the input, in which case read_number()
         * will already have printed an error message. */
        if (ft->sox_errno) return SOX_EOF;
        return done;
      }
      if (lsx_readb(ft, &sc) || sc != ' ') {
           lsx_warn("tempo with no sample data at line %u", p->line_no);
           return done;
      }

      /* Read a complete set of channels */
      for (chan = 0; chan < ft->signal.channels; chan++) {
        SOX_SAMPLE_LOCALS;

        if (read_number(ft, &sampval) == SOX_EOF) {
          /* read_number() will already have printed an error message */
          return 0;
        }

        *buf++ = SOX_FLOAT_64BIT_TO_SAMPLE(sampval, ft->clips);

        /* Read the terminating character that read_number() left us
         * in the pending characters buffer.
         */
        if (lsx_readb(ft, &sc)) {
          /* If EOF, return the sample frames that we managed to read
           * but not any samples in a half-complete current one.
           * Actually, this can't happen bcos lsx_unreadb() never fails
           * so lsx_readb is guaranteed to return something.
           */
          done -= chan;
          goto got_eof;
        }
        /* Check it's what it should be */

        /* Was this the last channel of one line? */
        if (chan == ft->signal.channels - 1) {
          /* Yes. Eat trailing spaces, optional CR and newline */
          while (sc == ' ') if (lsx_readb(ft, &sc)) goto got_eof;
          if (sc == '\r') if (lsx_readb(ft, &sc)) goto got_eof;
          if (sc == '\n') {
            p->line_no++;
            done++;
            break; /* out of the "chan" loop */
          }
          /* Old sox ignores any trailing garbage at the end of the line */
          while (lsx_readb(ft, &sc) == SOX_SUCCESS && sc != '\n')
            ;
        } else {
          /* We read either a tempo or a sample that's not the last channel */
          if (sc != ' ') {
            lsx_warn("missing channel data on line %u of `%s'",
                      p->line_no, ft->filename);
            /* Return the whole sample frames completed so far */
            done -= chan;
            return (done);
          }
        }
        done++;
      }
    }

    return (done);

got_eof:
    /* Unexpected EOF in the middle of a line */
    lsx_warn("data file `%s' seems truncated", ft->filename);
    return done;
}

static size_t sox_datwrite(sox_format_t * ft, const sox_sample_t *buf, size_t nsamp)
{
    priv_t * dat = (priv_t *) ft->priv;
    size_t done = 0;
    double sampval=0.0;
    char s[LINEWIDTH];
    size_t i=0;

    /* Always write a complete set of channels */
    nsamp -= (nsamp % ft->signal.channels);

    /* Write time, then sample values, then CRLF newline */
    while(done < nsamp) {
      sprintf(s," %15.8g ",dat->timevalue);
      if (lsx_writes(ft, s))
        write_error();
      for (i=0; i<ft->signal.channels; i++) {
        sampval = SOX_SAMPLE_TO_FLOAT_64BIT(*buf++, ft->clips);
        sprintf(s," %15.11g", sampval);
        if (lsx_writes(ft, s))
          write_error();
        done++;
      }
      sprintf(s," \r\n");
      if (lsx_writes(ft, s))
        write_error();
      dat->timevalue += dat->deltat;
    }
    return done;
}

LSX_FORMAT_HANDLER(dat)
{
  static char const * const names[] = {"dat", NULL};
  static unsigned const write_encodings[] = {SOX_ENCODING_FLOAT_TEXT, 0, 0};
  static sox_format_handler_t const handler = {SOX_LIB_VERSION_CODE,
    "Textual representation of the sampled audio", names, 0,
    sox_datstartread, sox_datread, NULL,
    sox_datstartwrite, sox_datwrite, NULL,
    NULL, write_encodings, NULL, sizeof(priv_t)
  };
  return &handler;
}
