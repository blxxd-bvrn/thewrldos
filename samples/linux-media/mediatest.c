/* libjpeg, libpng and SQLite on THE WRLD OS's Linux layer (roadmap milestone 9): the build
 * machine's libjpeg-turbo, libpng and SQLite, as a distribution ships them.
 *   JPEG: decode the OS's own wallpapers, encode and decode again (baseline and progressive)
 *   PNG:  write and read back RGBA, plain and interlaced, on C: and in /tmp
 *   SQLite: a database on C: (it outlives a restart), transactions, rollback, an index,
 *           integrity_check; two processes on one database (record locks: the second is told
 *           the database is busy while the first writes); WAL mode across processes (a shared
 *           mapping of the -shm file) */
#include <stddef.h>
#include <stdio.h>
#include <jpeglib.h>
#include <png.h>
#include <sqlite3.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static int passed, failed;
static void check(const char *what, int ok) { printf("%s  %s\n", ok ? "PASS" : "FAIL", what); fflush(stdout); if (ok) passed++; else failed++; }

/* ---------------- JPEG ---------------- */
struct jerr { struct jpeg_error_mgr pub; jmp_buf jb; };
static void jerr_exit(j_common_ptr c) { char m[JMSG_LENGTH_MAX]; c->err->format_message(c, m); printf("      libjpeg: %s\n", m); longjmp(((struct jerr *)c->err)->jb, 1); }
static unsigned char *jpeg_load(const char *path, int *w, int *h, int *progressive) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    struct jpeg_decompress_struct d; struct jerr e; unsigned char *px = NULL;
    d.err = jpeg_std_error(&e.pub); e.pub.error_exit = jerr_exit;
    if (setjmp(e.jb)) { jpeg_destroy_decompress(&d); fclose(f); free(px); return NULL; }
    jpeg_create_decompress(&d); jpeg_stdio_src(&d, f); jpeg_read_header(&d, TRUE);
    d.out_color_space = JCS_RGB; jpeg_start_decompress(&d);
    *w = d.output_width; *h = d.output_height; if (progressive) *progressive = jpeg_has_multiple_scans(&d);
    px = malloc((size_t)*w * *h * 3);
    while (d.output_scanline < d.output_height) { JSAMPROW row = px + (size_t)d.output_scanline * *w * 3; jpeg_read_scanlines(&d, &row, 1); }
    jpeg_finish_decompress(&d); jpeg_destroy_decompress(&d); fclose(f);
    return px;
}
static int jpeg_save(const char *path, const unsigned char *px, int w, int h, int quality, int progressive) {
    FILE *f = fopen(path, "wb"); if (!f) return 0;
    struct jpeg_compress_struct c; struct jerr e;
    c.err = jpeg_std_error(&e.pub); e.pub.error_exit = jerr_exit;
    if (setjmp(e.jb)) { jpeg_destroy_compress(&c); fclose(f); return 0; }
    jpeg_create_compress(&c); jpeg_stdio_dest(&c, f);
    c.image_width = w; c.image_height = h; c.input_components = 3; c.in_color_space = JCS_RGB;
    jpeg_set_defaults(&c); jpeg_set_quality(&c, quality, TRUE);
    if (progressive) jpeg_simple_progression(&c);
    jpeg_start_compress(&c, TRUE);
    while (c.next_scanline < c.image_height) { JSAMPROW row = (JSAMPROW)px + (size_t)c.next_scanline * w * 3; jpeg_write_scanlines(&c, &row, 1); }
    jpeg_finish_compress(&c); jpeg_destroy_compress(&c); fclose(f);
    return 1;
}
static void test_jpeg(void) {
    int w = 0, h = 0, prog = 0;
    unsigned char *wp = jpeg_load("/mnt/c/WRLD/Wallpapers/Blue shoal.jpg", &w, &h, &prog);
    long sum = 0; if (wp) for (long i = 0; i < (long)w * h * 3; i++) sum += wp[i];
    printf("      a wallpaper: %dx%d, mean level %ld\n", w, h, wp ? sum / ((long)w * h * 3) : 0);
    check("libjpeg decodes a wallpaper of the OS", wp && w > 100 && h > 100 && sum > 0);
    free(wp);
    int W = 320, H = 200;
    unsigned char *src = malloc(W * H * 3);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { unsigned char *p = src + (y * W + x) * 3; p[0] = x * 255 / W; p[1] = y * 255 / H; p[2] = (x + y) & 255 ? 128 : 0; }
    for (int pr = 0; pr < 2; pr++) {
        const char *path = pr ? "/tmp/progressive.jpg" : "/mnt/c/jpeg-test.jpg";
        int ok = jpeg_save(path, src, W, H, 92, pr), w2, h2, p2 = -1;
        unsigned char *back = ok ? jpeg_load(path, &w2, &h2, &p2) : NULL;
        long err = 0; int maxe = 0;
        if (back && w2 == W && h2 == H) for (int i = 0; i < W * H * 3; i++) { int d = abs(back[i] - src[i]); err += d; if (d > maxe) maxe = d; }
        printf("      %s: mean error %.2f, largest %d\n", path, back ? (double)err / (W * H * 3) : -1.0, maxe);
        check(pr ? "libjpeg: a progressive JPEG, written and read back" : "libjpeg: a baseline JPEG written to C: and read back",
              back && w2 == W && h2 == H && err / (W * H * 3) < 4 && p2 == pr);
        free(back);
    }
    free(src);
}

/* ---------------- PNG ---------------- */
static int png_save(const char *path, const unsigned char *px, int w, int h, int interlace) {
    FILE *f = fopen(path, "wb"); if (!f) return 0;
    png_structp p = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop i = png_create_info_struct(p);
    if (setjmp(png_jmpbuf(p))) { png_destroy_write_struct(&p, &i); fclose(f); return 0; }
    png_init_io(p, f);
    png_set_IHDR(p, i, w, h, 8, PNG_COLOR_TYPE_RGBA, interlace ? PNG_INTERLACE_ADAM7 : PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_text t = { PNG_TEXT_COMPRESSION_NONE, "Software", "THE WRLD OS mediatest", 0, 0, 0, 0 };
    png_set_text(p, i, &t, 1);
    png_write_info(p, i);
    png_bytep *rows = malloc(sizeof *rows * h);
    for (int y = 0; y < h; y++) rows[y] = (png_bytep)px + (size_t)y * w * 4;
    png_write_image(p, rows); png_write_end(p, NULL);
    png_destroy_write_struct(&p, &i); free(rows); fclose(f);
    return 1;
}
static unsigned char *png_load(const char *path, int *w, int *h) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    png_structp p = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop i = png_create_info_struct(p);
    unsigned char *px = NULL; png_bytep *rows = NULL;
    if (setjmp(png_jmpbuf(p))) { png_destroy_read_struct(&p, &i, NULL); fclose(f); free(px); free(rows); return NULL; }
    png_init_io(p, f); png_read_info(p, i);
    *w = png_get_image_width(p, i); *h = png_get_image_height(p, i);
    png_set_expand(p); png_set_strip_16(p); png_set_gray_to_rgb(p); png_set_add_alpha(p, 0xFF, PNG_FILLER_AFTER);
    png_set_interlace_handling(p); png_read_update_info(p, i);
    px = malloc((size_t)*w * *h * 4); rows = malloc(sizeof *rows * *h);
    for (int y = 0; y < *h; y++) rows[y] = px + (size_t)y * *w * 4;
    png_read_image(p, rows); png_read_end(p, NULL);
    png_destroy_read_struct(&p, &i, NULL); free(rows); fclose(f);
    return px;
}
static void test_png(void) {
    int W = 300, H = 200;
    unsigned char *src = malloc(W * H * 4);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { unsigned char *q = src + (y * W + x) * 4; q[0] = x * 255 / W; q[1] = 255 - y * 255 / H; q[2] = (x ^ y) & 255; q[3] = (x + y) * 255 / (W + H); }
    for (int il = 0; il < 2; il++) {
        const char *path = il ? "/tmp/adam7.png" : "/mnt/c/png-test.png";
        int w = 0, h = 0, ok = png_save(path, src, W, H, il);
        unsigned char *back = ok ? png_load(path, &w, &h) : NULL;
        check(il ? "libpng: an interlaced (Adam7) RGBA PNG, written and read back exactly" : "libpng: an RGBA PNG written to C: and read back exactly",
              back && w == W && h == H && !memcmp(back, src, W * H * 4));
        free(back);
    }
    free(src);
}

/* ---------------- SQLite ---------------- */
static int exec(sqlite3 *db, const char *sql) { char *e = NULL; int r = sqlite3_exec(db, sql, NULL, NULL, &e); if (r != SQLITE_OK) { printf("      sqlite: %s: %s\n", sql, e ? e : "?"); sqlite3_free(e); } return r; }
static long one(sqlite3 *db, const char *sql) { sqlite3_stmt *s; long v = -1; if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int64(s, 0); sqlite3_finalize(s); return v; }
static const char *text1(sqlite3 *db, const char *sql) { static char b[64]; sqlite3_stmt *s; strcpy(b, "?"); if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW) snprintf(b, sizeof b, "%s", sqlite3_column_text(s, 0)); sqlite3_finalize(s); return b; }
/* The other process is a fresh one (this program again, started with exec): a SQLite
 * connection must not be carried across fork(), because the lock state it keeps per file
 * would come along with the copied memory. */
static const char *const self_path = "/proc/self/exe";
static pid_t spawn(const char *mode, const char *db, int up, int down) {
    char a[16], b[16]; snprintf(a, sizeof a, "%d", up); snprintf(b, sizeof b, "%d", down);
    pid_t pid = fork();
    if (pid == 0) { execl(self_path, "mediatest", mode, db, a, b, (char *)NULL); execl("/usr/local/bin/mediatest", "mediatest", mode, db, a, b, (char *)NULL); _exit(99); }
    return pid;
}
static int child(const char *mode, const char *db, int up, int down) {
    sqlite3 *d2;
    if (sqlite3_open(db, &d2) != SQLITE_OK) return 98;
    if (!strcmp(mode, "writer")) {
        sqlite3_busy_timeout(d2, 0);
        int r1 = sqlite3_exec(d2, "INSERT INTO t(name, v) VALUES ('child', 2)", NULL, NULL, NULL);
        char c = (char)r1; write(up, &c, 1);
        char go; read(down, &go, 1);                         /* the parent has committed */
        sqlite3_busy_timeout(d2, 2000);
        int r2 = sqlite3_exec(d2, "INSERT INTO t(name, v) VALUES ('child', 2)", NULL, NULL, NULL);
        if (r2 != SQLITE_OK) { printf("      the other process: %s (%d)\n", sqlite3_errmsg(d2), sqlite3_extended_errcode(d2)); fflush(stdout); }
        sqlite3_close(d2);
        return r2 == SQLITE_OK ? 0 : 10 + r2;
    }
    sqlite3_busy_timeout(d2, 2000);
    int r = sqlite3_exec(d2, "BEGIN; INSERT INTO t(name, v) SELECT 'wal', 3 FROM t LIMIT 500; COMMIT;", NULL, NULL, NULL);
    sqlite3_close(d2);
    return r == SQLITE_OK ? 0 : 1;
}

static void test_sqlite(void) {
    const char *path = "/mnt/c/WRLD/mediatest.db";
    sqlite3 *db;
    int had = access(path, F_OK) == 0;
    check("SQLite opens a database on C:", sqlite3_open(path, &db) == SQLITE_OK);
    if (had) printf("      it was already there from an earlier run: %ld rows of runs\n", one(db, "SELECT count(*) FROM runs"));
    exec(db, "PRAGMA journal_mode=DELETE");
    exec(db, "CREATE TABLE IF NOT EXISTS runs(id INTEGER PRIMARY KEY, at TEXT)");
    exec(db, "INSERT INTO runs(at) VALUES (datetime('now'))");
    exec(db, "DROP TABLE IF EXISTS t");
    exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT, v REAL)");
    exec(db, "BEGIN");
    sqlite3_stmt *ins; sqlite3_prepare_v2(db, "INSERT INTO t(name, v) VALUES (?, ?)", -1, &ins, NULL);
    for (int i = 0; i < 20000; i++) { char n[32]; snprintf(n, sizeof n, "row %d", i); sqlite3_bind_text(ins, 1, n, -1, SQLITE_TRANSIENT); sqlite3_bind_double(ins, 2, i * 0.5); sqlite3_step(ins); sqlite3_reset(ins); }
    sqlite3_finalize(ins);
    exec(db, "COMMIT");
    check("SQLite: 20,000 rows in one transaction", one(db, "SELECT count(*) FROM t") == 20000 && one(db, "SELECT sum(v) FROM t") == 99995000);
    exec(db, "BEGIN"); exec(db, "DELETE FROM t WHERE id % 2 = 0"); exec(db, "ROLLBACK");
    check("SQLite: a rolled-back delete leaves the rows", one(db, "SELECT count(*) FROM t") == 20000);
    exec(db, "CREATE INDEX t_name ON t(name)");
    check("SQLite: a lookup through an index", one(db, "SELECT id FROM t WHERE name = 'row 12345'") == 12346);
    check("SQLite: PRAGMA integrity_check is ok", !strcmp(text1(db, "PRAGMA integrity_check"), "ok"));
    /* two processes: while this one writes, the other is told the database is busy */
    exec(db, "BEGIN IMMEDIATE"); exec(db, "INSERT INTO t(name, v) VALUES ('parent', 1)");
    int up[2], down[2]; pipe(up); pipe(down);   /* child to parent, parent to child */
    pid_t pid = spawn("writer", path, up[1], down[0]);
    close(up[1]); close(down[0]);                            /* so a child that fails to start is seen */
    char r1 = -1; read(up[0], &r1, 1);
    if (exec(db, "COMMIT") != SQLITE_OK) printf("      COMMIT: %s\n", sqlite3_errmsg(db));
    write(down[1], "g", 1);
    int st = 0; waitpid(pid, &st, 0);
    printf("      the other process: %s while this one wrote, then %s\n", r1 == SQLITE_BUSY ? "SQLITE_BUSY" : sqlite3_errstr(r1), WIFEXITED(st) && WEXITSTATUS(st) == 0 ? "its write went in" : "its write failed");
    check("SQLite: record locks keep a second process out while one writes", r1 == SQLITE_BUSY);
    check("SQLite: the second process writes once the first has committed", WIFEXITED(st) && WEXITSTATUS(st) == 0 && one(db, "SELECT count(*) FROM t WHERE name IN ('parent','child')") == 2);
    /* WAL: readers and a writer in different processes share the -shm index */
    check("SQLite: WAL journal mode", !strcmp(text1(db, "PRAGMA journal_mode=WAL"), "wal"));
    pid = spawn("wal", path, -1, -1);
    waitpid(pid, &st, 0);
    long walrows = one(db, "SELECT count(*) FROM t WHERE name = 'wal'");
    printf("      rows another process wrote in WAL mode, as this one sees them: %ld\n", walrows);
    check("SQLite: in WAL mode one process sees what another committed", WIFEXITED(st) && WEXITSTATUS(st) == 0 && walrows == 500);
    exec(db, "PRAGMA wal_checkpoint(TRUNCATE)"); exec(db, "PRAGMA journal_mode=DELETE");
    check("SQLite: integrity after all of it", !strcmp(text1(db, "PRAGMA integrity_check"), "ok"));
    sqlite3_close(db);
}

int main(int argc, char **argv) {
    if (argc == 5) return child(argv[1], argv[2], atoi(argv[3]), atoi(argv[4]));
    printf("THE WRLD OS - media and storage test (libjpeg-turbo %d, libpng %s, SQLite %s)\n", LIBJPEG_TURBO_VERSION_NUMBER, png_get_libpng_ver(NULL), sqlite3_libversion());
    test_jpeg();
    test_png();
    test_sqlite();
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
