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

#include "risu.h"
#include <stdlib.h>
#include <string.h>
#ifdef RISU_MACOS9
#include <SIOUX.h>
#include <fcntl.h>
#include <errno.h>
#include <Memory.h>
#endif

char * argcargv(char ***dargv, int *dargc, char *buf)
{
    if (dargv) *dargv = NULL;
    if (dargc) *dargc = 0;
    if (!buf) {
        return 0;
    }
    int argc;
    char** argv;
    char *s;
    char *d;
    for (int p = 0; p <= (dargv != NULL); p++) {
        if (p) {
            argv = (char**)calloc(argc, sizeof(char*));
            if (dargv) *dargv = argv;
        }
        argc = 0;
        s = buf;
        d = buf;
        while (*s) {
            while ((*s == 9) || (*s == ' ')) {
                s++;
            }
            if ((*s == 10) || (*s == 13)) {
                s++;
                break;
            }
            if (*s) {
                if (p) argv[argc] = d;
                argc++;
                while (*s) {
                    if (*s == '"') {
                        s++;
                        while (*s && *s != '"') {
                            if (p) *d++ = *s;
                            s++;
                        }
                        if (*s == '"')
                            s++;
                        else
                            printf("Missing terminating \" quote\n");
                    }
                    else if (*s == '\'') {
                        s++;
                        while (*s && *s != '\'') {
                            if (p) *d++ = *s;
                            s++;
                        }
                        if (*s == '\'')
                            s++;
                        else
                            printf("Missing terminating ' quote\n");
                    }
                    else if (*s == '\\') {
                        s++;
                        if (*s) {
                            switch (*s) {
                                case '\\': if (p) *d++ = '\\'; break;
                                case 't': if (p) *d++ =  9; break;
                                case 'n': if (p) *d++ = 10; break;
                                case 'r': if (p) *d++ = 13; break;
                                default : if (p) *d++ = *s; break;
                            }
                            s++;
                        } else {
                            printf("Missing \\ quoted character\n");
                        }
                    }
                    else if ((*s == 9) || (*s == ' ')) {
                        s++;
                        break;
                    }
                    else if ((*s == 10) || (*s == 13)) {
                        s++;
                        if (p) {
                            *d++ = 0;
                        }
                        goto donewords;
                    }
                    else {
                        if (p) *d++ = *s;
                        s++;
                    }
                } // while word
                if (p) {
                    *d++ = 0;
                }
            } // if word
        } // while words

donewords:
        ;

    } // for pass

    if (dargc) *dargc = argc;
    return s;
}

#ifdef RISU_MACOS9
char gbuf[30000];

void eval(char *buf)
{
    int argc = 0;
    char **argv = NULL;
    while (buf && *buf) {
        if (argv) {
            free(argv);
            argv = 0;
        }
        buf = argcargv(&argv, &argc, buf);
        if (argc > 0) {
            #if 1
            printf("\n%d args: ", argc);
            for (int i = 0; i < argc; i++) {
                printf("%s%s", argv[i], i < argc - 1 ? " " : "");
            }
            printf("\n");
            #endif
            if (!strcmp(argv[0], "exit")) {
                printf("[Process completed]\n");
                break;
            }
            if (!strcmp(argv[0], "source")) {
                if (argc != 2) {
                    printf("Expected filename.");
                } else {
                    struct stat st;
                    int fd = open(argv[1], O_RDONLY);
                    if (fd < 0) {
                        fprintf(stderr, "failed to open file %s\n", argv[1]);
                        continue;
                    }
                    if (fstat(fd, &st) != 0) {
                        perror("fstat");
                        close(fd);
                        continue;
                    }
                    char *addr = (char *)malloc(st.st_size + 1);
                    if (!addr) {
                        if (!errno)
                            errno = MemError();
                        perror("malloc");
                        close(fd);
                        continue;
                    }
                    read(fd, addr, st.st_size);
                    close(fd);
                    addr[st.st_size] = '\0';
                    eval(addr);
                    free(addr);
                }
            }
            if (!strcmp(argv[0], "risu")) {
                risu_main(argc, argv);
                #if 0
                    optreset = 1; // for unix
                #else
                    optind = 0; // for compat/getopt.h
                #endif
            }
        }
    }
    if (argv) {
        free(argv);
        argv = 0;
    }
}
#endif

int main(int argc, char **argv)
{
#ifdef RISU_MACOS9
    #pragma unused(argc, argv)
    SIOUXSettings.showstatusline = true;

    while (1) {
        printf("# ");
        fgets(gbuf, sizeof(gbuf), stdin);
        eval(gbuf);
    }
#else
    return risu_main(argc, argv);
#endif
}
