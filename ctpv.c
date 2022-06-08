#include <stdio.h>
#include <magic.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <openssl/md5.h>

#include "ctpv.h"
#include "error.h"
#include "utils.h"
#include "config.h"
#include "server.h"
#include "preview.h"
#include "previews.h"

const char any_type[] = ANY_TYPE;

static magic_t magic;

static struct {
    enum {
        MODE_PREVIEW,
        MODE_SERVER,
        MODE_CLEAR,
        MODE_END,
        MODE_LIST,
        MODE_MIME,
    } mode;
    char *server_id_s;
} ctpv = { .mode = MODE_PREVIEW };

static VectorPreview *previews;

static void cleanup(void)
{
    previews_cleanup();
    config_cleanup();
    if (magic != NULL)
        magic_close(magic);
    if (previews)
        vectorPreview_free(previews);
}

static int init_magic(void)
{
    ERRCHK_RET(!(magic = magic_open(MAGIC_MIME_TYPE)), FUNCFAILED("magic_open"),
               magic_error(magic));

    ERRCHK_RET(magic_load(magic, NULL) != 0, FUNCFAILED("magic_load"),
               magic_error(magic));

    return OK;
}

static int create_dir(char *buf, size_t len)
{
    char dir[len];
    strncpy(dir, buf, LEN(dir) - 1);
    ERRCHK_RET(mkpath(dir, 0700) == -1, FUNCFAILED("mkpath"), ERRNOS);

    return OK;
}

static int get_config_file(char *buf, size_t len)
{
    ERRCHK_RET_OK(get_config_dir(buf, len, "ctpv/"));
    ERRCHK_RET_OK(create_dir(buf, len));

    strncat(buf, "config", len - 1);

    if (access(buf, F_OK) != 0)
        close(creat(buf, 0600));

    return OK;
}

static int init_previews(void)
{
    /* 20 is some arbitrary number, it's here in order to
     * to save one realloc() if user has less then 20 custom previews */
    previews = vectorPreview_new(LEN(b_previews) + 20);
    vectorPreview_append_arr(previews, b_previews, LEN(b_previews));

    char config_file[FILENAME_MAX];
    get_config_file(config_file, LEN(config_file));

    ERRCHK_RET_OK(config_load(previews, config_file));

    previews_init(previews->buf, previews->len);

    return OK;
}

static const char *get_mimetype(const char *path)
{
    const char *r = magic_file(magic, path);
    if (!r) {
        PRINTINTERR(FUNCFAILED("magic_file"), magic_error(magic));
        return NULL;
    }

    return r;
}

static int check_file(const char *f)
{
    if (!f) {
        print_error("file not given");
        return ERR;
    }

    if (access(f, R_OK) != 0) {
        print_errorf("failed to access '%s': %s", f, ERRNOS);
        return ERR;
    }

    return OK;
}

static int is_newer(int *resp, char *f1, char *f2)
{
    struct stat stat1, stat2;
    ERRCHK_RET(stat(f1, &stat1) == -1, FUNCFAILED("stat"), ERRNOS);
    ERRCHK_RET(stat(f2, &stat2) == -1, FUNCFAILED("stat"), ERRNOS);

    int sec_d = stat1.st_mtim.tv_sec - stat2.st_mtim.tv_sec;
    if (sec_d < 0)
        goto older;
    else if (sec_d == 0 && stat1.st_mtim.tv_nsec <= stat2.st_mtim.tv_nsec)
        goto older;

    *resp = 1;
    return OK;

older:
    *resp = 0;
    return OK;
}

static void md5_string(char *buf, size_t len, char *s)
{
    unsigned char out[MD5_DIGEST_LENGTH];
    char b[16];

    MD5((const unsigned char *)s, strlen(s), out);

    buf[0] = '\0';
    for(unsigned int i = 0; i < LEN(out); i++) {
        snprintf(b, LEN(b)-1, "%02x", out[i]);
        strncat(buf, b, len);
    }
}

static int get_cache_file(char *buf, size_t len, char *file)
{
    ERRCHK_RET_OK(get_cache_dir(buf, len, "ctpv/"));
    ERRCHK_RET_OK(create_dir(buf, len));

    char name[64];
    md5_string(name, LEN(name) - 1, file);
    strncat(buf, name, len - 1);

    return OK;
}

static int check_cache(int *resp, char *file, char *cache_file)
{
    if (access(cache_file, F_OK) != 0) {
        *resp = 0;
        return OK;
    }

    return is_newer(resp, cache_file, file);
}

#define GET_PARG(a, i) (a) = argc > (i) ? argv[i] : NULL

static int preview(int argc, char *argv[])
{
    char *f, *w, *h, *x, *y, *id;
    GET_PARG(f, 0);
    GET_PARG(w, 1);
    GET_PARG(h, 2);
    GET_PARG(x, 3);
    GET_PARG(y, 4);
    GET_PARG(id, 5);

    ERRCHK_RET_OK(check_file(f));

    ERRCHK_RET_OK(init_magic());

    ERRCHK_RET_OK(init_previews());

    const char *mimetype;
    ERRCHK_RET(!(mimetype = get_mimetype(f)));

    char cache_file[FILENAME_MAX];
    ERRCHK_RET_OK(get_cache_file(cache_file, LEN(cache_file), f));

    int cache_valid;
    ERRCHK_RET_OK(check_cache(&cache_valid, f, cache_file));

    PreviewArgs args = {
        .f = f, .w = w, .h = h, .x = x, .y = y, .id = id,
        .cache_file = cache_file, .cache_valid = cache_valid,
    };

    return preview_run(get_ext(f), mimetype, &args);
}

static int server(void)
{
    return server_listen(ctpv.server_id_s);
}

static int clear(void)
{
    return server_clear(ctpv.server_id_s);
}

static int end(void)
{
    return server_end(ctpv.server_id_s);
}

static int list(void)
{
    ERRCHK_RET_OK(init_previews());

    size_t len;
    Preview p, **list = previews_get(&len);
    const char *n, *e, *t, *s;

    const char header_name[] = "Name", header_ext[] = "Extension",
               header_mime[] = "MIME type";

    int width_name = 0, width_ext = 0;

    for (size_t i = 0; i < len + 1; i++) {
        if (i < len) {
            p = *list[i];
            n = p.name;
            e = p.ext;
        } else {
            n = header_name;
            e = header_ext;
        }

        int name_len = strlennull(n);
        int ext_len = strlennull(e);
        width_name = MAX(width_name, name_len);
        width_ext = MAX(width_ext, ext_len);
    }

    width_name += 2, width_ext += 2;

    puts("List of available previews:");
    printf("\t%-*s %-*s %s\n", width_name, header_name, width_ext, header_ext,
           header_mime);

    for (size_t i = 0; i < len; i++) {
        p = *list[i];
        e = p.ext;
        t = p.type;
        s = p.subtype;

        if (!e)
            e = any_type;

        if (!t) {
            t = any_type;
            s = any_type;
        } else if (!s) {
            s = any_type;
        }

        printf("\t%-*s .%-*s %s/%s\n", width_name, p.name, width_ext - 1, e, t,
               s);
    }

    puts("\nNote: '" ANY_TYPE "' means that it matches any.");

    return OK;
}

static int mime(int argc, char *argv[])
{
    const char *f, *mimetype;

    for (int i = 0; i < argc; i++) {
        f = argv[i];
        ERRCHK_RET_OK(check_file(f));

        ERRCHK_RET_OK(init_magic());

        mimetype = get_mimetype(f);
        ERRCHK_RET(!mimetype);

        if (argc > 1)
            printf("%s: ", f);

        printf(".%s ", get_ext(f));
        puts(mimetype);
    }

    return OK;
}

int main(int argc, char *argv[])
{
    program = argc > 0 ? argv[0] : "ctpv";

    int c;
    while ((c = getopt(argc, argv, "s:c:e:lm")) != -1) {
        switch (c) {
        case 's':
            ctpv.mode = MODE_SERVER;
            ctpv.server_id_s = optarg;
            break;
        case 'c':
            ctpv.mode = MODE_CLEAR;
            ctpv.server_id_s = optarg;
            break;
        case 'e':
            ctpv.mode = MODE_END;
            ctpv.server_id_s = optarg;
            break;
        case 'l':
            ctpv.mode = MODE_LIST;
            break;
        case 'm':
            ctpv.mode = MODE_MIME;
            break;
        default:
            return EXIT_FAILURE;
        }
    }

    argc -= optind;
    argv = &argv[optind];

    int ret;
    switch (ctpv.mode) {
        case MODE_PREVIEW:
            ret = preview(argc, argv);
            break;
        case MODE_SERVER:
            ret = server();
            break;
        case MODE_CLEAR:
            ret = clear();
            break;
        case MODE_END:
            ret = end();
            break;
        case MODE_LIST:
            ret = list();
            break;
        case MODE_MIME:
            ret = mime(argc, argv);
            break;
        default:
            PRINTINTERR("unknowm mode: %d", ctpv.mode);
            ret = ERR;
            break;
    }

    cleanup();

    return ret == OK ? EXIT_SUCCESS : EXIT_FAILURE;
}
