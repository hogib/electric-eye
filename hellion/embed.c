// Build-time tool: turns a file into a C array, so hellion carries its
// web page inside the binary and needs nothing installed beside it.
//
//   embed <symbol> <input> <output.c>
//
// Writes `const unsigned char <symbol>[]` (NUL-terminated, for safety) and
// `const size_t <symbol>_len` (without the NUL). Built with $(HOSTCC), so
// it runs on the build machine even when cross-compiling hellion.
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  if (argc != 4) {
    fprintf(stderr, "usage: embed <symbol> <input> <output.c>\n");
    return 2;
  }
  FILE *in = fopen(argv[2], "rb");
  if (!in) {
    perror(argv[2]);
    return 1;
  }
  FILE *out = fopen(argv[3], "w");
  if (!out) {
    perror(argv[3]);
    fclose(in);
    return 1;
  }
  fprintf(out,
          "/* Generated from %s by hellion/embed.c. Do not edit. */\n"
          "#include <stddef.h>\n"
          "const unsigned char %s[] = {\n",
          argv[2], argv[1]);
  size_t n = 0;
  int c;
  while ((c = fgetc(in)) != EOF) {
    fprintf(out, "%s0x%02x,", n % 16 ? "" : "\n  ", (unsigned)c);
    n++;
  }
  fprintf(out, "\n  0x00};\nconst size_t %s_len = %lu;\n", argv[1],
          (unsigned long)n);
  fclose(in);
  return fclose(out) == 0 ? 0 : 1;
}
