/* LZ4 and zstd over payload files: bytes, compress/decompress ns. cc -O2 codecs.c -l:liblz4.so.1 -l:libzstd.so.1 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int LZ4_compressBound(int);
int LZ4_compress_default(const char *, char *, int, int);
int LZ4_decompress_safe(const char *, char *, int, int);
size_t ZSTD_compressBound(size_t);
size_t ZSTD_compress(void *, size_t, const void *, size_t, int);
size_t ZSTD_decompress(void *, size_t, const void *, size_t);

static size_t lz4_c(char *d, size_t cap, const char *s, size_t n, int l) { return (size_t) LZ4_compress_default(s, d, (int) n, (int) cap); }
static size_t lz4_d(char *d, size_t cap, const char *s, size_t n) { return (size_t) LZ4_decompress_safe(s, d, (int) n, (int) cap); }
static size_t zstd_c(char *d, size_t cap, const char *s, size_t n, int l) { return ZSTD_compress(d, cap, s, n, l); }
static size_t zstd_d(char *d, size_t cap, const char *s, size_t n) { return ZSTD_decompress(d, cap, s, n); }

static const struct {
	const char *name;
	size_t (*c)(char *, size_t, const char *, size_t, int);
	size_t (*d)(char *, size_t, const char *, size_t);
	int level;
} codecs[] = {{"lz4", lz4_c, lz4_d, 0}, {"zstd-1", zstd_c, zstd_d, -1}, {"zstd1", zstd_c, zstd_d, 1}, {"zstd3", zstd_c, zstd_d, 3}};

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e9 + t.tv_nsec;
}

int main(int argc, char **argv)
{
	for (int a = 1; a < argc; a++) {
		FILE *f = fopen(argv[a], "rb");
		fseek(f, 0, SEEK_END);
		size_t n = (size_t) ftell(f);
		fseek(f, 0, SEEK_SET);
		size_t cap = ZSTD_compressBound(n) + 64;
		char *src = malloc(n), *dst = malloc(cap), *back = malloc(n);
		if (fread(src, 1, n, f) != n) return 1;
		fclose(f);
		int it = 1 + 20000000 / (int) (n + 64);
		for (size_t k = 0; k < sizeof codecs / sizeof *codecs; k++) {
			size_t c = codecs[k].c(dst, cap, src, n, codecs[k].level);
			if (codecs[k].d(back, n, dst, c) != n || memcmp(src, back, n) != 0) return 1;
			double best[2] = {1e18, 1e18};
			for (int r = 0; r < 5; r++) {
				double t = now();
				for (int i = 0; i < it; i++) codecs[k].c(dst, cap, src, n, codecs[k].level);
				double m = now();
				for (int i = 0; i < it; i++) codecs[k].d(back, n, dst, c);
				double e = now();
				if ((m - t) / it < best[0]) best[0] = (m - t) / it;
				if ((e - m) / it < best[1]) best[1] = (e - m) / it;
			}
			printf("%s\t%zu\t%s\t%zu\t%.0f\t%.0f\n", strrchr(argv[a], '/') + 1, n, codecs[k].name, c, best[0], best[1]);
		}
		free(src); free(dst); free(back);
	}
	return 0;
}
