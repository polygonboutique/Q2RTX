/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
#include "client.h"

/*
key up events are sent even if in console mode
*/

static int  anykeydown;

static keywaitcb_t  key_wait_cb;
static void         *key_wait_arg;

static char     *keybindings[256];

// if false, passed to interpreter while in console
static qboolean consolekeys[256];

// if true, passed to interpreter while in menu
static qboolean menubound[256];

#if !USE_CHAR_EVENTS
// key to map to if shift held down in console
static int      keyshift[256];
#endif

static int      key_repeats[256];   // if > 1, it is autorepeating
static qboolean keydown[256];

static qboolean key_overstrike;

typedef struct keyname_s {
    char    *name;
    int     keynum;
} keyname_t;

#define K(x) { #x, K_##x }

static const keyname_t keynames[] = {
    K(BACKSPACE),
    K(TAB),
    K(ENTER),
    K(PAUSE),
    K(ESCAPE),
    K(SPACE),

    K(UPARROW),
    K(DOWNARROW),
    K(LEFTARROW),
    K(RIGHTARROW),

    K(ALT),
    K(LALT),
    K(RALT),
    K(CTRL),
    K(LCTRL),
    K(RCTRL),
    K(SHIFT),
    K(LSHIFT),
    K(RSHIFT),

    K(F1),
    K(F2),
    K(F3),
    K(F4),
    K(F5),
    K(F6),
    K(F7),
    K(F8),
    K(F9),
    K(F10),
    K(F11),
    K(F12),

    K(INS),
    K(DEL),
    K(PGDN),
    K(PGUP),
    K(HOME),
    K(END),

    K(NUMLOCK),
    K(CAPSLOCK),
    K(SCROLLOCK),
    K(LWINKEY),
    K(RWINKEY),
    K(MENU),

    K(KP_HOME),
    K(KP_UPARROW),
    K(KP_PGUP),
    K(KP_LEFTARROW),
    K(KP_5),
    K(KP_RIGHTARROW),
    K(KP_END),
    K(KP_DOWNARROW),
    K(KP_PGDN),
    K(KP_ENTER),
    K(KP_INS),
    K(KP_DEL),
    K(KP_SLASH),
    K(KP_MINUS),
    K(KP_PLUS),
    K(KP_MULTIPLY),

    K(MOUSE1),
    K(MOUSE2),
    K(MOUSE3),
    K(MOUSE4),
    K(MOUSE5),
    K(MOUSE6),
    K(MOUSE7),
    K(MOUSE8),

    K(MWHEELUP),
    K(MWHEELDOWN),
    K(MWHEELRIGHT),
    K(MWHEELLEFT),

    {"SEMICOLON", ';'}, // because a raw semicolon seperates commands

    {NULL, 0}
};

#undef K

//============================================================================

/*
===================
Key_GetOverstrikeMode
===================
*/
qboolean Key_GetOverstrikeMode(void)
{
    return key_overstrike;
}

/*
===================
Key_SetOverstrikeMode
===================
*/
void Key_SetOverstrikeMode(qboolean overstrike)
{
    key_overstrike = overstrike;
}

/*
===================
Key_GetDest
===================
*/
keydest_t Key_GetDest(void)
{
    return cls.key_dest;
}

/*
===================
Key_SetDest
===================
*/
void Key_SetDest(keydest_t dest)
{
    int diff;

// if not connected, console or menu should be up
    if (cls.state < ca_active && !(dest & (KEY_MENU | KEY_CONSOLE))) {
        dest |= KEY_CONSOLE;
    }

    diff = cls.key_dest ^ dest;
    if (diff & KEY_CONSOLE) {
        if (dest & KEY_CONSOLE) {
// release all keys, to keep the character from continuing an
// action started before a console switch
            Key_ClearStates();
        }
    }

    cls.key_dest = dest;

// activate or deactivate mouse
    if (diff & (KEY_CONSOLE | KEY_MENU)) {
        IN_Activate();
        CL_CheckForPause();
    }
}

/*
===================
Key_IsDown
===================
*/
qboolean Key_IsDown(int key)
{
    if (key < 0 || key > 255) {
        return qfalse;
    }

    return keydown[key];
}

int Key_Repeats(int key)
{
    if (key < 0 || key > 255) {
        return 0;
    }

    return key_repeats[key];
}

/*
===================
Key_AnyKeyDown
===================
*/
qboolean Key_AnyKeyDown(void)
{
    return anykeydown;
}

/*
===================
Key_StringToKeynum

Returns a key number to be used to index keybindings[] by looking at
the given string.  Single ascii characters return themselves, while
the K_* names are matched up.
===================
*/
int Key_StringToKeynum(const char *str)
{
    const keyname_t *kn;

    if (!str || !str[0])
        return -1;
    if (!str[1])
        return str[0];

    for (kn = keynames; kn->name; kn++) {
        if (!Q_stricmp(str, kn->name))
            return kn->keynum;
    }
    return -1;
}

/*
===================
Key_KeynumToString

Returns a string (either a single ascii char, or a K_* name) for the
given keynum.
FIXME: handle quote special (general escape sequence?)
===================
*/
char *Key_KeynumToString(int keynum)
{
    const keyname_t *kn;
    static char tinystr[2];

    if (keynum == -1)
        return "<KEY NOT FOUND>";
    if (keynum > 32 && keynum < 127) {
        // printable ascii
        tinystr[0] = keynum;
        tinystr[1] = 0;
        return tinystr;
    }

    for (kn = keynames; kn->name; kn++)
        if (keynum == kn->keynum)
            return kn->name;

    return "<UNKNOWN KEYNUM>";
}

/*
===================
Key_SetBinding

Returns the name of the first key found.
===================
*/
char *Key_GetBinding(const char *binding)
{
    int key;

    for (key = 0; key < 256; key++) {
        if (keybindings[key]) {
            if (!Q_stricmp(keybindings[key], binding)) {
                return Key_KeynumToString(key);
            }
        }
    }

    return "";
}

/*
===================
Key_EnumBindings
===================
*/
int Key_EnumBindings(int key, const char *binding)
{
    if (key < 0) {
        key = 0;
    }
    for (; key < 256; key++) {
        if (keybindings[key]) {
            if (!Q_stricmp(keybindings[key], binding)) {
                return key;
            }
        }
    }

    return -1;
}

/*
===================
Key_SetBinding
===================
*/
void Key_SetBinding(int keynum, const char *binding)
{
    if (keynum < 0 || keynum > 255)
        return;

// free old binding
    if (keybindings[keynum]) {
        Z_Free(keybindings[keynum]);
    }

// allocate memory for new binding
    keybindings[keynum] = Z_CopyString(binding);
}

static void Key_Name_g(genctx_t *ctx)
{
    const keyname_t *k;

    ctx->ignorecase = qtrue;
    for (k = keynames; k->name; k++) {
        if (!Prompt_AddMatch(ctx, k->name)) {
            break;
        }
    }
}

static void Key_Bound_g(genctx_t *ctx)
{
    int i;

    ctx->ignorecase = qtrue;
    for (i = 0; i < 256; i++) {
        if (keybindings[i]) {
            if (!Prompt_AddMatch(ctx, Key_KeynumToString(i))) {
                break;
            }
        }
    }
}

static void Key_Bind_c(genctx_t *ctx, int argnum)
{
    if (argnum == 1) {
        Key_Name_g(ctx);
    } else {
        Com_Generic_c(ctx, argnum - 2);
    }
}

static void Key_Unbind_c(genctx_t *ctx, int argnum)
{
    if (argnum == 1) {
        Key_Bound_g(ctx);
    }
}

/*
===================
Key_Unbind_f
===================
*/
static void Key_Unbind_f(void)
{
    int     b;

    if (Cmd_Argc() != 2) {
        Com_Printf("unbind <key> : remove commands from a key\n");
        return;
    }

    b = Key_StringToKeynum(Cmd_Argv(1));
    if (b == -1) {
        Com_Printf("\"%s\" isn't a valid key\n", Cmd_Argv(1));
        return;
    }

    Key_SetBinding(b, NULL);
}

/*
===================
Key_Unbindall_f
===================
*/
static void Key_Unbindall_f(void)
{
    int     i;

    for (i = 0; i < 256; i++)
        if (keybindings[i])
            Key_SetBinding(i, NULL);
}


/*
===================
Key_Bind_f
===================
*/
static void Key_Bind_f(void)
{
    int c, b;

    c = Cmd_Argc();

    if (c < 2) {
        Com_Printf("bind <key> [command] : attach a command to a key\n");
        return;
    }
    b = Key_StringToKeynum(Cmd_Argv(1));
    if (b == -1) {
        Com_Printf("\"%s\" isn't a valid key\n", Cmd_Argv(1));
        return;
    }

    if (c == 2) {
        if (keybindings[b])
            Com_Printf("\"%s\" = \"%s\"\n", Cmd_Argv(1), keybindings[b]);
        else
            Com_Printf("\"%s\" is not bound\n", Cmd_Argv(1));
        return;
    }

// copy the rest of the command line
    Key_SetBinding(b, Cmd_ArgsFrom(2));
}

/*
============
Key_WriteBindings

Writes lines containing "bind key value"
============
*/
void Key_WriteBindings(qhandle_t f)
{
    int     i;

    for (i = 0; i < 256; i++) {
        if (keybindings[i] && keybindings[i][0]) {
            FS_FPrintf(f, "bind %s \"%s\"\n", Key_KeynumToString(i),
                       keybindings[i]);
        }
    }
}


/*
============
Key_Bindlist_f

============
*/
static void Key_Bindlist_f(void)
{
    int     i;

    for (i = 0; i < 256; i++) {
        if (keybindings[i] && keybindings[i][0]) {
            Com_Printf("%s \"%s\"\n", Key_KeynumToString(i),
                       keybindings[i]);
        }
    }
}

static cmdreg_t c_keys[] = {
    { "bind", Key_Bind_f, Key_Bind_c },
    { "unbind", Key_Unbind_f, Key_Unbind_c },
    { "unbindall", Key_Unbindall_f },
    { "bindlist", Key_Bindlist_f },

    { NULL }
};

/*
===================
Key_Init
===================
*/
void Key_Init(void)
{
    int     i;

//
// init ascii characters in console mode
//
    for (i = K_ASCIIFIRST; i <= K_ASCIILAST; i++)
        consolekeys[i] = qtrue;

#define K(x) \
    consolekeys[K_##x] = qtrue

    K(BACKSPACE);
    K(TAB);
    K(ENTER);

    K(UPARROW);
    K(DOWNARROW);
    K(LEFTARROW);
    K(RIGHTARROW);

    K(ALT);
    K(LALT);
    K(RALT);
    K(CTRL);
    K(LCTRL);
    K(RCTRL);
    K(SHIFT);
    K(LSHIFT);
    K(RSHIFT);

    K(INS);
    K(DEL);
    K(PGDN);
    K(PGUP);
    K(HOME);
    K(END);

    K(KP_HOME);
    K(KP_UPARROW);
    K(KP_PGUP);
    K(KP_LEFTARROW);
    K(KP_5);
    K(KP_RIGHTARROW);
    K(KP_END);
    K(KP_DOWNARROW);
    K(KP_PGDN);
    K(KP_ENTER);
    K(KP_INS);
    K(KP_DEL);
    K(KP_SLASH);
    K(KP_MINUS);
    K(KP_PLUS);
    K(KP_MULTIPLY);

    K(MOUSE3);

    K(MWHEELUP);
    K(MWHEELDOWN);

#undef K

#if !USE_CHAR_EVENTS
//
// init ascii keyshift characters
//
    for (i = 0; i < 256; i++)
        keyshift[i] = i;
    for (i = 'a'; i <= 'z'; i++)
        keyshift[i] = i - 'a' + 'A';

    keyshift['1'] = '!';
    keyshift['2'] = '@';
    keyshift['3'] = '#';
    keyshift['4'] = '$';
    keyshift['5'] = '%';
    keyshift['6'] = '^';
    keyshift['7'] = '&';
    keyshift['8'] = '*';
    keyshift['9'] = '(';
    keyshift['0'] = ')';
    keyshift['-'] = '_';
    keyshift['='] = '+';
    keyshift[','] = '<';
    keyshift['.'] = '>';
    keyshift['/'] = '?';
    keyshift[';'] = ':';
    keyshift['\''] = '"';
    keyshift['['] = '{';
    keyshift[']'] = '}';
    keyshift['`'] = '~';
    keyshift['\\'] = '|';
#endif

    menubound[K_ESCAPE] = qtrue;
    for (i = 0; i < 12; i++)
        menubound[K_F1 + i] = qtrue;

//
// register our functions
//
    Cmd_Register(c_keys);
}

/*
===================
Key_Event

Called by the system between frames for both key up and key down events
Should NOT be called during an interrupt!
===================
*/
void Key_Event(unsigned key, qboolean down, unsigned time)
{
    char    *kb;
    char    cmd[MAX_STRING_CHARS];

    if (key >= 256) {
        Com_Error(ERR_FATAL, "%s: bad key", __func__);
    }

    Com_DDDPrintf("%u: %c%s\n", time,
                  down ? '+' : '-', Key_KeynumToString(key));

    // hack for menu key binding
    if (key_wait_cb && down && !key_wait_cb(key_wait_arg, key)) {
        return;
    }

    // update auto-repeat status
    if (down) {
        key_repeats[key]++;
        if (!(cls.key_dest & (KEY_CONSOLE | KEY_MESSAGE | KEY_MENU))
            && key != K_BACKSPACE
            && key != K_PAUSE
            && key != K_ESCAPE
            && key != K_PGUP
            && key != K_KP_PGUP
            && key != K_PGDN
            && key != K_KP_PGDN
            && key_repeats[key] > 1) {
            return; // ignore most autorepeats
        }

        if (key >= K_MOUSEFIRST && !keybindings[key] && !consolekeys[key]) {
            Com_Printf("%s is unbound, hit F4 to set.\n",
                       Key_KeynumToString(key));
        }
    } else {
        key_repeats[key] = 0;
    }

    // console key is hardcoded, so the user can never unbind it
    if (!Key_IsDown(K_SHIFT) && (key == '`' || key == '~')) {
        if (down) {
            Con_ToggleConsole_f();
        }
        return;
    }

    // Alt+Enter is hardcoded for all systems
    if (Key_IsDown(K_ALT) && key == K_ENTER) {
        if (down) {
            extern void VID_ToggleFullscreen(void);

            VID_ToggleFullscreen();
        }
        return;
    }

    // menu key is hardcoded, so the user can never unbind it
    if (key == K_ESCAPE) {
        if (!down) {
            return;
        }

        if (cls.key_dest == KEY_GAME &&
            cl.frame.ps.stats[STAT_LAYOUTS] &&
            cls.demo.playback == qfalse) {
            if (key_repeats[key] == 2) {
                // force main menu if escape is held
                UI_OpenMenu(UIMENU_GAME);
            } else if (key_repeats[key] == 1) {
                // put away help computer / inventory
                CL_ClientCommand("putaway");
            }
            return;
        }

        if (key_repeats[key] > 1) {
            return;
        }

        if (cls.key_dest & KEY_CONSOLE) {
            if (cls.state < ca_active && !(cls.key_dest & KEY_MENU)) {
                UI_OpenMenu(UIMENU_MAIN);
            } else {
                Con_Close(qtrue);
            }
        } else if (cls.key_dest & KEY_MENU) {
            UI_Keydown(key);
        } else if (cls.key_dest & KEY_MESSAGE) {
            Key_Message(key);
        } else if (cls.state == ca_active) {
            UI_OpenMenu(UIMENU_GAME);
        } else {
            UI_OpenMenu(UIMENU_MAIN);
        }
        return;
    }

    // track if any key is down for BUTTON_ANY
    keydown[key] = down;
    if (down) {
        if (key_repeats[key] == 1)
            anykeydown++;
    } else {
        anykeydown--;
        if (anykeydown < 0)
            anykeydown = 0;
    }

    // hack for demo freelook in windowed mode
    if (cls.key_dest == KEY_GAME && cls.demo.playback && key == K_SHIFT) {
        IN_Activate();
    }

//
// if not a consolekey, send to the interpreter no matter what mode is
//
    if ((cls.key_dest == KEY_GAME) ||
        ((cls.key_dest & KEY_CONSOLE) && !consolekeys[key]) ||
        ((cls.key_dest & KEY_MENU) && menubound[key])) {
//
// Key up events only generate commands if the game key binding is
// a button command (leading + sign).
// Button commands include the kenum as a parameter, so multiple
// downs can be matched with ups.
//
        if (!down) {
            kb = keybindings[key];
            if (kb && kb[0] == '+') {
                Q_snprintf(cmd, sizeof(cmd), "-%s %i %i\n",
                           kb + 1, key, time);
                Cbuf_AddText(&cmd_buffer, cmd);
            }
#if !USE_CHAR_EVENTS
            if (keyshift[key] != key) {
                kb = keybindings[keyshift[key]];
                if (kb && kb[0] == '+') {
                    Q_snprintf(cmd, sizeof(cmd), "-%s %i %i\n",
                               kb + 1, key, time);
                    Cbuf_AddText(&cmd_buffer, cmd);
                }
            }
#endif
            return;
        }

        if (key_repeats[key] > 1) {
            return;
        }

#if !USE_CHAR_EVENTS
        if (Key_IsDown(K_SHIFT) && keyshift[key] != key && keybindings[keyshift[key]]) {
            key = keyshift[key];
        }
#endif

        kb = keybindings[key];
        if (kb) {
            if (kb[0] == '+') {
                // button commands add keynum and time as a parm
                Q_snprintf(cmd, sizeof(cmd), "%s %i %i\n", kb, key, time);
                Cbuf_AddText(&cmd_buffer, cmd);
            } else {
                Cbuf_AddText(&cmd_buffer, kb);
                Cbuf_AddText(&cmd_buffer, "\n");
            }
        }
        return;
    }

    if (!down)
        return;     // other subsystems only care about key down events

    if (cls.key_dest & KEY_CONSOLE) {
        Key_Console(key);
    } else if (cls.key_dest & KEY_MENU) {
        UI_Keydown(key);
    } else if (cls.key_dest & KEY_MESSAGE) {
        Key_Message(key);
    }

#if !USE_CHAR_EVENTS

    if (Key_IsDown(K_CTRL) || Key_IsDown(K_ALT)) {
        return;
    }

    switch (key) {
    case K_KP_SLASH:
        key = '/';
        break;
    case K_KP_MULTIPLY:
        key = '*';
        break;
    case K_KP_MINUS:
        key = '-';
        break;
    case K_KP_PLUS:
        key = '+';
        break;
    case K_KP_HOME:
        key = '7';
        break;
    case K_KP_UPARROW:
        key = '8';
        break;
    case K_KP_PGUP:
        key = '9';
        break;
    case K_KP_LEFTARROW:
        key = '4';
        break;
    case K_KP_5:
        key = '5';
        break;
    case K_KP_RIGHTARROW:
        key = '6';
        break;
    case K_KP_END:
        key = '1';
        break;
    case K_KP_DOWNARROW:
        key = '2';
        break;
    case K_KP_PGDN:
        key = '3';
        break;
    case K_KP_INS:
        key = '0';
        break;
    case K_KP_DEL:
        key = '.';
        break;
    }

    // if key is printable, generate char events
    if (key < 32 || key >= 127) {
        return;
    }

    if (Key_IsDown(K_SHIFT)) {
        key = keyshift[key];
    }

    if (cls.key_dest & KEY_CONSOLE) {
        Char_Console(key);
    } else if (cls.key_dest & KEY_MENU) {
        UI_CharEvent(key);
    } else if (cls.key_dest & KEY_MESSAGE) {
        Char_Message(key);
    }

#endif // !USE_CHAR_EVENTS

}

#if USE_CHAR_EVENTS

/*
===================
Key_CharEvent
===================
*/
void Key_CharEvent(int key)
{
    if (key == '`' || key == '~') {
        return;
    }

    if (cls.key_dest & KEY_CONSOLE) {
        Char_Console(key);
    } else if (cls.key_dest & KEY_MENU) {
        UI_CharEvent(key);
    } else if (cls.key_dest & KEY_MESSAGE) {
        Char_Message(key);
    }
}

#endif

/*
===================
Key_ClearStates
===================
*/
void Key_ClearStates(void)
{
    int     i;

    for (i = 0; i < 256; i++) {
        if (keydown[i] || key_repeats[i])
            Key_Event(i, qfalse, 0);
        keydown[i] = 0;
        key_repeats[i] = 0;
    }

    anykeydown = 0;
}

/*
===================
Key_WaitKey
===================
*/
void Key_WaitKey(keywaitcb_t wait, void *arg)
{
    key_wait_cb = wait;
    key_wait_arg = arg;
}

