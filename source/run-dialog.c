/**
 * rofi
 *
 * MIT/X11 License
 * Copyright 2013-2014 Qball  Cow <qball@gmpclient.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <X11/X.h>

#include <unistd.h>
#include <limits.h>
#include <signal.h>
#include <sys/types.h>
#include <dirent.h>
#include <strings.h>
#include <string.h>
#include <errno.h>

#include "rofi.h"
#include "history.h"
#include "run-dialog.h"
#ifdef TIMING
#include <time.h>
#endif

#define RUN_CACHE_FILE    "rofi-2.runcache"

static inline int execsh ( const char *cmd, int run_in_term )
{
// use sh for args parsing
    if ( run_in_term ) {
        return execlp ( config.terminal_emulator, config.terminal_emulator, "-e", "sh", "-c", cmd, NULL );
    }

    return execlp ( "/bin/sh", "sh", "-c", cmd, NULL );
}

// execute sub-process
static pid_t exec_cmd ( const char *cmd, int run_in_term )
{
    if ( !cmd || !cmd[0] ) {
        return -1;
    }

    signal ( SIGCHLD, catch_exit );
    pid_t pid = fork ();

    if ( !pid ) {
        setsid ();
        execsh ( cmd, run_in_term );
        exit ( EXIT_FAILURE );
    }

    /**
     * This happens in non-critical time (After launching app)
     * It is allowed to be a bit slower.
     */
    char *path = NULL;
    if ( asprintf ( &path, "%s/%s", cache_dir, RUN_CACHE_FILE ) == -1 ) {
        return -1;
    }
    history_set ( path, cmd );

    free ( path );

    return pid;
}
// execute sub-process
static void delete_entry ( const char *cmd )
{
    /**
     * This happens in non-critical time (After launching app)
     * It is allowed to be a bit slower.
     */
    char *path = NULL;
    if ( asprintf ( &path, "%s/%s", cache_dir, RUN_CACHE_FILE ) == -1 ) {
        return;
    }
    history_remove ( path, cmd );

    free ( path );
}
static int sort_func ( const void *a, const void *b )
{
    const char *astr = *( const char * const * ) a;
    const char *bstr = *( const char * const * ) b;
    return strcasecmp ( astr, bstr );
}
static char ** get_apps ( void )
{
    unsigned int    num_favorites = 0;
    unsigned int    index         = 0;
    char            *path;
    char            **retv = NULL;
#ifdef TIMING
    struct timespec start, stop;
    clock_gettime ( CLOCK_REALTIME, &start );
#endif

    if ( getenv ( "PATH" ) == NULL ) {
        return NULL;
    }


    if ( asprintf ( &path, "%s/%s", cache_dir, RUN_CACHE_FILE ) > 0 ) {
        retv = history_get_list ( path, &index );
        free ( path );
        // Keep track of how many where loaded as favorite.
        num_favorites = index;
    }


    path = strdup ( getenv ( "PATH" ) );

    for ( const char *dirname = strtok ( path, ":" );
          dirname != NULL;
          dirname = strtok ( NULL, ":" ) ) {
        DIR *dir = opendir ( dirname );

        if ( dir != NULL ) {
            struct dirent *dent;

            while ( ( dent = readdir ( dir ) ) != NULL ) {
                if ( dent->d_type != DT_REG &&
                     dent->d_type != DT_LNK &&
                     dent->d_type != DT_UNKNOWN ) {
                    continue;
                }

                int found = 0;

                // This is a nice little penalty, but doable? time will tell.
                // given num_favorites is max 25.
                for ( unsigned int j = 0; found == 0 && j < num_favorites; j++ ) {
                    if ( strcasecmp ( dent->d_name, retv[j] ) == 0 ) {
                        found = 1;
                    }
                }

                if ( found == 1 ) {
                    continue;
                }

                char ** tr = realloc ( retv, ( index + 2 ) * sizeof ( char* ) );
                if ( tr != NULL ) {
                    retv            = tr;
                    retv[index]     = strdup ( dent->d_name );
                    retv[index + 1] = NULL;
                    index++;
                }
            }

            closedir ( dir );
        }
    }

    // TODO: check this is still fast enough. (takes 1ms on laptop.)
    if ( index > num_favorites ) {
        qsort ( &retv[num_favorites], index - num_favorites, sizeof ( char* ), sort_func );
    }
    free ( path );
#ifdef TIMING
    clock_gettime ( CLOCK_REALTIME, &stop );

    if ( stop.tv_sec != start.tv_sec ) {
        stop.tv_nsec += ( stop.tv_sec - start.tv_sec ) * 1e9;
    }

    long diff = stop.tv_nsec - start.tv_nsec;
    printf ( "Time elapsed: %ld us\n", diff / 1000 );
#endif
    return retv;
}

SwitcherMode run_switcher_dialog ( char **input )
{
    int          shift         = 0;
    int          selected_line = 0;
    SwitcherMode retv          = MODE_EXIT;
    // act as a launcher
    char         **cmd_list = get_apps ( );

    if ( cmd_list == NULL ) {
        cmd_list    = malloc ( 2 * sizeof ( char * ) );
        cmd_list[0] = strdup ( "No applications found" );
        cmd_list[1] = NULL;
    }

    int mretv = menu ( cmd_list, input, "run:", NULL, &shift, token_match, NULL, &selected_line );

    if ( mretv == MENU_NEXT ) {
        retv = NEXT_DIALOG;
    }
    else if ( mretv == MENU_OK && cmd_list[selected_line] != NULL ) {
        exec_cmd ( cmd_list[selected_line], shift );
    }
    else if ( mretv == MENU_CUSTOM_INPUT && *input != NULL && *input[0] != '\0' ) {
        exec_cmd ( *input, shift );
    }
    else if ( mretv == MENU_ENTRY_DELETE && cmd_list[selected_line] ) {
        delete_entry ( cmd_list[selected_line] );
        retv = RUN_DIALOG;
    }

    for ( int i = 0; cmd_list != NULL && cmd_list[i] != NULL; i++ ) {
        free ( cmd_list[i] );
    }

    if ( cmd_list != NULL ) {
        free ( cmd_list );
    }

    return retv;
}
