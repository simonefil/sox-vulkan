/*
 * samples.c: Given a list of decimal values,
 * output binary little-endian sample values for them.
 */

#include <stdlib.h>
#include <stdio.h>

static void usage(void)
{
    fprintf(stderr, "Usage: samples [-b n] [sample_value] ...\n");
    fprintf(stderr, "-b Set the number of bits to output per sample (default: 16)\n");
}

int
main(int argc, char **argv)
{
    int bits = 16;
    int i;

    if (argv[1][0] == '-') switch (argv[1][1]) {
    case 'b':	/* Number of bits per sample */
	if (argv[1][2] == '\0') {
	    /* Separate numeric argument */
	    if (argc < 3) { usage(); exit(1); }
	    bits = atoi(argv[2]);
	    argv += 2; argc -= 2;
	} else {
	    /* Numeric argument attached to -b */
	    bits = atoi(&argv[1][2]);
	    argv++; argc--;
	}
	switch (bits) {
	case 8: case 16: case 24: case 32:
	    break;
	default:
	    fprintf(stderr, "The number of bits must be 8, 16, 24 or 32.\n");
	    exit(1);
	}
    }

    for (i=1; i<argc; i++) {
	long val = atol(argv[i]);
	putchar(val & 0xFF);
	if (bits > 8) putchar((val >> 8) & 0xFF);
	if (bits > 16) putchar((val >> 16) & 0xFF);
	if (bits > 24) putchar((val >> 24) & 0xFF);
    }
    return 0;
}
