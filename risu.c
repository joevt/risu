/******************************************************************************
 * Copyright (c) 2010 Linaro Limited
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * which accompanies this distribution, and is available at
 * http://www.eclipse.org/legal/epl-v10.html
 *
 * Contributors:
 *     Peter Maydell (Linaro) - initial implementation
 *****************************************************************************/


/* Random Instruction Sequences for Userspace */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <ucontext.h>
#include <setjmp.h>
#include <assert.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <string.h>

#include "risu.h"

enum {
    MASTER = 0, APPRENTICE = 1
};

static struct reginfo ri[2];
static uint8_t other_memblock[MEMBLOCKLEN];
static trace_header_t header;

/* Memblock pointer into the execution image. */
static void *memblock;

static int comm_fd;
static bool trace;
static size_t signal_count;
static uintptr_t signal_pc;
static bool is_setup = false;

#ifdef HAVE_ZLIB
#include <zlib.h>
static gzFile gz_trace_file;
#define TRACE_TYPE "compressed"
#else
#define TRACE_TYPE "uncompressed"
#endif

static sigjmp_buf jmpbuf;

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

/* I/O functions */

static RisuResult read_buffer(void *ptr, size_t bytes)
{
    size_t res;

    if (!trace) {
        return recv_data_pkt(comm_fd, ptr, bytes);
    }

#ifdef HAVE_ZLIB
    if (comm_fd == STDIN_FILENO) {
#endif
        res = read(comm_fd, ptr, bytes);
#ifdef HAVE_ZLIB
    } else {
        res = gzread(gz_trace_file, ptr, bytes);
    }
#endif

    return res == bytes ? RES_OK : RES_BAD_IO;
}

static RisuResult write_buffer(void *ptr, size_t bytes)
{
    size_t res;

    if (!trace) {
        return send_data_pkt(comm_fd, ptr, bytes);
    }

#ifdef HAVE_ZLIB
    if (comm_fd == STDOUT_FILENO) {
#endif
        res = write(comm_fd, ptr, bytes);
#ifdef HAVE_ZLIB
    } else {
        res = gzwrite(gz_trace_file, ptr, bytes);
    }
#endif

    return res == bytes ? RES_OK : RES_BAD_IO;
}

static void respond(RisuResult r)
{
    if (!trace) {
        send_response_byte(comm_fd, r);
    }
}

static RisuResult send_register_info(void *uc, void *siaddr)
{
    uint64_t paramreg;
    RisuResult res;
    RisuOp op;
    void *extra;

    reginfo_init(&ri[MASTER], uc, siaddr);
    op = get_risuop(&ri[MASTER]);
    if (is_setup && op == OP_SIGILL) {
        return RES_OK;
    }

    /* Write a header with PC/op to keep in sync */
    header.magic = RISU_MAGIC;
    header.pc = get_pc(&ri[MASTER]);
    header.risu_op = op;

    switch (op) {
    case OP_TESTEND:
    case OP_COMPARE:
    case OP_SIGILL:
        header.size = reginfo_size(&ri[MASTER]);
        extra = &ri[MASTER];
        break;
    case OP_COMPAREMEM:
        header.size = MEMBLOCKLEN;
        extra = memblock;
        break;
    case OP_SETMEMBLOCK:
    case OP_GETMEMBLOCK:
    case OP_SETUPBEGIN:
    case OP_SETUPEND:
        header.size = 0;
        extra = NULL;
        break;
    default:
        abort();
    }

    res = write_buffer(&header, sizeof(header));
    if (res != RES_OK) {
        return res;
    }
    if (extra) {
        res = write_buffer(extra, header.size);
        if (res != RES_OK) {
            return res;
        }
    }

    switch (op) {
    case OP_COMPARE:
    case OP_SIGILL:
    case OP_COMPAREMEM:
        break;
    case OP_TESTEND:
        return RES_END;
    case OP_SETMEMBLOCK:
        paramreg = get_reginfo_paramreg(&ri[MASTER]);
        memblock = (void *)(uintptr_t)paramreg;
        break;
    case OP_GETMEMBLOCK:
        paramreg = get_reginfo_paramreg(&ri[MASTER]);
        set_ucontext_paramreg(uc, paramreg + (uintptr_t)memblock);
        break;
    case OP_SETUPBEGIN:
    case OP_SETUPEND:
        is_setup = op == OP_SETUPBEGIN;
        break;
    default:
        abort();
    }
    return RES_OK;
}

static void master_sigill(int sig, siginfo_t *si, void *uc)
{
    RisuResult r;
    signal_count++;

    if (sig == SIGBUS) {
        r = RES_SIGBUS;
    }
    else {
        r = send_register_info(uc, si->si_addr);
    }
    if (r == RES_OK) {
        advance_pc(uc);
    } else {
        signal_pc = get_uc_pc(uc, si->si_addr);
        siglongjmp(jmpbuf, r);
    }
}

static RisuResult recv_register_info(struct reginfo *ri)
{
    RisuResult res;

    res = read_buffer(&header, sizeof(header));
    if (res != RES_OK) {
        return res;
    }

    if (header.magic != RISU_MAGIC) {
        /* If the magic number is wrong, we can't trust the rest. */
        return RES_BAD_MAGIC;
    }

    switch (header.risu_op) {
    case OP_COMPARE:
    case OP_TESTEND:
    case OP_SIGILL:
        /* If we can't store the data, report invalid size. */
        if (header.size > sizeof(*ri)) {
            return RES_BAD_SIZE_HEADER;
        }
        respond(RES_OK);
        res = read_buffer(ri, header.size);
        if (res == RES_OK && header.size != reginfo_size(ri)) {
            /* The payload size is not self-consistent with the data. */
            return RES_BAD_SIZE_REGINFO;
        }
        return res;

    case OP_COMPAREMEM:
        if (header.size != MEMBLOCKLEN) {
            return RES_BAD_SIZE_MEMBLOCK;
        }
        respond(RES_OK);
        return read_buffer(other_memblock, MEMBLOCKLEN);

    case OP_SETMEMBLOCK:
    case OP_GETMEMBLOCK:
    case OP_SETUPBEGIN:
    case OP_SETUPEND:
        return header.size == 0 ? RES_OK : RES_BAD_SIZE_ZERO;

    default:
        return RES_BAD_OP;
    }
}

static RisuResult recv_and_compare_register_info(void *uc, void *siaddr)
{
    uint64_t paramreg;
    RisuResult res;
    RisuOp op;

    reginfo_init(&ri[APPRENTICE], uc, siaddr);
    op = get_risuop(&ri[APPRENTICE]);
    if (is_setup && op == OP_SIGILL) {
        return RES_OK;
    }

    res = recv_register_info(&ri[MASTER]);
    if (res != RES_OK) {
        goto done;
    }

    switch (op) {
    case OP_COMPARE:
    case OP_TESTEND:
    case OP_SIGILL:
        /*
         * If we have nothing to compare against, report an op mismatch.
         * Otherwise allow the compare to continue, and assume that
         * something in the reginfo will be different.
         */
        if (header.risu_op != OP_COMPARE &&
            header.risu_op != OP_TESTEND &&
            header.risu_op != OP_SIGILL) {
            res = RES_MISMATCH_OP;
        } else if (!is_setup && !reginfo_is_eq(&ri[MASTER], &ri[APPRENTICE])) {
            /* register mismatch */
            res = RES_MISMATCH_REG;
        } else if (op != header.risu_op) {
            /* The reginfo matched.  We should have matched op. */
            res = RES_MISMATCH_OP;
        } else if (op == OP_TESTEND) {
            res = RES_END;
        } else {
            reginfo_update(&ri[MASTER], uc, siaddr);
        }
        break;

    case OP_SETMEMBLOCK:
        if (op != header.risu_op) {
            res = RES_MISMATCH_OP;
            break;
        }
        paramreg = get_reginfo_paramreg(&ri[APPRENTICE]);
        memblock = (void *)(uintptr_t)paramreg;
        break;

    case OP_GETMEMBLOCK:
        if (op != header.risu_op) {
            res = RES_MISMATCH_OP;
            break;
        }
        paramreg = get_reginfo_paramreg(&ri[APPRENTICE]);
        set_ucontext_paramreg(uc, paramreg + (uintptr_t)memblock);
        break;

    case OP_COMPAREMEM:
        if (op != header.risu_op) {
            res = RES_MISMATCH_OP;
            break;
        }
        if (memcmp(memblock, other_memblock, MEMBLOCKLEN) != 0) {
            /* memory mismatch */
            res = RES_MISMATCH_MEM;
        }
        break;

    case OP_SETUPBEGIN:
    case OP_SETUPEND:
        if (op != header.risu_op) {
            res = RES_MISMATCH_OP;
            break;
        }
        is_setup = op == OP_SETUPBEGIN;
        break;

    default:
        abort();
    }

 done:
    /* On error, tell master to exit. */
    respond(res == RES_OK ? RES_OK : RES_END);
    return res;
}

static void apprentice_sigill(int sig, siginfo_t *si, void *uc)
{
    RisuResult r;
    signal_count++;

    if (sig == SIGBUS) {
        r = RES_SIGBUS;
    }
    else {
        r = recv_and_compare_register_info(uc, si->si_addr);
    }
    if (r == RES_OK) {
        advance_pc(uc);
    } else {
        signal_pc = get_uc_pc(uc, si->si_addr);
        siglongjmp(jmpbuf, r);
    }
}

static void set_sigill_handler(void (*fn) (int, siginfo_t *, void *))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(struct sigaction));

    sa.sa_sigaction = fn;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGILL, &sa, 0) != 0) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
    if (sigaction(SIGBUS, &sa, 0) != 0) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
}

uintptr_t image_start_address;
entrypoint_fn *image_start;
size_t image_size;

static void load_image(const char *imgfile)
{
    /* Load image file into memory as executable */
    struct stat st;
    fprintf(stderr, "loading test image %s...\n", imgfile);
    int fd = open(imgfile, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "failed to open image file %s\n", imgfile);
        exit(EXIT_FAILURE);
    }
    if (fstat(fd, &st) != 0) {
        perror("fstat");
        exit(EXIT_FAILURE);
    }
    image_size = st.st_size;
    void *addr;

    /* Map writable because we include the memory area for store
     * testing in the image.
     */
    addr =
        mmap(0, image_size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE, fd,
             0);
    if (!addr) {
        perror("mmap");
        exit(EXIT_FAILURE);
    }
    close(fd);
    image_start = addr;
    image_start_address = (uintptr_t) addr;
}

static void close_comm()
{
#ifdef HAVE_ZLIB
    if (trace && comm_fd != STDOUT_FILENO) {
        gzclose(gz_trace_file);
    }
#endif
    close(comm_fd);
}

static int master(void)
{
    int result;
    RisuResult res = sigsetjmp(jmpbuf, 1);

    switch (res) {
    case RES_OK:
        set_sigill_handler(&master_sigill);
        fprintf(stderr, "starting image at 0x%"PRIxPTR"\n",
                image_start_address);
        do_image();
        fprintf(stderr, "image returned unexpectedly\n");
        result = EXIT_FAILURE;
        break;

    case RES_END:
        fprintf(stderr, "done after %zd checkpoints\n", signal_count);
        close_comm();
        result = EXIT_SUCCESS;
        break;

    case RES_BAD_IO:
        fprintf(stderr, "i/o error at image + 0x%"PRIxPTR" after %zd checkpoints\n", signal_pc - image_start_address, signal_count);
        result = EXIT_FAILURE;
        break;

    case RES_SIGBUS:
        fprintf(stderr, "bus error at image + 0x%"PRIxPTR" after %zd checkpoints\n", signal_pc - image_start_address, signal_count);
        close_comm();
        result = EXIT_FAILURE;
        break;

    default:
        fprintf(stderr, "unexpected result %d at image + 0x%"PRIxPTR" after %zd checkpoints\n", res, signal_pc - image_start_address, signal_count);
        close_comm();
        result = EXIT_FAILURE;
        break;
    }
    return result;
}

static const char *op_name(RisuOp op)
{
    switch (op) {
    case OP_SIGILL:
        return "SIGILL";
    case OP_COMPARE:
        return "COMPARE";
    case OP_TESTEND:
        return "TESTEND";
    case OP_SETMEMBLOCK:
        return "SETMEMBLOCK";
    case OP_GETMEMBLOCK:
        return "GETMEMBLOCK";
    case OP_COMPAREMEM:
        return "COMPAREMEM";
    case OP_SETUPBEGIN:
        return "SETUPBEGIN";
    case OP_SETUPEND:
        return "SETUPEND";
    }
    abort();
}

static int apprentice(void)
{
    int result;
    RisuResult res = sigsetjmp(jmpbuf, 1);

    switch (res) {
    case RES_OK:
        set_sigill_handler(&apprentice_sigill);
        fprintf(stderr, "starting image at 0x%"PRIxPTR"\n",
                image_start_address);
        do_image();
        fprintf(stderr, "image returned unexpectedly\n");
        result = EXIT_FAILURE;
        break;

    case RES_END:
        fprintf(stderr, "done after %zd checkpoints\n", signal_count);
        result = EXIT_SUCCESS;
        break;

    case RES_MISMATCH_REG:
        fprintf(stderr, "Mismatch reg at image + 0x%"PRIxPTR" after %zd checkpoints\n", signal_pc - image_start_address, signal_count);
        fprintf(stderr, "master reginfo:\n");
        reginfo_dump(&ri[MASTER], stderr);
        fprintf(stderr, "apprentice reginfo:\n");
        reginfo_dump(&ri[APPRENTICE], stderr);
        reginfo_dump_mismatch(&ri[MASTER], &ri[APPRENTICE], stderr);
        result = EXIT_FAILURE;
        break;

    case RES_MISMATCH_MEM:
        fprintf(stderr, "Mismatch mem at image + 0x%"PRIxPTR" after %zd checkpoints\n", signal_pc - image_start_address, signal_count);
        result = EXIT_FAILURE;
        break;

    case RES_MISMATCH_OP:
        /* Out of sync, but both opcodes are known valid. */
        fprintf(stderr, "Mismatch header at image + 0x%"PRIxPTR" after %zd checkpoints\n"
                "mismatch detail (master : apprentice):\n"
                "  opcode: %s vs %s\n",
                signal_pc - image_start_address,
                signal_count, op_name(header.risu_op),
                op_name(get_risuop(&ri[APPRENTICE])));
        result = EXIT_FAILURE;
        break;

    case RES_BAD_IO:
        fprintf(stderr, "i/o error at image + 0x%"PRIxPTR" after %zd checkpoints\n", signal_pc - image_start_address, signal_count);
        result = EXIT_FAILURE;
        break;

    case RES_BAD_MAGIC:
        fprintf(stderr, "Unexpected magic number: %#08x at image + 0x%"PRIxPTR" after %zd checkpoints\n", header.magic, signal_pc - image_start_address, signal_count);
        result = EXIT_FAILURE;
        break;

    case RES_BAD_SIZE_HEADER:
        fprintf(stderr, "Payload size %u in header exceeds expected size %d at image + 0x%"PRIxPTR" after %zd checkpoints\n", header.size, sizeof(struct reginfo), signal_pc - image_start_address, signal_count);
        result = EXIT_FAILURE;
        break;

    case RES_BAD_SIZE_REGINFO:
        fprintf(stderr, "Payload size %u in header doesn't match received payload size %d at image + 0x%"PRIxPTR" after %zd checkpoints\n", header.size, reginfo_size(&ri[MASTER]), signal_pc - image_start_address, signal_count);
        result = EXIT_FAILURE;
        break;

    case RES_BAD_SIZE_MEMBLOCK:
        fprintf(stderr, "Payload size %u in header doesn't match the expected memory block payload size %d at image + 0x%"PRIxPTR" after %zd checkpoints\n", header.size, MEMBLOCKLEN, signal_pc - image_start_address, signal_count);
        result = EXIT_FAILURE;
        break;

    case RES_BAD_SIZE_ZERO:
        fprintf(stderr, "Payload size %u in header is expected to be 0 at image + 0x%"PRIxPTR" after %zd checkpoints\n", header.size, signal_pc - image_start_address, signal_count);
        result = EXIT_FAILURE;
        break;

    case RES_BAD_OP:
        fprintf(stderr, "Unexpected opcode: %d at image + 0x%"PRIxPTR" after %zd checkpoints\n", header.risu_op, signal_pc - image_start_address, signal_count);
        result = EXIT_FAILURE;
        break;

    case RES_SIGBUS:
        fprintf(stderr, "bus error at image + 0x%"PRIxPTR" after %zd checkpoints\n", signal_pc - image_start_address, signal_count);
        result = EXIT_FAILURE;
        break;

    default:
        fprintf(stderr, "Unexpected result %d at image + 0x%"PRIxPTR" after %zd checkpoints\n", res, signal_pc - image_start_address, signal_count);
        result = EXIT_FAILURE;
        break;
    }
    return result;
}

static int ismaster;

static void usage(void)
{
    fprintf(stderr,
            "Usage: risu [--master] [--host <ip>] [--port <port>] <image file>"
            "\n\n");
    fprintf(stderr,
            "Run through the pattern file verifying each instruction\n");
    fprintf(stderr, "between master and apprentice risu processes.\n\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --master          Be the master (server)\n");
    fprintf(stderr, "  -t, --trace=FILE  Record/playback " TRACE_TYPE " trace file\n");
    fprintf(stderr,
            "  -h, --host=HOST   Specify master host machine (apprentice only)"
            "\n");
    fprintf(stderr,
            "  -p, --port=PORT   Specify the port to connect to/listen on "
            "(default 9191)\n");
    if (arch_extra_help) {
        fprintf(stderr, "%s", arch_extra_help);
    }
}

static struct option * setup_options(char **short_opts)
{
    static struct option default_longopts[] = {
        {"help", no_argument, 0, '?'},
        {"master", no_argument, &ismaster, 1},
        {"host", required_argument, 0, 'h'},
        {"port", required_argument, 0, 'p'},
        {"trace", required_argument, 0, 't'},
        {0, 0, 0, 0}
    };
    struct option *lopts = &default_longopts[0];

    *short_opts = "h:p:t:";

    if (arch_long_opts) {
        const size_t osize = sizeof(struct option);
        const int default_count = ARRAY_SIZE(default_longopts) - 1;
        int arch_count;

        /* count additional opts */
        for (arch_count = 0; arch_long_opts[arch_count].name; arch_count++) {
            continue;
        }

        lopts = calloc(default_count + arch_count + 1, osize);

        /* Copy default opts + extra opts */
        memcpy(lopts, default_longopts, default_count * osize);
        memcpy(lopts + default_count, arch_long_opts, arch_count * osize);
    }

    return lopts;
}

#if __APPLE__
extern size_t mcontext_min_size;
extern size_t mcontext_max_size;
#endif

int main(int argc, char **argv)
{
    /* some handy defaults to make testing easier */
    uint16_t port = 9191;
    char *hostname = "localhost";
    char *imgfile;
    char *trace_fn = NULL;
    struct option *longopts;
    char *shortopts;
    stack_t ss;

    longopts = setup_options(&shortopts);

    for (;;) {
        int optidx = 0;
        int c = getopt_long(argc, argv, shortopts, longopts, &optidx);
        if (c == -1) {
            break;
        }

        switch (c) {
        case 0:
            /* flag set by getopt_long, do nothing */
            break;
        case 't':
            trace_fn = optarg;
            trace = true;
            break;
        case 'h':
            hostname = optarg;
            break;
        case 'p':
            /* FIXME err handling */
            port = strtol(optarg, 0, 10);
            break;
        case '?':
            usage();
            return EXIT_FAILURE;
        default:
            assert(c >= FIRST_ARCH_OPT);
            process_arch_opt(c, optarg);
            break;
        }
    }

    if (trace) {
        if (strcmp(trace_fn, "-") == 0) {
            comm_fd = ismaster ? STDOUT_FILENO : STDIN_FILENO;
        } else {
            if (ismaster) {
                comm_fd = open(trace_fn, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            } else {
                comm_fd = open(trace_fn, O_RDONLY);
            }
            if (comm_fd < 0) {
                fprintf(stderr, "trace file \"%s\" cannot be opened\n", trace_fn);
                perror("open");
                exit(EXIT_FAILURE);
            }
#ifdef HAVE_ZLIB
            gz_trace_file = gzdopen(comm_fd, ismaster ? "wb9" : "rb");
#endif
        }
    } else {
        if (ismaster) {
            fprintf(stderr, "master port %d\n", port);
            comm_fd = master_connect(port);
        } else {
            fprintf(stderr, "apprentice host %s port %d\n", hostname, port);
            comm_fd = apprentice_connect(hostname, port);
        }
    }

    imgfile = argv[optind];
    if (!imgfile) {
        fprintf(stderr, "Error: must specify image file name\n\n");
        usage();
        return EXIT_FAILURE;
    }

    load_image(imgfile);

    /* create alternate stack */
    ss.ss_sp = malloc(SIGSTKSZ);
    if (ss.ss_sp == NULL) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    ss.ss_size = SIGSTKSZ;
    ss.ss_flags = 0;
    if (sigaltstack(&ss, NULL) == -1) {
        perror("sigaltstac");
        exit(EXIT_FAILURE);
    }

    /* E.g. select requested SVE vector length. */
    arch_init();

    int result;
    if (ismaster) {
        fprintf(stderr, "starting master\n");
        result = master();
    } else {
        fprintf(stderr, "starting apprentice\n");
        result = apprentice();
    }

#if __APPLE__
    fprintf(stderr, "size:%d min:%d max:%d thread:%d float:%d exception:%d vector:%d\n",
        (int)sizeof(struct mcontext), /* ppc: 1032 */
        (int)mcontext_min_size, /* ppc: 456 before vrsave is set to -1 */
        (int)mcontext_max_size, /* ppc: 1032 after vrsave is set to -1 */
        (int)sizeof(ppc_thread_state_t), /* ppc: 160 */
        (int)sizeof(ppc_float_state_t), /* ppc: 264 */
        (int)sizeof(ppc_exception_state_t), /* ppc: 32 */
        (int)sizeof(ppc_vector_state_t) /* ppc: 576 */
    );
#endif

    return result;
}
