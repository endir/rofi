/**
 * simpleswitcher
 *
 * MIT/X11 License
 * Copyright (c) 2012 Sean Pringle <sean.pringle@gmail.com>
 * Modified 2013-2014 Qball  Cow <qball@gmpclient.org>
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <ctype.h>
#include <math.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <err.h>
#include <X11/X.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xmd.h>
#include <X11/Xutil.h>
#include <X11/Xproto.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <X11/Xft/Xft.h>
#include <X11/extensions/Xinerama.h>

#ifdef I3
#include <errno.h>
#include <linux/un.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <i3/ipc.h>
#endif

#include "simpleswitcher.h"
#include "run-dialog.h"

#define INNER_MARGIN 5

#define OPAQUE      0xffffffff
#define OPACITY     "_NET_WM_WINDOW_OPACITY"
#define I3_SOCKET_PATH_PROP "I3_SOCKET_PATH"




#define TERMINAL_DEFAULT "x-terminal-emulator"
char *config_terminal_emulator;





void* allocate( unsigned long bytes )
{
    void *ptr = malloc( bytes );

    if ( !ptr ) {
        fprintf( stderr, "malloc failed!\n" );
        exit( EXIT_FAILURE );
    }

    return ptr;
}
void* allocate_clear( unsigned long bytes )
{
    void *ptr = allocate( bytes );
    memset( ptr, 0, bytes );
    return ptr;
}
void* reallocate( void *ptr, unsigned long bytes )
{
    ptr = realloc( ptr, bytes );

    if ( !ptr ) {
        fprintf( stderr, "realloc failed!\n" );
        exit( EXIT_FAILURE );
    }

    return ptr;
}


static char **tokenize( const char *input )
{
    if ( input == NULL ) return NULL;

    char *saveptr = NULL, *token;
    char **retv  = NULL;
    // First entry is always full (modified) stringtext.
    int num_tokens = 1;

    //First entry is string that is modified.
    retv = allocate ( 2*sizeof( char* ) );
    retv[0] = strdup( input );
    retv[1] = NULL;

    // Iterate over tokens.
    for (
        token = strtok_r( retv[0], " ", &saveptr );
        token != NULL;
        token = strtok_r( NULL, " ", &saveptr ) ) {
        retv = reallocate( retv, sizeof( char* )*( num_tokens+2 ) );
        retv[num_tokens+1] = NULL;
        retv[num_tokens] = token;
        num_tokens++;
    }

    return retv;
}

static inline void tokenize_free( char **ip )
{
    if ( ip == NULL ) return;

    if ( ip[0] )
        free( ip[0] );

    free( ip );
}

#ifdef I3
// Path to I3 socket.
char *i3_socket_path = NULL;
// Focus window on I3 window manager.
static void focus_window_i3( const char *socket_path, int id )
{
    int s, t, len;
    struct sockaddr_un remote;

    if ( strlen( socket_path ) > UNIX_PATH_MAX ) {
        fprintf( stderr, "Socket path is to long. %zd > %d\n", strlen( socket_path ), UNIX_PATH_MAX );
        return;
    }

    if ( ( s = socket( AF_UNIX, SOCK_STREAM, 0 ) ) == -1 ) {
        fprintf( stderr, "Failed to open connection to I3: %s\n", strerror( errno ) );
        return;
    }

    remote.sun_family = AF_UNIX;
    strcpy( remote.sun_path, socket_path );
    len = strlen( remote.sun_path ) + sizeof( remote.sun_family );

    if ( connect( s, ( struct sockaddr * )&remote, len ) == -1 ) {
        fprintf( stderr, "Failed to connect to I3 (%s): %s\n", socket_path,strerror( errno ) );
        close( s );
        return ;
    }


// Formulate command
    {
        i3_ipc_header_t head;
        char command[128];
        snprintf( command, 128, "[id=\"%d\"] focus", id );
        // Prepare header.
        memcpy( head.magic, I3_IPC_MAGIC, 6 );
        head.size = strlen( command );
        head.type = I3_IPC_MESSAGE_TYPE_COMMAND;
        // Send header.
        send( s, &head, sizeof( head ),0 );
        // Send message
        send( s, command, strlen( command ),0 );
    }
    {
        i3_ipc_header_t head;
        char reply[128];
        // Receive header.
        t = recv( s, &head, sizeof( head ),0 );

        if ( t == sizeof( head ) ) {
            t= recv( s, reply, head.size, 0 );
            reply[t] = '\0';
            printf( "%s\n", reply );
        }
    }

    close( s );
}
#endif

void catch_exit( __attribute__( ( unused ) ) int sig )
{
    while ( 0 < waitpid( -1, NULL, WNOHANG ) );
}


// cli arg handling
static int find_arg( const int argc, char * const argv[], const char * const key )
{
    int i;

    for ( i = 0; i < argc && strcasecmp( argv[i], key ); i++ );

    return i < argc ? i: -1;
}
static char* find_arg_str( int argc, char *argv[], char *key, char* def )
{
    int i = find_arg( argc, argv, key );
    return ( i > 0 && i < argc-1 ) ? argv[i+1]: def;
}
static int find_arg_int( int argc, char *argv[], char *key, int def )
{
    int i = find_arg( argc, argv, key );
    return ( i > 0 && i < argc-1 ) ? strtol( argv[i+1], NULL, 10 ): def;
}

unsigned int NumlockMask = 0;
Display *display;
Screen *screen;
Window root;
int screen_id;

static int ( *xerror )( Display *, XErrorEvent * );

#define ATOM_ENUM(x) x
#define ATOM_CHAR(x) #x

#define EWMH_ATOMS(X) \
X(_NET_SUPPORTING_WM_CHECK),\
X(_NET_CLIENT_LIST),\
X(_NET_CLIENT_LIST_STACKING),\
X(_NET_NUMBER_OF_DESKTOPS),\
X(_NET_CURRENT_DESKTOP),\
X(_NET_DESKTOP_GEOMETRY),\
X(_NET_DESKTOP_VIEWPORT),\
X(_NET_WORKAREA),\
X(_NET_ACTIVE_WINDOW),\
X(_NET_CLOSE_WINDOW),\
X(_NET_MOVERESIZE_WINDOW),\
X(_NET_WM_NAME),\
X(_NET_WM_WINDOW_TYPE),\
X(_NET_WM_WINDOW_TYPE_DESKTOP),\
X(_NET_WM_WINDOW_TYPE_DOCK),\
X(_NET_WM_WINDOW_TYPE_SPLASH),\
X(_NET_WM_WINDOW_TYPE_UTILITY),\
X(_NET_WM_WINDOW_TYPE_TOOLBAR),\
X(_NET_WM_WINDOW_TYPE_MENU),\
X(_NET_WM_WINDOW_TYPE_DIALOG),\
X(_NET_WM_WINDOW_TYPE_NORMAL),\
X(_NET_WM_STATE),\
X(_NET_WM_STATE_MODAL),\
X(_NET_WM_STATE_STICKY),\
X(_NET_WM_STATE_MAXIMIZED_VERT),\
X(_NET_WM_STATE_MAXIMIZED_HORZ),\
X(_NET_WM_STATE_SHADED),\
X(_NET_WM_STATE_SKIP_TASKBAR),\
X(_NET_WM_STATE_SKIP_PAGER),\
X(_NET_WM_STATE_HIDDEN),\
X(_NET_WM_STATE_FULLSCREEN),\
X(_NET_WM_STATE_ABOVE),\
X(_NET_WM_STATE_BELOW),\
X(_NET_WM_STATE_DEMANDS_ATTENTION),\
X(_NET_WM_STATE_ADD),\
X(_NET_WM_STATE_REMOVE),\
X(_NET_WM_STATE_TOGGLE),\
X(_NET_WM_STRUT),\
X(_NET_WM_STRUT_PARTIAL),\
X(_NET_WM_DESKTOP),\
X(_NET_SUPPORTED)

enum { EWMH_ATOMS( ATOM_ENUM ), NETATOMS };
const char *netatom_names[] = { EWMH_ATOMS( ATOM_CHAR ) };
Atom netatoms[NETATOMS];

// X error handler
int oops( __attribute__( ( unused ) ) Display *d, XErrorEvent *ee )
{
    if ( ee->error_code == BadWindow
         || ( ee->request_code == X_GrabButton && ee->error_code == BadAccess )
         || ( ee->request_code == X_GrabKey && ee->error_code == BadAccess )
       ) return 0;

    fprintf( stderr, "error: request code=%d, error code=%d\n", ee->request_code, ee->error_code );
    return xerror( display, ee );
}

// usable space on a monitor
typedef struct {
    int x, y, w, h;
    int l, r, t, b;
} workarea;


// window lists
typedef struct {
    Window *array;
    void **data;
    int len;
} winlist;

winlist *cache_client;
winlist *cache_xattr;

#define winlist_ascend(l,i,w) for ((i) = 0; (i) < (l)->len && (((w) = (l)->array[i]) || 1); (i)++)
#define winlist_descend(l,i,w) for ((i) = (l)->len-1; (i) >= 0 && (((w) = (l)->array[i]) || 1); (i)--)

#define WINLIST 32

winlist* winlist_new()
{
    winlist *l = allocate( sizeof( winlist ) );
    l->len = 0;
    l->array = allocate( sizeof( Window ) * ( WINLIST+1 ) );
    l->data  = allocate( sizeof( void* ) * ( WINLIST+1 ) );
    return l;
}
int winlist_append( winlist *l, Window w, void *d )
{
    if ( l->len > 0 && !( l->len % WINLIST ) ) {
        l->array = reallocate( l->array, sizeof( Window ) * ( l->len+WINLIST+1 ) );
        l->data  = reallocate( l->data,  sizeof( void* )  * ( l->len+WINLIST+1 ) );
    }

    l->data[l->len] = d;
    l->array[l->len++] = w;
    return l->len-1;
}
void winlist_empty( winlist *l )
{
    while ( l->len > 0 ) free( l->data[--( l->len )] );
}
void winlist_free( winlist *l )
{
    winlist_empty( l );
    free( l->array );
    free( l->data );
    free( l );
}
void winlist_empty_2d( winlist *l )
{
    while ( l->len > 0 ) winlist_free( l->data[--( l->len )] );
}
int winlist_find( winlist *l, Window w )
{
// iterate backwards. theory is: windows most often accessed will be
// nearer the end. testing with kcachegrind seems to support this...
    int i;
    Window o;

    winlist_descend( l, i, o ) if ( w == o ) return i;

    return -1;
}
int winlist_forget( winlist *l, Window w )
{
    int i, j;

    for ( i = 0, j = 0; i < l->len; i++, j++ ) {
        l->array[j] = l->array[i];
        l->data[j]  = l->data[i];

        if ( l->array[i] == w ) {
            free( l->data[i] );
            j--;
        }
    }

    l->len -= ( i-j );
    return j != i ?1:0;
}

#define CLIENTTITLE 100
#define CLIENTCLASS 50
#define CLIENTNAME 50
#define CLIENTSTATE 10
#define CLIENTROLE 50

// a managable window
typedef struct {
    Window window, trans;
    XWindowAttributes xattr;
    char title[CLIENTTITLE], class[CLIENTCLASS], name[CLIENTNAME], role[CLIENTROLE];
    int states;
    Atom state[CLIENTSTATE], type;
    workarea monitor;
} client;

#define MENUXFTFONT "mono-14"
#define MENUWIDTH 50
#define MENULINES 15
#define MENUFG "#222222"
#define MENUBG "#f2f1f0"
#define MENUBGALT "#e9e8e7"
#define MENUHLFG "#ffffff"
#define MENUHLBG "#005577"
#define MENUBC "black"

char *config_menu_font;
char *config_menu_fg;
char *config_menu_bg;
char *config_menu_hlfg;
char *config_menu_hlbg;
char *config_menu_bgalt;
char *config_menu_bc;
unsigned int config_menu_width;
int config_menu_lines;
unsigned int config_focus_mode;
unsigned int config_raise_mode;
unsigned int config_window_placement;
unsigned int config_window_opacity;
unsigned int config_zeltak_mode;
#ifdef I3
int config_i3_mode = 0;
#endif

// allocate a pixel value for an X named color
static unsigned int color_get( const char *const name )
{
    XColor color;
    Colormap map = DefaultColormap( display, screen_id );
    return XAllocNamedColor( display, map, name, &color, &color ) ? color.pixel: None;
}

// find mouse pointer location
int pointer_get( Window root, int *x, int *y )
{
    *x = 0;
    *y = 0;
    Window rr, cr;
    int rxr, ryr, wxr, wyr;
    unsigned int mr;

    if ( XQueryPointer( display, root, &rr, &cr, &rxr, &ryr, &wxr, &wyr, &mr ) ) {
        *x = rxr;
        *y = ryr;
        return 1;
    }

    return 0;
}

int take_keyboard( Window w )
{
    int i;

    for ( i = 0; i < 1000; i++ ) {
        if ( XGrabKeyboard( display, w, True, GrabModeAsync, GrabModeAsync, CurrentTime ) == GrabSuccess )
            return 1;

        usleep( 1000 );
    }

    return 0;
}
void release_keyboard()
{
    XUngrabKeyboard( display, CurrentTime );
}

// XGetWindowAttributes with caching
XWindowAttributes* window_get_attributes( Window w )
{
    int idx = winlist_find( cache_xattr, w );

    if ( idx < 0 ) {
        XWindowAttributes *cattr = allocate( sizeof( XWindowAttributes ) );

        if ( XGetWindowAttributes( display, w, cattr ) ) {
            winlist_append( cache_xattr, w, cattr );
            return cattr;
        }

        free( cattr );
        return NULL;
    }

    return cache_xattr->data[idx];
}

// retrieve a property of any type from a window
int window_get_prop( Window w, Atom prop, Atom *type, int *items, void *buffer, unsigned int bytes )
{
    Atom _type;

    if ( !type ) type = &_type;

    int _items;

    if ( !items ) items = &_items;

    int format;
    unsigned long nitems, nbytes;
    unsigned char *ret = NULL;
    memset( buffer, 0, bytes );

    if ( XGetWindowProperty( display, w, prop, 0, bytes/4, False, AnyPropertyType, type,
                             &format, &nitems, &nbytes, &ret ) == Success && ret && *type != None && format ) {
        if ( format ==  8 ) memmove( buffer, ret, MIN( bytes, nitems ) );

        if ( format == 16 ) memmove( buffer, ret, MIN( bytes, nitems * sizeof( short ) ) );

        if ( format == 32 ) memmove( buffer, ret, MIN( bytes, nitems * sizeof( long ) ) );

        *items = ( int )nitems;
        XFree( ret );
        return 1;
    }

    return 0;
}

// retrieve a text property from a window
// technically we could use window_get_prop(), but this is better for character set support
char* window_get_text_prop( Window w, Atom atom )
{
    XTextProperty prop;
    char *res = NULL;
    char **list = NULL;
    int count;

    if ( XGetTextProperty( display, w, &prop, atom ) && prop.value && prop.nitems ) {
        if ( prop.encoding == XA_STRING ) {
            res = allocate( strlen( ( char* )prop.value )+1 );
            strcpy( res, ( char* )prop.value );
        } else if ( XmbTextPropertyToTextList( display, &prop, &list, &count ) >= Success && count > 0 && *list ) {
            res = allocate( strlen( *list )+1 );
            strcpy( res, *list );
            XFreeStringList( list );
        }
    }

    if ( prop.value ) XFree( prop.value );

    return res;
}

int window_get_atom_prop( Window w, Atom atom, Atom *list, int count )
{
    Atom type;
    int items;
    return window_get_prop( w, atom, &type, &items, list, count*sizeof( Atom ) ) && type == XA_ATOM ? items:0;
}

void window_set_atom_prop( Window w, Atom prop, Atom *atoms, int count )
{
    XChangeProperty( display, w, prop, XA_ATOM, 32, PropModeReplace, ( unsigned char* )atoms, count );
}

int window_get_cardinal_prop( Window w, Atom atom, unsigned long *list, int count )
{
    Atom type;
    int items;
    return window_get_prop( w, atom, &type, &items, list, count*sizeof( unsigned long ) ) && type == XA_CARDINAL ? items:0;
}

// a ClientMessage
int window_send_message( Window target, Window subject, Atom atom, unsigned long protocol, unsigned long mask, Time time )
{
    XEvent e;
    memset( &e, 0, sizeof( XEvent ) );
    e.xclient.type = ClientMessage;
    e.xclient.message_type = atom;
    e.xclient.window    = subject;
    e.xclient.data.l[0]    = protocol;
    e.xclient.data.l[1] = time;
    e.xclient.send_event   = True;
    e.xclient.format    = 32;
    int r = XSendEvent( display, target, False, mask, &e ) ?1:0;
    XFlush( display );
    return r;
}

// find the dimensions of the monitor displaying point x,y
void monitor_dimensions( Screen *screen, int x, int y, workarea *mon )
{
    memset( mon, 0, sizeof( workarea ) );
    mon->w = WidthOfScreen( screen );
    mon->h = HeightOfScreen( screen );

// locate the current monitor
    if ( XineramaIsActive( display ) ) {
        int monitors, i;
        XineramaScreenInfo *info = XineramaQueryScreens( display, &monitors );

        if ( info ) for ( i = 0; i < monitors; i++ ) {
                if ( INTERSECT( x, y, 1, 1, info[i].x_org, info[i].y_org, info[i].width, info[i].height ) ) {
                    mon->x = info[i].x_org;
                    mon->y = info[i].y_org;
                    mon->w = info[i].width;
                    mon->h = info[i].height;
                    break;
                }
            }

        XFree( info );
    }
}

// determine which monitor holds the active window, or failing that the mouse pointer
void monitor_active( workarea *mon )
{
    Window root = RootWindow( display, XScreenNumberOfScreen( screen ) );

    unsigned long id;
    Atom type;
    int count;

    if ( window_get_prop( root, netatoms[_NET_ACTIVE_WINDOW], &type, &count, &id, 1 )
         && type == XA_WINDOW && count > 0 ) {
        XWindowAttributes *attr = window_get_attributes( id );
        monitor_dimensions( screen, attr->x, attr->y, mon );
        return;
    }

    int x, y;

    if ( pointer_get( root, &x, &y ) ) {
        monitor_dimensions( screen, x, y, mon );
        return;
    }

    monitor_dimensions( screen, 0, 0, mon );
}

// _NET_WM_STATE_*
int client_has_state( client *c, Atom state )
{
    int i;

    for ( i = 0; i < c->states; i++ )
        if ( c->state[i] == state ) return 1;

    return 0;
}

// collect info on any window
// doesn't have to be a window we'll end up managing
client* window_client( Window win )
{
    if ( win == None ) return NULL;

    int idx = winlist_find( cache_client, win );

    if ( idx >= 0 ) {
        return cache_client->data[idx];
    }

// if this fails, we're up that creek
    XWindowAttributes *attr = window_get_attributes( win );

    if ( !attr ) return NULL;

    client *c = allocate_clear( sizeof( client ) );
    c->window = win;
// copy xattr so we don't have to care when stuff is freed
    memmove( &c->xattr, attr, sizeof( XWindowAttributes ) );
    XGetTransientForHint( display, win, &c->trans );

    c->states = window_get_atom_prop( win, netatoms[_NET_WM_STATE], c->state, CLIENTSTATE );
    window_get_atom_prop( win, netatoms[_NET_WM_WINDOW_TYPE], &c->type, 1 );

    if ( c->type == None ) c->type = ( c->trans != None )
                                         // trasients default to dialog
                                         ? netatoms[_NET_WM_WINDOW_TYPE_DIALOG]
                                         // non-transients default to normal
                                         : netatoms[_NET_WM_WINDOW_TYPE_NORMAL];

    char *name;

    if ( ( name = window_get_text_prop( c->window, netatoms[_NET_WM_NAME] ) ) && name ) {
        snprintf( c->title, CLIENTTITLE, "%s", name );
        free( name );
    } else if ( XFetchName( display, c->window, &name ) ) {
        snprintf( c->title, CLIENTTITLE, "%s", name );
        XFree( name );
    }

    name = window_get_text_prop ( c->window, XInternAtom( display, "WM_WINDOW_ROLE", False ) );

    if ( name != NULL ) {
        snprintf( c->role, CLIENTROLE, "%s", name );
        XFree( name );
    }

    XClassHint chint;

    if ( XGetClassHint( display, c->window, &chint ) ) {
        snprintf( c->class, CLIENTCLASS, "%s", chint.res_class );
        snprintf( c->name, CLIENTNAME, "%s", chint.res_name );
        XFree( chint.res_class );
        XFree( chint.res_name );
    }

    monitor_dimensions( c->xattr.screen, c->xattr.x, c->xattr.y, &c->monitor );
    winlist_append( cache_client, c->window, c );
    return c;
}

unsigned int windows_modmask;
KeySym windows_keysym;
unsigned int rundialog_modmask;
KeySym rundialog_keysym;
// flags to set if we switch modes on the fly
Window main_window = None;

#include "textbox.h"

void menu_draw( textbox *text, textbox **boxes, int max_lines, int selected, char **filtered )
{
    int i;
    textbox_draw( text );

    for ( i = 0; i < max_lines; i++ ) {
        if ( filtered[i] == NULL ) {
            textbox_font( boxes[i], config_menu_font,
                          config_menu_fg,
                          config_menu_bg );
            textbox_text( boxes[i], "" );
        } else {
            textbox_font( boxes[i], config_menu_font,
                          i == selected ? config_menu_hlfg: config_menu_fg,
                          i == selected ? config_menu_hlbg: config_menu_bg );
            textbox_text( boxes[i], filtered[i] );
        }

        textbox_draw( boxes[i] );
    }
}


/* Very bad implementation of tab completion.
* It will complete to the common prefix
*/
static int calculate_common_prefix( char **filtered, int max_lines )
{
    int length_prefix = 0,j,found = 1;

    if ( filtered[0] != NULL ) {
        char *p = filtered[0];

        do {
            found = 1;

            for ( j=0; j < max_lines && filtered[j] != NULL; j++ ) {
                if ( filtered[j][length_prefix] == '\0' || filtered[j][length_prefix] != *p ) {
                    if ( found )
                        found=0;

                    break;
                }
            }

            if ( found )
                length_prefix++;

            p++;
        } while ( found );
    }

    return length_prefix;
}


int window_match ( char **tokens, const char *input, int index, void *data)
{
    int match =1;
    winlist *ids = (winlist *)data;
    client *c = window_client(ids->array[index]);
    if ( tokens ) for ( int j  = 1; match && tokens[j]; j++ ) {
        int test = 0;

        if ( !test && c->title[0] != '\0' )
            test = ( strcasestr( c->title, tokens[j] ) != NULL );

        if ( !test && c->class[0] != '\0' )
            test = ( strcasestr( c->class, tokens[j] ) != NULL );

        if ( !test && c->role[0] != '\0' )
            test = ( strcasestr( c->role, tokens[j] ) != NULL );

        if ( !test && c->name[0] != '\0' )
            test = ( strcasestr( c->name, tokens[j] ) != NULL );

        if ( test == 0 ) match = 0;
    }

    return match;
} 

int menu( char **lines, char **input, char *prompt, int selected, Time *time, int *shift,
        menu_match_cb mmc, void *mmc_data)
{
    int line = -1, i, j, chosen = 0;
    workarea mon;
    monitor_active( &mon );

    int num_lines = 0;

    for ( ; lines[num_lines]; num_lines++ );

    int max_lines = MIN( config_menu_lines, num_lines );
    selected = MAX( MIN( num_lines-1, selected ), 0 );

    int w = config_menu_width < 101 ? ( mon.w/100 )*config_menu_width: config_menu_width;
    int x = mon.x + ( mon.w - w )/2;

    Window box;
    XWindowAttributes attr;

    // main window isn't explicitly destroyed in case we switch modes. Reusing it prevents flicker
    if ( main_window != None && XGetWindowAttributes( display, main_window, &attr ) ) {
        box = main_window;
    } else {
        box = XCreateSimpleWindow( display, root, x, 0, w, 300, 1, color_get( config_menu_bc ), color_get( config_menu_bg ) );
        XSelectInput( display, box, ExposureMask );
        // make it an unmanaged window
        window_set_atom_prop( box, netatoms[_NET_WM_STATE], &netatoms[_NET_WM_STATE_ABOVE], 1 );
        //window_set_atom_prop(box, netatoms[_NET_WM_WINDOW_TYPE], &netatoms[_NET_WM_WINDOW_TYPE_DOCK], 1);
        XSetWindowAttributes sattr;
        sattr.override_redirect = True;
        XChangeWindowAttributes( display, box, CWOverrideRedirect, &sattr );
        main_window = box;

        // Set the WM_NAME
        XStoreName( display, box, "simpleswitcher" );

        // Hack to set window opacity.
        unsigned int opacity_set = ( unsigned int )( ( config_window_opacity/100.0 )* OPAQUE );
        XChangeProperty( display, box, XInternAtom( display, OPACITY, False ),
                         XA_CARDINAL, 32, PropModeReplace,
                         ( unsigned char * ) &opacity_set, 1L );
    }

    // search text input
    textbox *text = textbox_create( box, TB_AUTOHEIGHT|TB_EDITABLE, INNER_MARGIN, INNER_MARGIN,
                                    w-( 2*INNER_MARGIN ), 1,
                                    config_menu_font, config_menu_fg, config_menu_bg,
                                    ( input!= NULL )?*input:"", prompt );
    textbox_show( text );

    int line_height = text->font->ascent + text->font->descent;
    //line_height += line_height/10;
    int row_margin = line_height/10;
    line_height+=row_margin;

    // filtered list display
    textbox **boxes = allocate_clear( sizeof( textbox* ) * max_lines );

    for ( i = 0; i < max_lines; i++ ) {
        boxes[i] = textbox_create( box, TB_AUTOHEIGHT, INNER_MARGIN, ( i+1 ) * line_height +
                                   INNER_MARGIN, w-( 2*INNER_MARGIN ), 1,
                                   config_menu_font, config_menu_fg, config_menu_bg, lines[i], NULL );
        textbox_show( boxes[i] );
    }

    // filtered list
    char **filtered = allocate_clear( sizeof( char* ) * max_lines );
    int *line_map = allocate_clear( sizeof( int ) * max_lines );
    int filtered_lines = 0;

    int jin = 0;

    if ( input && *input ) {
        char **tokens = tokenize( *input );

        // input changed
        for ( i = 0, j = 0; i < num_lines && j < max_lines; i++ ) {
            int match = mmc(tokens,lines[i], i, mmc_data); 

            // If each token was matched, add it to list.
            if ( match ) {
                line_map[j] = i;
                filtered[j++] = lines[i];
                filtered_lines++;
            }
        }

        tokenize_free( tokens );
    } else {
        for ( i = 0; i < max_lines; i++ ) {
            filtered[jin] = lines[i];
            line_map[jin] = i;
            jin++;
            filtered_lines++;
        }
    }

    // resize window vertically to suit
    // Subtract the margin of the last row.
    int h = line_height * ( max_lines+1 ) + INNER_MARGIN*2 - row_margin;
    int y = mon.y + ( mon.h - h )/2;
    XMoveResizeWindow( display, box, x, y, w, h );
    XMapRaised( display, box );

    take_keyboard( box );

    for ( ;; ) {
        XEvent ev;
        XNextEvent( display, &ev );

        if ( ev.type == Expose ) {
            while ( XCheckTypedEvent( display, Expose, &ev ) );

            menu_draw( text, boxes, max_lines, selected, filtered );
        } else if ( ev.type == KeyPress ) {
            while ( XCheckTypedEvent( display, KeyPress, &ev ) );

            if ( time )
                *time = ev.xkey.time;

            KeySym key = XkbKeycodeToKeysym( display, ev.xkey.keycode, 0, 0 );

            if ( ( ( ev.xkey.state&ShiftMask ) == ShiftMask ) &&
                 key == XK_slash ) {
                line = -2;
                break;
            }

            int rc = textbox_keypress( text, &ev );

            if ( rc < 0 ) {
                if ( shift != NULL )
                    ( *shift ) = ( ( ev.xkey.state&ShiftMask ) == ShiftMask );

                chosen = 1;
                break;
            } else if ( rc ) {
                char **tokens = tokenize( text->text );

                // input changed
                for ( i = 0, j = 0; i < num_lines && j < max_lines; i++ ) {
                    int match = mmc(tokens,lines[i], i, mmc_data); 

                    // If each token was matched, add it to list.
                    if ( match ) {
                        line_map[j] = i;
                        filtered[j++] = lines[i];
                    }
                }

                // Cleanup + bookkeeping.
                filtered_lines = j;
                selected = MAX( 0, MIN( selected, j-1 ) );

                for ( ; j < max_lines; j++ )
                    filtered[j] = NULL;

                if ( config_zeltak_mode && filtered_lines == 1 ) {
                    chosen = 1;
                    break;
                }

                tokenize_free( tokens );
            } else {
                // unhandled key
                KeySym key = XkbKeycodeToKeysym( display, ev.xkey.keycode, 0, 0 );

                if ( key == XK_Escape
                     // pressing one of the global key bindings closes the switcher. this allows fast closing of the menu if an item is not selected
                     || ( ( windows_modmask == AnyModifier || ev.xkey.state & windows_modmask ) && key == windows_keysym )
                     || ( ( rundialog_modmask == AnyModifier || ev.xkey.state & rundialog_modmask ) && key == rundialog_keysym )
                   ) {
                    break;
                }

                else

                    // Up or Shift-Tab
                    if ( key == XK_Up || ( key == XK_Tab && ev.xkey.state & ShiftMask ) )
                        selected = selected ? MAX( 0, selected-1 ): MAX( 0, filtered_lines-1 );

                    else

                        // Down or Tab
                        if ( key == XK_Down || key == XK_Tab ) {
                            if ( filtered_lines == 1 ) {
                                chosen = 1;
                                break;
                            }

                            int length_prefix = calculate_common_prefix( filtered, max_lines );

                            if ( length_prefix && strncasecmp( filtered[0], text->text, length_prefix ) ) {
                                // Do not want to modify original string, so make copy.
                                // not eff..
                                char * str = strndup( filtered[0], length_prefix );
                                textbox_text( text, str );
                                textbox_cursor_end( text );
                                free( str );
                            } else
                                selected = selected < filtered_lines-1 ? MIN( filtered_lines-1, selected+1 ): 0;
                        }
            }

            menu_draw( text, boxes, max_lines, selected, filtered );
        }
    }

    release_keyboard();

    if ( chosen && filtered[selected] ) {
        line = line_map[selected];
    }

    if ( line < 0 && input ) {
        if ( *input != NULL ) free( *input );

        *input = strdup( text->text );

        // If chosen and the like.
        if ( chosen )
            line = -3;
    }


    textbox_free( text );

    for ( i = 0; i < max_lines; i++ )
        textbox_free( boxes[i] );

    free( boxes );

    free( filtered );
    free( line_map );

    return line;
}

#define FORK 1
#define NOFORK 2




SwitcherMode run_switcher_window ( char **input )
{
    SwitcherMode retv = MODE_EXIT;
    // find window list
    Atom type;
    int nwins;
    unsigned long wins[100];

    if ( window_get_prop( root, netatoms[_NET_CLIENT_LIST_STACKING],
                          &type, &nwins, wins, 100 * sizeof( unsigned long ) )
         && type == XA_WINDOW ) {
        char pattern[50];
        int i;
        unsigned int classfield = 0;
        // windows we actually display. may be slightly different to _NET_CLIENT_LIST_STACKING
        // if we happen to have a window destroyed while we're working...
        winlist *ids = winlist_new();

        // calc widths of fields
        for ( i = nwins-1; i > -1; i-- ) {
            client *c;

            if ( ( c = window_client( wins[i] ) )
                 && !c->xattr.override_redirect
                 && !client_has_state( c, netatoms[_NET_WM_STATE_SKIP_PAGER] )
                 && !client_has_state( c, netatoms[_NET_WM_STATE_SKIP_TASKBAR] ) ) {

                classfield = MAX( classfield, strlen( c->class ) );

#ifdef I3

                // In i3 mode, skip the i3bar completely.
                if ( config_i3_mode && strstr( c->class, "i3bar" ) != NULL ) continue;

#endif

                winlist_append( ids, c->window, NULL );
            }
        }

        // Create pattern for printing the line.
        sprintf( pattern, "%%-%ds   %%s", MAX( 5, classfield ) );
        char **list = allocate_clear( sizeof( char* ) * ( ids->len+1 ) );
        int lines = 0;

        // build the actual list
        Window w;
        winlist_ascend( ids, i, w ) {
            client *c;

            if ( ( c = window_client( w ) ) ) {
                // final line format
                char *line = allocate( strlen( c->title ) + strlen( c->class ) + classfield + 50 );

                sprintf( line, pattern, c->class, c->title );

                list[lines++] = line;
            }
        }
        Time time;
        int n = menu( list, input, "> ", 0, &time, NULL,window_match, ids );

        if ( n == -2 ) {
            retv = RUN_DIALOG;
        } else if ( n >= 0 && list[n] ) {
#ifdef I3

            if ( config_i3_mode ) {
                // Hack for i3.
                focus_window_i3( i3_socket_path,ids->array[n] );
            } else
#endif
            {
                window_send_message( root, ids->array[n], netatoms[_NET_ACTIVE_WINDOW], 2, // 2 = pager
                                     SubstructureNotifyMask | SubstructureRedirectMask, time );
            }
        }


        for ( i = 0; i < lines; i++ )
            free( list[i] );

        free( list );
        winlist_free( ids );
    }

    return retv;
}

void run_switcher( int fmode, SwitcherMode mode )
{
    // TODO: this whole function is messy. build a nicer solution

    // we fork because it's technically possible to have multiple window
    // lists up at once on a zaphod multihead X setup.
    // this also happens to isolate the Xft font stuff in a child process
    // that gets cleaned up every time. that library shows some valgrind
    // strangeness...
    if ( fmode == FORK ) {
        if ( fork() ) return;

        display = XOpenDisplay( 0 );
        XSync( display, True );
    }

    char *input = NULL;

    do {
        if ( mode == WINDOW_SWITCHER ) {
            mode = run_switcher_window( &input );
        } else if ( mode == RUN_DIALOG ) {
            mode = run_switcher_dialog( &input );
        }
    } while ( mode != MODE_EXIT );

    if ( input != NULL ) {
        free( input );
    }

    if ( fmode == FORK ) {
        exit( EXIT_SUCCESS );
    }
}

// KeyPress event
void handle_keypress( XEvent *ev )
{
    KeySym key = XkbKeycodeToKeysym( display, ev->xkey.keycode, 0, 0 );

    if ( ( windows_modmask == AnyModifier || ev->xkey.state & windows_modmask ) &&
         key == windows_keysym ) {
        run_switcher( FORK , WINDOW_SWITCHER );
    }

    if ( ( rundialog_modmask == AnyModifier || ev->xkey.state & rundialog_modmask ) &&
         key == rundialog_keysym ) {
        run_switcher( FORK , RUN_DIALOG );
    }
}

// convert a Mod+key arg to mod mask and keysym
void parse_key( char *combo, unsigned int *mod, KeySym *key )
{
    unsigned int modmask = 0;

    if ( strcasestr( combo, "shift" ) )   modmask |= ShiftMask;

    if ( strcasestr( combo, "control" ) ) modmask |= ControlMask;

    if ( strcasestr( combo, "mod1" ) )    modmask |= Mod1Mask;

    if ( strcasestr( combo, "alt" ) )    modmask |= Mod1Mask;

    if ( strcasestr( combo, "mod2" ) )    modmask |= Mod2Mask;

    if ( strcasestr( combo, "mod3" ) )    modmask |= Mod3Mask;

    if ( strcasestr( combo, "mod4" ) )    modmask |= Mod4Mask;

    if ( strcasestr( combo, "mod5" ) )    modmask |= Mod5Mask;

    *mod = modmask ? modmask: AnyModifier;

    char i = strlen( combo );

    while ( i > 0 && !strchr( "-+", combo[i-1] ) ) i--;

    KeySym sym = XStringToKeysym( combo+i );

    if ( sym == NoSymbol || ( !modmask && ( strchr( combo, '-' ) || strchr( combo, '+' ) ) ) ) {
        fprintf( stderr, "sorry, cannot understand key combination: %s\n", combo );
        exit( EXIT_FAILURE );
    }

    *key = sym;
}

// bind a key combination on a root window, compensating for Lock* states
void grab_key( unsigned int modmask, KeySym key )
{
    KeyCode keycode = XKeysymToKeycode( display, key );
    XUngrabKey( display, keycode, AnyModifier, root );

    if ( modmask != AnyModifier ) {
        // bind to combinations of mod and lock masks, so caps and numlock don't confuse people
        XGrabKey( display, keycode, modmask, root, True, GrabModeAsync, GrabModeAsync );
        XGrabKey( display, keycode, modmask|LockMask, root, True, GrabModeAsync, GrabModeAsync );

        if ( NumlockMask ) {
            XGrabKey( display, keycode, modmask|NumlockMask, root, True, GrabModeAsync, GrabModeAsync );
            XGrabKey( display, keycode, modmask|NumlockMask|LockMask, root, True, GrabModeAsync, GrabModeAsync );
        }
    } else {
        // nice simple single key bind
        XGrabKey( display, keycode, AnyModifier, root, True, GrabModeAsync, GrabModeAsync );
    }
}



/**
 * Help function. This calls man.
 */
void help()
{
    int code = execlp( "man","man", MANPAGE_PATH,NULL );

    if ( code == -1 ) {
        fprintf( stderr, "Failed to execute man: %s\n", strerror( errno ) );
    }
}




int main( int argc, char *argv[] )
{
    int i, j;

    // catch help request
    if ( find_arg( argc, argv, "-help" ) >= 0
         || find_arg( argc, argv, "--help" ) >= 0
         || find_arg( argc, argv, "-h" ) >= 0 ) {
        help();
        return EXIT_SUCCESS;
    }

    const char *display_str= find_arg_str( argc, argv, "-display", getenv( "DISPLAY" ) );

    if ( !( display = XOpenDisplay( display_str ) ) ) {
        fprintf( stderr, "cannot open display!\n" );
        return EXIT_FAILURE;
    }

    signal( SIGCHLD, catch_exit );
    screen = DefaultScreenOfDisplay( display );
    screen_id = DefaultScreen( display );
    root = DefaultRootWindow( display );
    XSync( display, False );
    xerror = XSetErrorHandler( oops );
    XSync( display, False );

    // determine numlock mask so we can bind on keys with and without it
    XModifierKeymap *modmap = XGetModifierMapping( display );

    for ( i = 0; i < 8; i++ )
        for ( j = 0; j < ( int )modmap->max_keypermod; j++ )
            if ( modmap->modifiermap[i*modmap->max_keypermod+j] == XKeysymToKeycode( display, XK_Num_Lock ) )
                NumlockMask = ( 1<<i );

    XFreeModifiermap( modmap );

    int ac = argc;
    char **av = argv;
    cache_client = winlist_new();
    cache_xattr  = winlist_new();

    // X atom values
    for ( i = 0; i < NETATOMS; i++ ) netatoms[i] = XInternAtom( display, netatom_names[i], False );

    config_menu_width     = find_arg_int( ac, av, "-width", MENUWIDTH );
    config_menu_lines     = find_arg_int( ac, av, "-lines", MENULINES );
    config_menu_font      = find_arg_str( ac, av, "-font", MENUXFTFONT );
    config_menu_fg        = find_arg_str( ac, av, "-fg", MENUFG );
    config_menu_bg        = find_arg_str( ac, av, "-bg", MENUBG );
    config_menu_bgalt     = find_arg_str( ac, av, "-bgalt", MENUBGALT );
    config_menu_hlfg      = find_arg_str( ac, av, "-hlfg", MENUHLFG );
    config_menu_hlbg      = find_arg_str( ac, av, "-hlbg", MENUHLBG );
    config_menu_bc        = find_arg_str( ac, av, "-bc", MENUBC );
    config_window_opacity = find_arg_int( ac, av, "-o", 100 );

    config_terminal_emulator = find_arg_str( ac, av, "-term", TERMINAL_DEFAULT );

    config_zeltak_mode    = ( find_arg( ac, av, "-zeltak" ) >= 0 );

#ifdef I3
    // Check for i3
    {
        config_i3_mode = 0;
        Atom atom = XInternAtom( display, I3_SOCKET_PATH_PROP,True );

        if ( atom != None ) {
            i3_socket_path = window_get_text_prop( root, atom );

            if ( i3_socket_path != NULL ) {
                printf( "Auto detected I3 running, switching to I3 mode: %s\n",
                        i3_socket_path );
                config_i3_mode = 1;
            }
        }
    }
#endif

    // flags to run immediately and exit
    if ( find_arg( ac, av, "-now" ) >= 0 ) {
        run_switcher( NOFORK, WINDOW_SWITCHER );
#ifdef I3

        if ( i3_socket_path != NULL ) free( i3_socket_path );

#endif
        exit( EXIT_SUCCESS );
    }

    if ( find_arg( ac, av, "-rnow" ) >= 0 ) {
        run_switcher( NOFORK, RUN_DIALOG );
#ifdef I3

        if ( i3_socket_path != NULL ) free( i3_socket_path );

#endif
        exit( EXIT_SUCCESS );
    }

    // in background mode from here on

    // key combination to display all windows from all desktops
    parse_key( find_arg_str( ac, av, "-key", "F12" ), &windows_modmask, &windows_keysym );
    parse_key( find_arg_str( ac, av, "-rkey", "mod1+F2" ), &rundialog_modmask, &rundialog_keysym );


    // bind key combos
    grab_key( windows_modmask, windows_keysym );
    grab_key( rundialog_modmask, rundialog_keysym );

    XEvent ev;

    for ( ;; ) {
        // caches only live for a single event
        winlist_empty( cache_xattr );
        winlist_empty( cache_client );

        // block and wait for something
        XNextEvent( display, &ev );

        if ( ev.xany.window == None ) continue;

        if ( ev.type == KeyPress ) handle_keypress( &ev );
    }

#ifdef I3

    if ( i3_socket_path != NULL ) free( i3_socket_path );

#endif
    return EXIT_SUCCESS;
}