#define _XOPEN_SOURCE 600
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <termios.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ioctl.h>

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#define MAX_SNAME 1024
#define FDUDATA_METATABLE "fdudata_metatable"
#define TERMUDATA_METATABLE "termudata_metatable"

#define DPRINT(SD, FMT, ...) \
    if((SD)->dflag) fprintf(stderr, "%20s,%4d|pid=%6ds,ppid=%6d,fd=%4d| " FMT "\n", __func__, __LINE__, (SD)->pid, (SD)->ppid, (SD)->fd, __VA_ARGS__);

#define DPRINT2(FMT, ...) \
    fprintf(stderr, "%20s,%4d|" FMT "\n", __func__, __LINE__, __VA_ARGS__);

typedef struct _fddata {
    pid_t pid;
    pid_t ppid;
    int fd;
    int dflag;
} FDDATA;

typedef struct _termdata{
    pid_t pid;
    int fd;
    struct termios orig;
} TERMDATA;

int ptyMOpen(char *sname, size_t snlen)
{
    int mfd;
    char *pn;
    int errnobk;

    mfd = posix_openpt(O_RDWR | O_NOCTTY);
    if( mfd < 0 )
    {
        errnobk = errno;
        DPRINT2( "posix_openpt: %s", strerror(errno));
        errno = errnobk;
        return -1;
    }

    if( (grantpt(mfd) < 0) || (unlockpt(mfd) < 0) )
    {
        errnobk = errno;
        DPRINT2("grantpt/unlockpt: %s", strerror(errno));
        close(mfd);
        errno = errnobk;
        return -1;
    }

    pn = ptsname(mfd);
    if( strlen(pn) >= snlen )
    {
        close(mfd);
        errno = EOVERFLOW;
        DPRINT2("ptsname: name size overflow%s", "");
        return -1;
    }
    strncpy(sname, pn, snlen);
    return mfd;
}

void ttyRaw(lua_State *L, int fd)
{

    TERMDATA *data;
    struct termios ti;
    struct termios orig;

    if( tcgetattr(fd, &ti) < 0 )
    {
        return;
    }

    orig = ti;

    ti.c_lflag &= ~(ICANON|ISIG|IEXTEN|ECHO);
    ti.c_iflag &= ~(BRKINT|ICRNL|IGNBRK|IGNCR|INLCR|INPCK|ISTRIP|IXON|PARMRK);
    ti.c_oflag &= ~OPOST;
    ti.c_cc[VMIN] = 1;
    ti.c_cc[VTIME] = 0;
    if( tcsetattr(fd, TCSAFLUSH, &ti) < 0 )
    {
        return;
    }

    /* backup the parent's termios original mode. restore the 
     * original mode on exit. check _termdatagc */
    lua_getfield(L, LUA_REGISTRYINDEX, "CLEARTERMIDX");
    if( lua_isnil(L, -1) )
    {
        lua_pop(L, 1);
        data = (TERMDATA *)lua_newuserdata(L, sizeof(TERMDATA));
        data->pid = -1;
        luaL_setmetatable(L, TERMUDATA_METATABLE);
    }
    else
    {
        data = (TERMDATA *)luaL_checkudata(L, -1, TERMUDATA_METATABLE);
    }

    if( data->pid != getpid() )
    {
        data->fd = fd;
        data->pid = getpid();
        data->orig = orig;
    }
    lua_setfield(L, LUA_REGISTRYINDEX, "CLEARTERMIDX");
}

void newFDDATA(lua_State *L, int fd)
{
    FDDATA *data = (FDDATA *)lua_newuserdata(L, sizeof(FDDATA));
    data->pid = getpid();
    data->ppid = getppid();
    data->fd = fd;
    data->dflag = 0;
    luaL_setmetatable(L, FDUDATA_METATABLE);
}

static int ptyFork(lua_State *L)
{
    int mfd,sfd;
    pid_t cpid;
    char name[MAX_SNAME];

    struct termios stermios;
    struct winsize ws;
        
    if( tcgetattr(STDIN_FILENO, &stermios) < 0 )
    {
        lua_pushinteger(L, -1);
        lua_pushinteger(L, errno);
        DPRINT2("tcgetattr: %s", strerror(errno));
        return 2;
    }

    if( ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) < 0 )
    {
        lua_pushinteger(L, -1);
        lua_pushinteger(L, errno);
        DPRINT2("ioctl: %s", strerror(errno));
        return 2;
    }

    mfd = ptyMOpen(name, MAX_SNAME);
    if( mfd < 0 )
    {
        lua_pushinteger(L, -1);
        lua_pushinteger(L, errno);
        return 2;
    }

    cpid = fork();
    if( cpid < 0 )
    {
        lua_pushinteger(L, -1);
        lua_pushinteger(L, errno);
        close(mfd);
        DPRINT2("fork: %s", strerror(errno));
        return 2;
    }

    if( cpid > 0)
    {
        ttyRaw(L, STDIN_FILENO);
        lua_pushinteger(L, cpid);
        newFDDATA(L, mfd);
        newFDDATA(L, STDIN_FILENO);
        newFDDATA(L, STDOUT_FILENO);
        newFDDATA(L, STDERR_FILENO);
        return 5;
    }

    lua_pushinteger(L, 0);
    if( setsid() < 0 )
    {
        lua_pushnil(L);
        lua_pushinteger(L, errno);
        DPRINT2("setsid: %s", strerror(errno));
        return 3;
    }

    close(mfd);
    sfd = open(name, O_RDWR);
    if( sfd < 0 )
    {
        lua_pushnil(L);
        lua_pushinteger(L, errno);
        DPRINT2("open: %s", strerror(errno));
        return 3;
    }

#ifdef TIOCSCTTY
    if( ioctl(sfd, TIOCSCTTY, 0) < 0 )
    {
        lua_pushnil(L);
        lua_pushinteger(L, errno);
        DPRINT2("ioctl: %s", strerror(errno));
        return 3;
    }
#endif

    if( (tcsetattr(sfd, TCSANOW, &stermios) < 0) )
    {
        lua_pushnil(L);
        lua_pushinteger(L, errno);
        DPRINT2("tcsetattr: %s", strerror(errno));
        return 3;
    }
    if( ioctl(sfd, TIOCSWINSZ, &ws) < 0 )
    {
        lua_pushnil(L);
        lua_pushinteger(L, errno);
        DPRINT2("ioctl: %s", strerror(errno));
        return 3;
    }
    if( (dup2(sfd, STDIN_FILENO) != STDIN_FILENO) ||
        (dup2(sfd, STDOUT_FILENO) != STDOUT_FILENO) ||
        (dup2(sfd, STDERR_FILENO) != STDERR_FILENO) )
    {
        lua_pushnil(L);
        lua_pushinteger(L, errno);
        DPRINT2("dup2: %s", strerror(errno));
        return 3;
    }
    close(sfd);

    newFDDATA(L, STDIN_FILENO);
    newFDDATA(L, STDOUT_FILENO);
    newFDDATA(L, STDERR_FILENO);

    return 4;
}

static int _exec(lua_State *L)
{
    int idx;
    int optnum = 0;
    const char *path = luaL_checkstring(L, 1);
    if( !(lua_isnil(L, 2) || lua_isnone(L, 2)) )
    {
        luaL_checktype(L, 2, LUA_TTABLE);
        optnum = lua_rawlen(L, 2);
    }

    const char * argv[optnum+2];
    argv[0] = path;

    for( idx = 1; idx <= optnum; idx++ )
    {
        lua_rawgeti(L, 2, idx);
        argv[idx] = luaL_checkstring(L, -1);
    }
    argv[optnum +1] = NULL;
    if( execv(path,(char **) argv) < 0 )
    {
        lua_pushnil(L);
        lua_pushinteger(L, errno);
    }
    return 0;
}

static int _read(lua_State *L)
{
    FDDATA *data = (FDDATA *)luaL_checkudata(L, 1, FDUDATA_METATABLE);
    size_t len = (lua_Unsigned)luaL_checkinteger(L, 2);
    char buffer[len];

    int rd = read(data->fd, buffer, len);
    if( rd < 0 )
    {
        lua_pushnil(L);
        lua_pushinteger(L, errno);
        return 2;
    }

    lua_pushlstring(L, buffer, rd);
    return 1;
}

static int _write(lua_State *L)
{
    size_t in_len;
    FDDATA *data = (FDDATA *)luaL_checkudata(L, 1, FDUDATA_METATABLE);
    const char *buffer = luaL_checklstring(L, 2, &in_len);
    int s_len = luaL_optinteger(L, 3, in_len);
    if( s_len > in_len )
    {
        s_len = in_len;
    }
    int wr = write(data->fd, buffer, s_len);
    if( wr < 0 )
    {
        lua_pushnil(L);
        lua_pushinteger(L, errno);
        return 2;
    }

    lua_pushinteger(L, wr);
    return 1;
}

static int _close(lua_State *L)
{
    FDDATA *data = (FDDATA *)luaL_checkudata(L, 1, FDUDATA_METATABLE);
    if( data->fd >= 0 )
    {
        close(data->fd);
        data->fd = -1;
    }
    return 0;
}

static int _fd(lua_State *L)
{
    FDDATA *data = (FDDATA *)luaL_checkudata(L, 1, FDUDATA_METATABLE);
    lua_pushinteger(L, data->fd);
    return 1;
}

static int _termdatagc(lua_State *L)
{
    TERMDATA *data = (TERMDATA *)luaL_checkudata(L, 1, TERMUDATA_METATABLE);
    DPRINT2("pid = %d, mypid = %d, fd = %d", data->pid, getpid(), data->fd);
    if( getpid() == data->pid )
    {
        tcsetattr(data->fd, TCSANOW, &data->orig);
    }
    return 0;
}

static const luaL_Reg fdmethods[] =
{
    {"read", _read},
    {"write", _write},
    {"close", _close},
    {"fd", _fd},
    {NULL,NULL}
};

static const luaL_Reg ptymethods[] =
{
    {"ptyFork", ptyFork},
    {"exec", _exec},
    {NULL,NULL}
};

int luaopen_pty(lua_State *L)
{
    luaL_checkversion(L);
    luaL_newmetatable(L, FDUDATA_METATABLE);
    luaL_newlib(L, fdmethods);
    lua_setfield(L, -2, "__index");

    luaL_newmetatable(L, TERMUDATA_METATABLE);
    lua_pushcfunction(L, _termdatagc);
    lua_setfield(L, -2, "__gc");

    luaL_newlib(L, ptymethods);
    return 1;
} 
