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
#include <setjmp.h>
#include <assert.h>
#ifndef RISU_MACOS9
#include <sys/stat.h>
#include <sys/mman.h>
#endif
#include <fcntl.h>
#include <string.h>

#include "risu.h"
#include "endianswap.h"

#ifdef NO_SIGNAL
    sig_handler_fn *sig_handler;
#else
    #include <signal.h>
    #include <ucontext.h>
#endif

enum {
    MASTER = 0, APPRENTICE = 1
};

static struct reginfo ri[2];
static uint8_t other_memblock[MEMBLOCKLEN];
static trace_header_t header;

/* Memblock pointer into the execution image. */
static void *memblock;
arch_ptr_t arch_memblock;

static int comm_fd;
static bool trace;
size_t signal_count;
size_t illegal_instructions;
static arch_ptr_t signal_pc;
static bool is_setup;

#ifdef HAVE_ZLIB
#include <zlib.h>
static gzFile gz_trace_file;
#define TRACE_TYPE "compressed"
#else
#define TRACE_TYPE "uncompressed"
#endif

#ifdef RISU_MACOS9
    #include <setjmp.h>
    #include <signal.h>
    #include <MachineExceptions.h>
    #include <Memory.h>
    #include <OSUtils.h>
    static jmp_buf jmpbuf;
    ExceptionHandler prevHandler;
    OSStatus classic_exception_handler(ExceptionInformation *theException);
#else
    static sigjmp_buf jmpbuf;
#endif

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

/* Endian functions */

static void byte_swap_header(trace_header_t *header) {
    header->magic = (uint32_t)BYTESWAP_32(header->magic);
    header->size = (uint32_t)BYTESWAP_32(header->size);
    header->risu_op = (int32_t)BYTESWAP_32(header->risu_op);
    if (sizeof(header->pc) == 8)
        header->pc = (arch_ptr_t)BYTESWAP_64((uint64_t)header->pc);
    else
        header->pc = (arch_ptr_t)BYTESWAP_32(header->pc);
}

static void header_host_to_arch(trace_header_t *header) {
    if (
    #ifdef __LITTLE_ENDIAN__
        !
    #endif
        !get_arch_big_endian()
   ) {
        if (header->magic != BYTESWAP_32(RISU_MAGIC)) {
            byte_swap_header(header);
        }
    }
}

static void header_arch_to_host(trace_header_t *header) {
    if (header->magic == BYTESWAP_32(RISU_MAGIC)) {
        byte_swap_header(header);
    }
}

/* I/O functions */

static RisuResult read_buffer(void *ptr, size_t bytes)
{
    size_t res;

#ifndef RISU_MACOS9
    if (!trace) {
        return recv_data_pkt(comm_fd, ptr, (int)bytes);
    }
#endif

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

#ifndef RISU_MACOS9
    if (!trace) {
        return send_data_pkt(comm_fd, ptr, (int)bytes);
    }
#endif

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
    arch_ptr_t paramreg;
    RisuResult res;
    RisuOp op;
    void *extra;

    reginfo_init(&ri[MASTER], uc, siaddr);
    op = get_risuop(&ri[MASTER]);
    if (op == OP_SIGILL) {
        illegal_instructions++;
        if (is_setup)
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
        reginfo_host_to_arch(&ri[MASTER]);
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

    header_host_to_arch(&header);
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
        arch_memblock = get_reginfo_paramreg(&ri[MASTER]);
        memblock = get_arch_memory(arch_memblock);
        break;
    case OP_GETMEMBLOCK:
        paramreg = get_reginfo_paramreg(&ri[MASTER]);
        set_ucontext_paramreg(uc, paramreg + arch_memblock);
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

static void master_sigill(int sig, arch_siginfo_t *si, void *uc)
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
#ifdef RISU_MACOS9
        longjmp(jmpbuf, r);
#else
        siglongjmp(jmpbuf, r);
#endif
    }
}

static RisuResult recv_register_info(struct reginfo *ri)
{
    RisuResult res;

    res = read_buffer(&header, sizeof(header));
    if (res != RES_OK) {
        return res;
    }
    header_arch_to_host(&header);

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
        reginfo_arch_to_host(ri);
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
    arch_ptr_t paramreg;
    RisuResult res;
    RisuOp op;

    reginfo_init(&ri[APPRENTICE], uc, siaddr);
    op = get_risuop(&ri[APPRENTICE]);
    if (op == OP_SIGILL) {
        illegal_instructions++;
        if (is_setup)
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
        } else if (op != OP_SIGILL) {
            reginfo_update(&ri[MASTER], uc, siaddr);
        }
        break;

    case OP_SETMEMBLOCK:
        if (op != header.risu_op) {
            res = RES_MISMATCH_OP;
            break;
        }
        arch_memblock = get_reginfo_paramreg(&ri[APPRENTICE]);
        memblock = get_arch_memory(arch_memblock);
        break;

    case OP_GETMEMBLOCK:
        if (op != header.risu_op) {
            res = RES_MISMATCH_OP;
            break;
        }
        paramreg = get_reginfo_paramreg(&ri[APPRENTICE]);
        set_ucontext_paramreg(uc, paramreg + arch_memblock);
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

static void apprentice_sigill(int sig, arch_siginfo_t *si, void *uc)
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
#ifdef RISU_MACOS9
        longjmp(jmpbuf, r);
#else
        siglongjmp(jmpbuf, r);
#endif
    }
}

static void set_sigill_handler(sig_handler_fn *fn)
{
#ifdef NO_SIGNAL
    sig_handler = fn;
    #ifdef RISU_MACOS9
        prevHandler = InstallExceptionHandler(classic_exception_handler);
    #endif
#else
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
#endif
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

#ifdef RISU_MACOS9
    addr = malloc(image_size);
    if (!addr) {
        if (!errno)
            errno = MemError();
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    read(fd, addr, image_size);
    if (FlushCodeCacheRange)
	    FlushCodeCacheRange(addr, image_size);
    __eieio();
    __sync();
    __isync();
#else
    /* Map writable because we include the memory area for store
     * testing in the image.
     */
    addr =
        mmap(0, image_size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE, fd,
             0);
#endif
    if (!addr) {
        perror("mmap");
        exit(EXIT_FAILURE);
    }
    close(fd);
    image_start = (entrypoint_fn *)addr;
    image_start_address = (uintptr_t) addr;
}

static void unload_image()
{
#ifdef RISU_MACOS9
    free((void*)image_start_address);
#endif
    image_start_address = NULL;
    image_start = NULL;
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

static void print_loc()
{
    fprintf(stderr, " at image + 0x%" PRIxARCHPTR, signal_pc - get_arch_start_address());
}

static void print_stats()
{
    fprintf(stderr, " after %zd checkpoints and %zd illegal instructions\n", signal_count - illegal_instructions, illegal_instructions);
}

static int master(void)
{
    int result;
#ifdef RISU_MACOS9
    RisuResult res = (RisuResult)setjmp(jmpbuf);
#else
    RisuResult res = (RisuResult)sigsetjmp(jmpbuf, 1);
#endif

    switch (res) {
    case RES_OK:
        set_sigill_handler(&master_sigill);
        fprintf(stderr, "starting image at 0x%" PRIxARCHPTR "\n",
                get_arch_start_address());
        do_image();
        fprintf(stderr, "image returned unexpectedly\n");
        result = EXIT_FAILURE;
        break;

    case RES_END:
        fprintf(stderr, "done"); print_stats();
        close_comm();
        result = EXIT_SUCCESS;
        break;

    case RES_BAD_IO:
        fprintf(stderr, "i/o error"); print_loc(); print_stats();
        result = EXIT_FAILURE;
        break;

    case RES_SIGBUS:
        fprintf(stderr, "bus error"); print_loc(); print_stats();
        close_comm();
        result = EXIT_FAILURE;
        break;

    default:
        fprintf(stderr, "unexpected result %d", res); print_loc(); print_stats();
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
    return "";
}

static int apprentice(void)
{
    int result;
#ifdef RISU_MACOS9
    RisuResult res = (RisuResult)setjmp(jmpbuf);
#else
    RisuResult res = (RisuResult)sigsetjmp(jmpbuf, 1);
#endif

    switch (res) {
    case RES_OK:
        set_sigill_handler(&apprentice_sigill);
        fprintf(stderr, "starting image at 0x%" PRIxARCHPTR "\n",
                get_arch_start_address());
        do_image();
        fprintf(stderr, "image returned unexpectedly\n");
        result = EXIT_FAILURE;
        break;

    case RES_END:
        fprintf(stderr, "done"); print_stats();
        result = EXIT_SUCCESS;
        break;

    case RES_MISMATCH_REG:
        fprintf(stderr, "Mismatch reg"); print_loc(); print_stats();
        fprintf(stderr, "master reginfo:\n");
        reginfo_dump(&ri[MASTER], stderr);
        fprintf(stderr, "apprentice reginfo:\n");
        reginfo_dump(&ri[APPRENTICE], stderr);
        reginfo_dump_mismatch(&ri[MASTER], &ri[APPRENTICE], stderr);
        result = EXIT_FAILURE;
        break;

    case RES_MISMATCH_MEM:
        fprintf(stderr, "Mismatch mem"); print_loc(); print_stats();
        result = EXIT_FAILURE;
        break;

    case RES_MISMATCH_OP:
        /* Out of sync, but both opcodes are known valid. */
        fprintf(stderr, "Mismatch header"); print_loc(); print_stats();
        fprintf(stderr, "mismatch detail (master : apprentice):\n"
                        "  opcode: %s vs %s\n",
                op_name((RisuOp)header.risu_op),
                op_name(get_risuop(&ri[APPRENTICE])));
        result = EXIT_FAILURE;
        break;

    case RES_BAD_IO:
        fprintf(stderr, "i/o error"); print_loc(); print_stats();
        result = EXIT_FAILURE;
        break;

    case RES_BAD_MAGIC:
        fprintf(stderr, "Unexpected magic number: %#08x", header.magic); print_loc(); print_stats();
        result = EXIT_FAILURE;
        break;

    case RES_BAD_SIZE_HEADER:
        fprintf(stderr, "Payload size %u in header exceeds expected size %d", header.size, (int)sizeof(struct reginfo)); print_loc(); print_stats();
        result = EXIT_FAILURE;
        break;

    case RES_BAD_SIZE_REGINFO:
        fprintf(stderr, "Payload size %u in header doesn't match received payload size %d", header.size, reginfo_size(&ri[MASTER])); print_loc(); print_stats();
        result = EXIT_FAILURE;
        break;

    case RES_BAD_SIZE_MEMBLOCK:
        fprintf(stderr, "Payload size %u in header doesn't match the expected memory block payload size %d", header.size, MEMBLOCKLEN); print_loc(); print_stats();
        result = EXIT_FAILURE;
        break;

    case RES_BAD_SIZE_ZERO:
        fprintf(stderr, "Payload size %u in header is expected to be 0", header.size); print_loc(); print_stats();
        result = EXIT_FAILURE;
        break;

    case RES_BAD_OP:
        fprintf(stderr, "Unexpected opcode: %d", header.risu_op); print_loc(); print_stats();
        result = EXIT_FAILURE;
        break;

    case RES_SIGBUS:
        fprintf(stderr, "bus error"); print_loc(); print_stats();
        result = EXIT_FAILURE;
        break;

    default:
        fprintf(stderr, "Unexpected result %d" PRIxARCHPTR, res); print_loc(); print_stats();
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

static struct option * setup_options(const char **short_opts)
{
    static struct option default_longopts[] = {
        {"help", no_argument, 0, '?'},
        {"master", no_argument, &ismaster, 1},
        {"host", required_argument, 0, 'h'},
        {"port", required_argument, 0, 'p'},
        {"trace", required_argument, 0, 't'},
        {0, 0, 0, 0}
    };
    struct option *lopts;

    *short_opts = "h:p:t:";

    const size_t osize = sizeof(struct option);
    const int default_count = ARRAY_SIZE(default_longopts) - 1;

    if (arch_long_opts) {
        int arch_count;

        /* count additional opts */
        for (arch_count = 0; arch_long_opts[arch_count].name; arch_count++) {
            continue;
        }

        lopts = (struct option *)calloc(default_count + arch_count + 1, osize);

        /* Copy default opts + extra opts */
        memcpy(lopts, default_longopts, default_count * osize);
        memcpy(lopts + default_count, arch_long_opts, arch_count * osize);
    }
    else {
        lopts = (struct option *)calloc(default_count + 1, osize);

        /* Copy default opts + null opt */
        memcpy(lopts, default_longopts, (default_count + 1) * osize);
    }

    return lopts;
}

int risu_main(int argc, char **argv)
{
    /* some handy defaults to make testing easier */
    uint16_t port = 9191;
    const char *hostname = "localhost";
    char *imgfile;
    char *trace_fn = NULL;
    struct option *longopts;
    const char *shortopts;
    trace = false;
    signal_count = 0;
    illegal_instructions = 0;
    is_setup = false;
    ismaster = 0;

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
            free(longopts);
            return EXIT_FAILURE;
        default:
            assert(c >= FIRST_ARCH_OPT);
            process_arch_opt(c, optarg);
            break;
        }
    }

    if (trace) {
        if (trace_fn && strcmp(trace_fn, "-") == 0) {
#ifdef RISU_MACOS9
            fprintf(stderr, "trace file cannot be %s.\n", ismaster ? "stdout" : "stdin");
            perror("trace");
            exit(EXIT_FAILURE);
#else
            comm_fd = ismaster ? STDOUT_FILENO : STDIN_FILENO;
#endif
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
#ifdef RISU_MACOS9
        fprintf(stderr, "trace file must be specified.\n");
        perror("trace");
        exit(EXIT_FAILURE);
#else
        if (ismaster) {
            fprintf(stderr, "master port %d\n", port);
            comm_fd = master_connect(port);
        } else {
            fprintf(stderr, "apprentice host %s port %d\n", hostname, port);
            comm_fd = apprentice_connect(hostname, port);
        }
#endif
    }

    imgfile = argv[optind];
    if (!imgfile) {
        fprintf(stderr, "Error: must specify image file name\n\n");
        usage();
        free(longopts);
        return EXIT_FAILURE;
    }

    load_image(imgfile);

#ifndef NO_SIGNAL
    stack_t ss;

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
#endif

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

    unload_image();

    free(longopts);
    return result;
}
