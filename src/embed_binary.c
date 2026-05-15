/* Convert a binary file to a C uint32_t array.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Marking the bail-out path as non-returning keeps the checks below readable:
 * without it every analyser assumes execution falls through a failed open or
 * a short read and reports the buffers as possibly uninitialised. */
#if defined(__GNUC__) || defined(__clang__)
#define EMBED_NORETURN __attribute__((noreturn))
#elif defined(_MSC_VER)
#define EMBED_NORETURN __declspec(noreturn)
#else
#define EMBED_NORETURN
#endif

EMBED_NORETURN static void fail(char const *message)
{
  fprintf(stderr, "embed_binary: %s\n", message);
  exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
  FILE *input;
  FILE *output;
  long length;
  uint8_t *bytes;
  size_t words;
  size_t index;

  if (argc != 4)
    fail("usage: embed_binary INPUT OUTPUT SYMBOL");
  input = fopen(argv[1], "rb");
  if (!input || fseek(input, 0, SEEK_END) ||
      (length = ftell(input)) <= 0 || fseek(input, 0, SEEK_SET))
    fail("cannot open or size input");
  if ((length & 3) != 0)
    fail("input size is not a multiple of four");
  bytes = malloc((size_t)length);
  if (!bytes ||
      fread(bytes, 1, (size_t)length, input) != (size_t)length)
    fail("cannot read input");
  if (fclose(input))
    fail("cannot close input");
  output = fopen(argv[2], "wb");
  if (!output)
    fail("cannot create output");
  words = (size_t)length / 4u;
  fprintf(output, "static uint32_t const %s[] = {", argv[3]);
  for (index = 0; index < words; ++index) {
    uint32_t value =
        (uint32_t)bytes[index * 4u] |
        ((uint32_t)bytes[index * 4u + 1u] << 8) |
        ((uint32_t)bytes[index * 4u + 2u] << 16) |
        ((uint32_t)bytes[index * 4u + 3u] << 24);
    fprintf(output, "%s0x%08xU",
        !index ? "\n  " : index % 8u ? ", " : ",\n  ", value);
  }
  fprintf(output,
      "\n};\nstatic size_t const %s_size = sizeof(%s);\n",
      argv[3], argv[3]);
  free(bytes);
  if (fclose(output))
    fail("cannot close output");
  return EXIT_SUCCESS;
}
