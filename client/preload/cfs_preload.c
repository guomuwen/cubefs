/*
 * CubeFS LD_PRELOAD 拦截层
 *
 * 通过 LD_PRELOAD 拦截 POSIX 文件操作，将 CubeFS 路径下的 I/O
 * 透明转发到 libcfs.so（gosdk），非 CubeFS 路径透传到真实 libc。
 *
 * 用法：
 *   LD_PRELOAD=libcfs_preload.so \
 *   CFS_MASTER="ip:port,ip:port" CFS_VOL="volname" \
 *   CFS_AK="accesskey" CFS_SK="secretkey" \
 *   CFS_MOUNT_POINT="/mnt/cfs/" \
 *   CFS_BCACHE_DIRS="/bcache0;/bcache1" \
 *   <your_program>
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <limits.h>

/* ========================================================================
 * Constants
 * ======================================================================== */

#define FD_OFFSET       100000
#define MAX_CFS_FDS     65536
#define CFS_PATH_MAX    4096

/* ========================================================================
 * libcfs.so 类型定义（与 libcfs.h 对齐）
 * ======================================================================== */

struct cfs_stat_info {
    uint64_t ino;
    uint64_t size;
    uint64_t blocks;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t atime_nsec;
    uint32_t mtime_nsec;
    uint32_t ctime_nsec;
    mode_t   mode;
    uint32_t nlink;
    uint32_t blk_size;
    uint32_t uid;
    uint32_t gid;
};

struct cfs_dirent {
    uint64_t ino;
    char     name[256];
    char     d_type;
    uint32_t nameLen;
};

typedef long long GoInt64;
typedef GoInt64 GoInt;
typedef unsigned long long GoUint64;
typedef unsigned char GoUint8;
typedef struct { void *data; GoInt len; GoInt cap; } GoSlice;

/* ========================================================================
 * libcfs.so 函数指针类型
 * ======================================================================== */

typedef int64_t (*fn_cfs_new_client)(void);
typedef int     (*fn_cfs_set_client)(int64_t id, char *key, char *val);
typedef int     (*fn_cfs_start_client)(int64_t id);
typedef void    (*fn_cfs_close_client)(int64_t id);
typedef int     (*fn_cfs_open)(int64_t id, char *path, int flags, mode_t mode);
typedef ssize_t (*fn_cfs_read)(int64_t id, int fd, void *buf, size_t size, off_t off);
typedef void    (*fn_cfs_close)(int64_t id, int fd);
typedef int     (*fn_cfs_getattr)(int64_t id, char *path, struct cfs_stat_info *stat);
typedef int     (*fn_cfs_readdir)(int64_t id, int fd, GoSlice dirents, int count);

/* ========================================================================
 * 真实 libc 函数指针类型
 * ======================================================================== */

typedef int     (*fn_open)(const char *, int, ...);
typedef int     (*fn_open64)(const char *, int, ...);
typedef int     (*fn_openat)(int, const char *, int, ...);
typedef ssize_t (*fn_read)(int, void *, size_t);
typedef ssize_t (*fn_pread)(int, void *, size_t, off_t);
typedef ssize_t (*fn_pread64)(int, void *, size_t, off_t);
typedef int     (*fn_close)(int);
typedef int     (*fn_stat)(const char *, struct stat *);
typedef int     (*fn_lstat)(const char *, struct stat *);
typedef int     (*fn_fstat)(int, struct stat *);
typedef int     (*fn_xstat)(int, const char *, struct stat *);
typedef int     (*fn_fxstat)(int, int, struct stat *);
typedef int     (*fn_lxstat)(int, const char *, struct stat *);
typedef off_t   (*fn_lseek)(int, off_t, int);
typedef off_t   (*fn_lseek64)(int, off_t, int);
typedef DIR *   (*fn_opendir)(const char *);
typedef DIR *   (*fn_fdopendir)(int);
typedef struct dirent *(*fn_readdir)(DIR *);
typedef struct dirent64 *(*fn_readdir64)(DIR *);
typedef int     (*fn_closedir)(DIR *);
typedef ssize_t (*fn_getdents64)(int, void *, size_t);
typedef int     (*fn_fcntl)(int, int, ...);
typedef int     (*fn_fstatat)(int, const char *, struct stat *, int);
typedef int     (*fn_fstatat64)(int, const char *, struct stat *, int);
typedef int     (*fn_fstat64)(int, struct stat *);
typedef int     (*fn_stat64)(const char *, struct stat *);
typedef int     (*fn_lstat64)(const char *, struct stat *);

/* linux_dirent64 结构体 (用于 getdents64) */
struct linux_dirent64 {
    uint64_t       d_ino;
    int64_t        d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[];
};

/* ========================================================================
 * 全局状态
 * ======================================================================== */

/* fd 映射表条目 */
typedef struct {
    int      cfs_fd;                   /* libcfs 返回的 fd */
    off_t    offset;                   /* 当前文件偏移（read 使用） */
    char     path[CFS_PATH_MAX];       /* 文件路径（fstat 反查用） */
    int      valid;                    /* 是否有效 */
    int      is_dir;                   /* 是否是目录 fd */
    struct cfs_dirent *dir_entries;    /* 目录项缓存（getdents64 用）*/
    int      dir_count;               /* 目录项总数 */
    int      dir_pos;                 /* 当前读取位置 */
} fd_entry_t;

/* 目录句柄封装 */
#define CFS_DIR_MAGIC 0x43465344  /* "CFSD" */
typedef struct {
    uint32_t         magic;            /* 魔数，用于区分 cfs_dir_t 和系统 DIR* */
    int              cfs_fd;           /* cfs 目录 fd */
    struct cfs_dirent *entries;        /* 缓存的目录项 */
    int              count;            /* 总目录项数 */
    int              pos;              /* 当前读取位置 */
    struct dirent    dent;             /* 返回给调用方的 dirent */
} cfs_dir_t;

static inline int is_cfs_dir(DIR *dirp) {
    cfs_dir_t *dir = (cfs_dir_t *)dirp;
    return dir && dir->magic == CFS_DIR_MAGIC;
}

static int           g_preload_enabled = 0;
static int64_t       g_client_id       = -1;
static char          g_mount_point[CFS_PATH_MAX] = "";
static size_t        g_mount_point_len = 0;

static fd_entry_t    g_fd_table[MAX_CFS_FDS];
static pthread_rwlock_t g_fd_lock = PTHREAD_RWLOCK_INITIALIZER;

/* libcfs.so 句柄和函数指针 */
static void *g_libcfs_handle = NULL;

static fn_cfs_new_client   p_cfs_new_client   = NULL;
static fn_cfs_set_client   p_cfs_set_client   = NULL;
static fn_cfs_start_client p_cfs_start_client = NULL;
static fn_cfs_close_client p_cfs_close_client = NULL;
static fn_cfs_open         p_cfs_open         = NULL;
static fn_cfs_read         p_cfs_read         = NULL;
static fn_cfs_close        p_cfs_close        = NULL;
static fn_cfs_getattr      p_cfs_getattr      = NULL;
static fn_cfs_readdir      p_cfs_readdir      = NULL;

/* 真实 libc 函数指针 */
static fn_open     real_open     = NULL;
static fn_open64   real_open64   = NULL;
static fn_openat   real_openat   = NULL;
static fn_read     real_read     = NULL;
static fn_pread    real_pread    = NULL;
static fn_pread64  real_pread64  = NULL;
static fn_close    real_close    = NULL;
static fn_stat     real_stat     = NULL;
static fn_lstat    real_lstat    = NULL;
static fn_fstat    real_fstat    = NULL;
static fn_xstat    real_xstat    = NULL;
static fn_fxstat   real_fxstat   = NULL;
static fn_lxstat   real_lxstat   = NULL;
static fn_lseek    real_lseek    = NULL;
static fn_lseek64  real_lseek64  = NULL;
static fn_opendir  real_opendir  = NULL;
static fn_readdir  real_readdir  = NULL;
static fn_readdir64 real_readdir64 = NULL;
static fn_closedir real_closedir = NULL;
static fn_fdopendir real_fdopendir = NULL;
static fn_getdents64 real_getdents64 = NULL;
static fn_fcntl  real_fcntl  = NULL;
static fn_fstatat real_fstatat = NULL;
static fn_fstatat64 real_fstatat64 = NULL;
static fn_fstat64 real_fstat64 = NULL;
static fn_stat64 real_stat64 = NULL;
static fn_lstat64 real_lstat64 = NULL;

/* ========================================================================
 * 辅助函数
 * ======================================================================== */

#define PRELOAD_LOG(fmt, ...) \
    fprintf(stderr, "[cfs_preload] " fmt "\n", ##__VA_ARGS__)

/* 检查路径是否匹配 CubeFS 挂载点前缀 */
static inline int is_cfs_path(const char *path) {
    if (!path || !g_preload_enabled || g_mount_point_len == 0) return 0;
    return strncmp(path, g_mount_point, g_mount_point_len) == 0;
}

/* 从完整路径中提取 CubeFS 内部路径 */
static inline const char *get_cfs_path(const char *path) {
    /* /mnt/cfs/mlperf_data/... → /mlperf_data/... */
    const char *p = path + g_mount_point_len;
    if (*(p - 1) == '/') p--;  /* 保留开头的 / */
    return p;
}

/* 判断 fd 是否是 CubeFS fd */
static inline int is_cfs_fd(int fd) {
    if (!g_preload_enabled || fd < FD_OFFSET) return 0;
    int idx = fd - FD_OFFSET;
    return idx >= 0 && idx < MAX_CFS_FDS && g_fd_table[idx].valid;
}

/* 分配 fd 映射表条目 */
static int alloc_fd_entry(int cfs_fd, const char *path) {
    if (cfs_fd < 0 || cfs_fd >= MAX_CFS_FDS) return -1;
    pthread_rwlock_wrlock(&g_fd_lock);
    g_fd_table[cfs_fd].cfs_fd = cfs_fd;
    g_fd_table[cfs_fd].offset = 0;
    g_fd_table[cfs_fd].valid  = 1;
    strncpy(g_fd_table[cfs_fd].path, path, CFS_PATH_MAX - 1);
    g_fd_table[cfs_fd].path[CFS_PATH_MAX - 1] = '\0';
    pthread_rwlock_unlock(&g_fd_lock);
    return cfs_fd + FD_OFFSET;
}

/* 释放 fd 映射表条目 */
static void free_fd_entry(int idx) {
    if (idx >= 0 && idx < MAX_CFS_FDS) {
        if (g_fd_table[idx].dir_entries) {
            free(g_fd_table[idx].dir_entries);
            g_fd_table[idx].dir_entries = NULL;
        }
        g_fd_table[idx].valid = 0;
        g_fd_table[idx].is_dir = 0;
        g_fd_table[idx].cfs_fd = -1;
        g_fd_table[idx].offset = 0;
        g_fd_table[idx].dir_count = 0;
        g_fd_table[idx].dir_pos = 0;
        g_fd_table[idx].path[0] = '\0';
    }
}

/* cfs_stat_info 转换为 struct stat */
static void cfs_stat_to_stat(const struct cfs_stat_info *cfs_st, struct stat *st) {
    memset(st, 0, sizeof(*st));
    st->st_ino     = cfs_st->ino;
    st->st_mode    = cfs_st->mode;
    st->st_nlink   = cfs_st->nlink;
    st->st_uid     = cfs_st->uid;
    st->st_gid     = cfs_st->gid;
    st->st_size    = (off_t)cfs_st->size;
    st->st_blocks  = (blkcnt_t)cfs_st->blocks;
    st->st_blksize = cfs_st->blk_size ? cfs_st->blk_size : 4096;
    st->st_atim.tv_sec  = cfs_st->atime;
    st->st_atim.tv_nsec = cfs_st->atime_nsec;
    st->st_mtim.tv_sec  = cfs_st->mtime;
    st->st_mtim.tv_nsec = cfs_st->mtime_nsec;
    st->st_ctim.tv_sec  = cfs_st->ctime;
    st->st_ctim.tv_nsec = cfs_st->ctime_nsec;
}

/* ========================================================================
 * Constructor / Destructor
 * ======================================================================== */

static void load_real_functions(void) {
    real_open     = (fn_open)    dlsym(RTLD_NEXT, "open");
    real_open64   = (fn_open64)  dlsym(RTLD_NEXT, "open64");
    real_openat   = (fn_openat)  dlsym(RTLD_NEXT, "openat");
    real_read     = (fn_read)    dlsym(RTLD_NEXT, "read");
    real_pread    = (fn_pread)   dlsym(RTLD_NEXT, "pread");
    real_pread64  = (fn_pread64) dlsym(RTLD_NEXT, "pread64");
    real_close    = (fn_close)   dlsym(RTLD_NEXT, "close");
    real_stat     = (fn_stat)    dlsym(RTLD_NEXT, "stat");
    real_lstat    = (fn_lstat)   dlsym(RTLD_NEXT, "lstat");
    real_fstat    = (fn_fstat)   dlsym(RTLD_NEXT, "fstat");
    real_xstat    = (fn_xstat)   dlsym(RTLD_NEXT, "__xstat");
    real_fxstat   = (fn_fxstat)  dlsym(RTLD_NEXT, "__fxstat");
    real_lxstat   = (fn_lxstat)  dlsym(RTLD_NEXT, "__lxstat");
    real_lseek    = (fn_lseek)   dlsym(RTLD_NEXT, "lseek");
    real_lseek64  = (fn_lseek64) dlsym(RTLD_NEXT, "lseek64");
    real_opendir  = (fn_opendir) dlsym(RTLD_NEXT, "opendir");
    real_readdir  = (fn_readdir) dlsym(RTLD_NEXT, "readdir");
    real_readdir64 = (fn_readdir64) dlsym(RTLD_NEXT, "readdir64");
    real_closedir = (fn_closedir)dlsym(RTLD_NEXT, "closedir");
    real_fdopendir = (fn_fdopendir)dlsym(RTLD_NEXT, "fdopendir");
    real_getdents64 = (fn_getdents64)dlsym(RTLD_NEXT, "getdents64");
    real_fcntl = (fn_fcntl)dlsym(RTLD_NEXT, "fcntl");
    real_fstatat = (fn_fstatat)dlsym(RTLD_NEXT, "fstatat");
    real_fstatat64 = (fn_fstatat64)dlsym(RTLD_NEXT, "fstatat64");
    real_fstat64 = (fn_fstat64)dlsym(RTLD_NEXT, "fstat64");
    real_stat64 = (fn_stat64)dlsym(RTLD_NEXT, "stat64");
    real_lstat64 = (fn_lstat64)dlsym(RTLD_NEXT, "lstat64");
}

static int load_libcfs(const char *so_path) {
    g_libcfs_handle = dlopen(so_path, RTLD_NOW | RTLD_GLOBAL);
    if (!g_libcfs_handle) {
        PRELOAD_LOG("ERROR: dlopen(%s) failed: %s", so_path, dlerror());
        return -1;
    }

    #define LOAD_SYM(name) do { \
        p_##name = (fn_##name)dlsym(g_libcfs_handle, #name); \
        if (!p_##name) { \
            PRELOAD_LOG("ERROR: dlsym(%s) failed: %s", #name, dlerror()); \
            return -1; \
        } \
    } while (0)

    LOAD_SYM(cfs_new_client);
    LOAD_SYM(cfs_set_client);
    LOAD_SYM(cfs_start_client);
    LOAD_SYM(cfs_close_client);
    LOAD_SYM(cfs_open);
    LOAD_SYM(cfs_read);
    LOAD_SYM(cfs_close);
    LOAD_SYM(cfs_getattr);
    LOAD_SYM(cfs_readdir);
    #undef LOAD_SYM

    return 0;
}

static int set_client_env(int64_t id, const char *key, const char *env_name) {
    const char *val = getenv(env_name);
    if (!val) return 0;  /* optional */
    return p_cfs_set_client(id, (char *)key, (char *)val);
}

__attribute__((constructor))
static void cfs_preload_init(void) {
    /* 1. 保存真实 libc 函数指针 */
    load_real_functions();

    /* 初始化 fd 表 */
    memset(g_fd_table, 0, sizeof(g_fd_table));
    for (int i = 0; i < MAX_CFS_FDS; i++) {
        g_fd_table[i].cfs_fd = -1;
    }

    /* 2. 检查禁用开关 */
    const char *disable = getenv("CFS_PRELOAD_DISABLE");
    if (disable && strcmp(disable, "1") == 0) {
        PRELOAD_LOG("Preload disabled by CFS_PRELOAD_DISABLE=1");
        return;
    }

    /* 3. 检查必要环境变量 */
    const char *master = getenv("CFS_MASTER");
    const char *vol    = getenv("CFS_VOL");
    const char *mount  = getenv("CFS_MOUNT_POINT");

    if (!master || !vol || !mount) {
        PRELOAD_LOG("WARN: Missing CFS_MASTER/CFS_VOL/CFS_MOUNT_POINT, preload disabled");
        return;
    }

    /* 保存挂载点前缀 */
    strncpy(g_mount_point, mount, CFS_PATH_MAX - 1);
    g_mount_point_len = strlen(g_mount_point);
    /* 确保挂载点以 / 结尾 */
    if (g_mount_point_len > 0 && g_mount_point[g_mount_point_len - 1] != '/') {
        g_mount_point[g_mount_point_len] = '/';
        g_mount_point_len++;
        g_mount_point[g_mount_point_len] = '\0';
    }

    /* 4. 加载 libcfs.so */
    const char *so_path = getenv("CFS_LIBCFS_SO");
    if (!so_path) so_path = "libcfs.so";
    if (load_libcfs(so_path) != 0) {
        PRELOAD_LOG("ERROR: Failed to load libcfs.so, preload disabled");
        return;
    }

    /* 5. 初始化 CubeFS client */
    g_client_id = p_cfs_new_client();
    if (g_client_id < 0) {
        PRELOAD_LOG("ERROR: cfs_new_client failed");
        return;
    }

    p_cfs_set_client(g_client_id, "masterAddr", (char *)master);
    p_cfs_set_client(g_client_id, "volName",    (char *)vol);

    set_client_env(g_client_id, "accessKey",  "CFS_AK");
    set_client_env(g_client_id, "secretKey",  "CFS_SK");
    set_client_env(g_client_id, "logDir",     "CFS_LOG_DIR");
    set_client_env(g_client_id, "logLevel",   "CFS_LOG_LEVEL");
    set_client_env(g_client_id, "bcacheDirs", "CFS_BCACHE_DIRS");

    /* 启用 nearRead；bcache 通过环境变量控制 */
    p_cfs_set_client(g_client_id, "nearRead",       "true");
    const char *bcache_env = getenv("CFS_BCACHE_DIRS");
    p_cfs_set_client(g_client_id, "enableBcache",   (bcache_env && bcache_env[0]) ? "true" : "false");
    p_cfs_set_client(g_client_id, "readBlockThread", "2");
    p_cfs_set_client(g_client_id, "writeBlockThread","2");

    int ret = p_cfs_start_client(g_client_id);
    if (ret != 0) {
        PRELOAD_LOG("ERROR: cfs_start_client failed: %d", ret);
        p_cfs_close_client(g_client_id);
        g_client_id = -1;
        return;
    }

    g_preload_enabled = 1;
    PRELOAD_LOG("Initialized: vol=%s mount=%s", vol, g_mount_point);
}

__attribute__((destructor))
static void cfs_preload_fini(void) {
    if (!g_preload_enabled) return;
    g_preload_enabled = 0;

    /* 关闭所有未关闭的 cfs fd */
    for (int i = 0; i < MAX_CFS_FDS; i++) {
        if (g_fd_table[i].valid && p_cfs_close) {
            p_cfs_close(g_client_id, g_fd_table[i].cfs_fd);
            g_fd_table[i].valid = 0;
        }
    }

    if (g_client_id >= 0 && p_cfs_close_client) {
        p_cfs_close_client(g_client_id);
        g_client_id = -1;
    }

    if (g_libcfs_handle) {
        dlclose(g_libcfs_handle);
        g_libcfs_handle = NULL;
    }

    PRELOAD_LOG("Finalized");
}

/* ========================================================================
 * 拦截函数：open / open64 / openat
 * ======================================================================== */

int open(const char *pathname, int flags, ...) {
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }

    if (is_cfs_path(pathname)) {
        const char *cfs_path = get_cfs_path(pathname);
        int cfs_fd = p_cfs_open(g_client_id, (char *)cfs_path, flags, mode);
        if (cfs_fd < 0) {
            errno = -cfs_fd;
            return -1;
        }
        int ret_fd = alloc_fd_entry(cfs_fd, cfs_path);
        if (ret_fd >= FD_OFFSET && (flags & O_DIRECTORY)) {
            g_fd_table[ret_fd - FD_OFFSET].is_dir = 1;
        }
        return ret_fd;
    }

    return real_open(pathname, flags, mode);
}

int open64(const char *pathname, int flags, ...) {
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }

    if (is_cfs_path(pathname)) {
        const char *cfs_path = get_cfs_path(pathname);
        int cfs_fd = p_cfs_open(g_client_id, (char *)cfs_path, flags, mode);
        if (cfs_fd < 0) {
            errno = -cfs_fd;
            return -1;
        }
        int ret_fd = alloc_fd_entry(cfs_fd, cfs_path);
        if (ret_fd >= FD_OFFSET && (flags & O_DIRECTORY)) {
            g_fd_table[ret_fd - FD_OFFSET].is_dir = 1;
        }
        return ret_fd;
    }

    return real_open64(pathname, flags, mode);
}

int openat(int dirfd, const char *pathname, int flags, ...) {
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }

    /* 只处理绝对路径的 CubeFS 路径 */
    if (pathname && pathname[0] == '/' && is_cfs_path(pathname)) {
        const char *cfs_path = get_cfs_path(pathname);
        int cfs_fd = p_cfs_open(g_client_id, (char *)cfs_path, flags, mode);
        if (cfs_fd < 0) {
            errno = -cfs_fd;
            return -1;
        }
        int ret_fd = alloc_fd_entry(cfs_fd, cfs_path);
        /* 标记目录 fd，预加载目录项（供 getdents64 使用）*/
        if (ret_fd >= FD_OFFSET && (flags & O_DIRECTORY)) {
            int idx = ret_fd - FD_OFFSET;
            g_fd_table[idx].is_dir = 1;
            /* 延迟加载目录项：getdents64 调用时再读 */
        }
        return ret_fd;
    }

    return real_openat(dirfd, pathname, flags, mode);
}

/* openat64 在 glibc 64-bit 系统上等同 openat，但 CPython 可能显式链接 */
int openat64(int dirfd, const char *pathname, int flags, ...) {
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }

    if (pathname && pathname[0] == '/' && is_cfs_path(pathname)) {
        const char *cfs_path = get_cfs_path(pathname);
        int cfs_fd = p_cfs_open(g_client_id, (char *)cfs_path, flags, mode);
        if (cfs_fd < 0) {
            errno = -cfs_fd;
            return -1;
        }
        int ret_fd = alloc_fd_entry(cfs_fd, cfs_path);
        if (ret_fd >= FD_OFFSET && (flags & O_DIRECTORY)) {
            int idx = ret_fd - FD_OFFSET;
            g_fd_table[idx].is_dir = 1;
        }
        return ret_fd;
    }

    return real_openat(dirfd, pathname, flags, mode);
}

/* ========================================================================
 * 拦截函数：read / pread / pread64
 * ======================================================================== */

ssize_t read(int fd, void *buf, size_t count) {
    if (is_cfs_fd(fd)) {
        int idx = fd - FD_OFFSET;
        pthread_rwlock_rdlock(&g_fd_lock);
        int cfs_fd = g_fd_table[idx].cfs_fd;
        off_t off  = g_fd_table[idx].offset;
        pthread_rwlock_unlock(&g_fd_lock);

        ssize_t n = p_cfs_read(g_client_id, cfs_fd, buf, count, off);
        if (n > 0) {
            __atomic_add_fetch(&g_fd_table[idx].offset, n, __ATOMIC_SEQ_CST);
        }
        if (n < 0) { errno = -n; return -1; }
        return n;
    }
    return real_read(fd, buf, count);
}

ssize_t pread(int fd, void *buf, size_t count, off_t offset) {
    if (is_cfs_fd(fd)) {
        int idx = fd - FD_OFFSET;
        pthread_rwlock_rdlock(&g_fd_lock);
        int cfs_fd = g_fd_table[idx].cfs_fd;
        pthread_rwlock_unlock(&g_fd_lock);

        ssize_t n = p_cfs_read(g_client_id, cfs_fd, buf, count, offset);
        if (n < 0) { errno = -n; return -1; }
        return n;
    }
    return real_pread(fd, buf, count, offset);
}

ssize_t pread64(int fd, void *buf, size_t count, off_t offset) {
    if (is_cfs_fd(fd)) {
        int idx = fd - FD_OFFSET;
        pthread_rwlock_rdlock(&g_fd_lock);
        int cfs_fd = g_fd_table[idx].cfs_fd;
        pthread_rwlock_unlock(&g_fd_lock);

        ssize_t n = p_cfs_read(g_client_id, cfs_fd, buf, count, offset);
        if (n < 0) { errno = -n; return -1; }
        return n;
    }
    return real_pread64(fd, buf, count, offset);
}

/* ========================================================================
 * 拦截函数：close
 * ======================================================================== */

int close(int fd) {
    if (is_cfs_fd(fd)) {
        int idx = fd - FD_OFFSET;
        pthread_rwlock_wrlock(&g_fd_lock);
        int cfs_fd = g_fd_table[idx].cfs_fd;
        free_fd_entry(idx);
        pthread_rwlock_unlock(&g_fd_lock);

        p_cfs_close(g_client_id, cfs_fd);
        return 0;
    }
    return real_close(fd);
}

/* ========================================================================
 * 拦截函数：stat / lstat / fstat / __xstat / __fxstat / __lxstat
 * ======================================================================== */

static int do_cfs_stat(const char *path, struct stat *st) {
    struct cfs_stat_info cfs_st;
    int ret = p_cfs_getattr(g_client_id, (char *)path, &cfs_st);
    if (ret != 0) {
        errno = -ret;
        return -1;
    }
    cfs_stat_to_stat(&cfs_st, st);
    return 0;
}

int stat(const char *pathname, struct stat *st) {
    if (is_cfs_path(pathname)) {
        return do_cfs_stat(get_cfs_path(pathname), st);
    }
    if (real_stat) return real_stat(pathname, st);
    if (real_xstat) return real_xstat(1, pathname, st);
    errno = ENOSYS; return -1;
}

int lstat(const char *pathname, struct stat *st) {
    /* CubeFS 不支持符号链接，lstat 等同于 stat */
    if (is_cfs_path(pathname)) {
        return do_cfs_stat(get_cfs_path(pathname), st);
    }
    if (real_lstat) return real_lstat(pathname, st);
    if (real_lxstat) return real_lxstat(1, pathname, st);
    errno = ENOSYS; return -1;
}

int fstat(int fd, struct stat *st) {
    if (is_cfs_fd(fd)) {
        int idx = fd - FD_OFFSET;
        pthread_rwlock_rdlock(&g_fd_lock);
        char path[CFS_PATH_MAX];
        snprintf(path, CFS_PATH_MAX, "%s", g_fd_table[idx].path);
        pthread_rwlock_unlock(&g_fd_lock);
        return do_cfs_stat(path, st);
    }
    if (real_fstat) return real_fstat(fd, st);
    if (real_fxstat) return real_fxstat(1, fd, st);
    errno = ENOSYS; return -1;
}

int __xstat(int ver, const char *pathname, struct stat *st) {
    if (is_cfs_path(pathname)) {
        return do_cfs_stat(get_cfs_path(pathname), st);
    }
    if (real_xstat) return real_xstat(ver, pathname, st);
    if (real_stat) return real_stat(pathname, st);
    errno = ENOSYS; return -1;
}

int __fxstat(int ver, int fd, struct stat *st) {
    if (is_cfs_fd(fd)) {
        int idx = fd - FD_OFFSET;
        pthread_rwlock_rdlock(&g_fd_lock);
        char path[CFS_PATH_MAX];
        snprintf(path, CFS_PATH_MAX, "%s", g_fd_table[idx].path);
        pthread_rwlock_unlock(&g_fd_lock);
        return do_cfs_stat(path, st);
    }
    if (real_fxstat) return real_fxstat(ver, fd, st);
    if (real_fstat) return real_fstat(fd, st);
    errno = ENOSYS; return -1;
}

int __lxstat(int ver, const char *pathname, struct stat *st) {
    if (is_cfs_path(pathname)) {
        return do_cfs_stat(get_cfs_path(pathname), st);
    }
    if (real_lxstat) return real_lxstat(ver, pathname, st);
    if (real_lstat) return real_lstat(pathname, st);
    errno = ENOSYS; return -1;
}

/* ========================================================================
 * 拦截函数：fstatat（glibc 2.33+ 中 fstat 的底层实现）
 * ======================================================================== */

int fstatat(int dirfd, const char *pathname, struct stat *st, int flags) {
    /* Case 1: fstat(fd, &st) → fstatat(fd, "", &st, AT_EMPTY_PATH) */
    if ((flags & AT_EMPTY_PATH) && pathname && pathname[0] == '\0' &&
        g_preload_enabled && dirfd >= FD_OFFSET) {
        int idx = dirfd - FD_OFFSET;
        if (idx >= 0 && idx < MAX_CFS_FDS && g_fd_table[idx].valid) {
            pthread_rwlock_rdlock(&g_fd_lock);
            char path[CFS_PATH_MAX];
            snprintf(path, CFS_PATH_MAX, "%s", g_fd_table[idx].path);
            pthread_rwlock_unlock(&g_fd_lock);
            return do_cfs_stat(path, st);
        }
    }

    /* Case 2: stat(path) → fstatat(AT_FDCWD, path, &st, 0) */
    if (dirfd == AT_FDCWD && pathname && is_cfs_path(pathname)) {
        const char *cfs_path = get_cfs_path(pathname);
        return do_cfs_stat(cfs_path, st);
    }

    /* Case 3: lstat(path) → fstatat(AT_FDCWD, path, &st, AT_SYMLINK_NOFOLLOW) */
    if (dirfd == AT_FDCWD && pathname && (flags & AT_SYMLINK_NOFOLLOW) && is_cfs_path(pathname)) {
        const char *cfs_path = get_cfs_path(pathname);
        return do_cfs_stat(cfs_path, st);
    }

    if (real_fstatat) return real_fstatat(dirfd, pathname, st, flags);
    errno = ENOSYS; return -1;
}

int fstatat64(int dirfd, const char *__restrict pathname,
              struct stat64 *__restrict st, int flags) {
    /* Case 1: fstat64(fd) → fstatat64(fd, "", &st, AT_EMPTY_PATH) */
    if ((flags & AT_EMPTY_PATH) && pathname && pathname[0] == '\0' &&
        g_preload_enabled && dirfd >= FD_OFFSET) {
        int idx = dirfd - FD_OFFSET;
        if (idx >= 0 && idx < MAX_CFS_FDS && g_fd_table[idx].valid) {
            pthread_rwlock_rdlock(&g_fd_lock);
            char path[CFS_PATH_MAX];
            snprintf(path, CFS_PATH_MAX, "%s", g_fd_table[idx].path);
            pthread_rwlock_unlock(&g_fd_lock);
            return do_cfs_stat(path, (struct stat *)st);
        }
    }
    if (dirfd == AT_FDCWD && pathname && is_cfs_path(pathname)) {
        return do_cfs_stat(get_cfs_path(pathname), (struct stat *)st);
    }
    if (real_fstatat64) return real_fstatat64(dirfd, pathname, (struct stat *)st, flags);
    if (real_fstatat) return real_fstatat(dirfd, pathname, (struct stat *)st, flags);
    errno = ENOSYS; return -1;
}

int fstat64(int fd, struct stat64 *st) {
    if (is_cfs_fd(fd)) {
        int idx = fd - FD_OFFSET;
        pthread_rwlock_rdlock(&g_fd_lock);
        char path[CFS_PATH_MAX];
        snprintf(path, CFS_PATH_MAX, "%s", g_fd_table[idx].path);
        pthread_rwlock_unlock(&g_fd_lock);
        return do_cfs_stat(path, (struct stat *)st);
    }
    if (real_fstat64) return real_fstat64(fd, (struct stat *)st);
    if (real_fstat) return real_fstat(fd, (struct stat *)st);
    errno = ENOSYS; return -1;
}

int stat64(const char *__restrict pathname, struct stat64 *__restrict st) {
    if (is_cfs_path(pathname)) return do_cfs_stat(get_cfs_path(pathname), (struct stat *)st);
    if (real_stat64) return real_stat64(pathname, (struct stat *)st);
    if (real_stat) return real_stat(pathname, (struct stat *)st);
    errno = ENOSYS; return -1;
}

int lstat64(const char *__restrict pathname, struct stat64 *__restrict st) {
    if (is_cfs_path(pathname)) return do_cfs_stat(get_cfs_path(pathname), (struct stat *)st);
    if (real_lstat64) return real_lstat64(pathname, (struct stat *)st);
    if (real_lstat) return real_lstat(pathname, (struct stat *)st);
    errno = ENOSYS; return -1;
}

/* ========================================================================
 * 拦截函数：access / faccessat
 * TensorFlow 的 tf.data.Dataset.list_files() 使用 access() 检查文件存在性
 * ======================================================================== */

typedef int (*fn_access)(const char *, int);
typedef int (*fn_faccessat)(int, const char *, int, int);
static fn_access    real_access    = NULL;
static fn_faccessat real_faccessat = NULL;

int access(const char *pathname, int mode) {
    if (is_cfs_path(pathname)) {
        /* 对 CubeFS 文件，用 cfs_getattr 检查存在性 */
        struct cfs_stat_info cfs_st;
        int ret = p_cfs_getattr(g_client_id, (char *)get_cfs_path(pathname), &cfs_st);
        if (ret != 0) { errno = -ret; return -1; }
        /* 简单处理：文件存在即可（F_OK），不检查 R/W/X 权限 */
        return 0;
    }
    if (!real_access) real_access = (fn_access)dlsym(RTLD_NEXT, "access");
    return real_access(pathname, mode);
}

int faccessat(int dirfd, const char *pathname, int mode, int flags) {
    if (pathname && pathname[0] == '/' && is_cfs_path(pathname)) {
        return access(pathname, mode);
    }
    if (!real_faccessat) real_faccessat = (fn_faccessat)dlsym(RTLD_NEXT, "faccessat");
    return real_faccessat(dirfd, pathname, mode, flags);
}

/* ========================================================================
 * 拦截函数：lseek / lseek64
 * ======================================================================== */

off_t lseek(int fd, off_t offset, int whence) {
    if (is_cfs_fd(fd)) {
        int idx = fd - FD_OFFSET;
        off_t new_off;
        switch (whence) {
        case SEEK_SET:
            new_off = offset;
            break;
        case SEEK_CUR:
            new_off = g_fd_table[idx].offset + offset;
            break;
        case SEEK_END: {
            /* 需要获取文件大小 */
            struct cfs_stat_info cfs_st;
            pthread_rwlock_rdlock(&g_fd_lock);
            char path[CFS_PATH_MAX];
            snprintf(path, CFS_PATH_MAX, "%s", g_fd_table[idx].path);
            pthread_rwlock_unlock(&g_fd_lock);

            int ret = p_cfs_getattr(g_client_id, path, &cfs_st);
            if (ret != 0) { errno = -ret; return -1; }
            new_off = (off_t)cfs_st.size + offset;
            break;
        }
        default:
            errno = EINVAL;
            return -1;
        }
        g_fd_table[idx].offset = new_off;
        return new_off;
    }
    return real_lseek(fd, offset, whence);
}

off_t lseek64(int fd, off_t offset, int whence) {
    /* 在 64-bit 系统上 lseek64 和 lseek 等价 */
    return lseek(fd, offset, whence);
}

/* ========================================================================
 * 拦截函数：opendir / readdir / closedir
 * ======================================================================== */

#define DIR_BATCH_SIZE 65536

static __thread int g_in_cfs_call = 0;  /* 递归保护（防止 SDK 内部回调触发拦截）*/

DIR *opendir(const char *name) {
    if (is_cfs_path(name)) {
        const char *cfs_path = get_cfs_path(name);
        int cfs_fd = p_cfs_open(g_client_id, (char *)cfs_path, O_RDONLY, 0);
        if (cfs_fd < 0) {
            errno = -cfs_fd;
            return NULL;
        }

        cfs_dir_t *dir = calloc(1, sizeof(cfs_dir_t));
        if (!dir) { p_cfs_close(g_client_id, cfs_fd); errno = ENOMEM; return NULL; }

        dir->cfs_fd = cfs_fd;

        /* 分批读取所有目录项（cfs_readdir 支持游标分页） */
        int capacity = DIR_BATCH_SIZE;
        dir->entries = calloc(capacity, sizeof(struct cfs_dirent));
        if (!dir->entries) {
            p_cfs_close(g_client_id, cfs_fd);
            free(dir);
            errno = ENOMEM;
            return NULL;
        }

        int total = 0;
        g_in_cfs_call = 1;
        for (;;) {
            /* 确保有足够空间 */
            if (total + DIR_BATCH_SIZE > capacity) {
                capacity *= 2;
                struct cfs_dirent *new_entries = realloc(dir->entries, capacity * sizeof(struct cfs_dirent));
                if (!new_entries) {
                    g_in_cfs_call = 0;
                    p_cfs_close(g_client_id, cfs_fd);
                    free(dir->entries);
                    free(dir);
                    errno = ENOMEM;
                    return NULL;
                }
                dir->entries = new_entries;
            }

            GoSlice slice = { dir->entries + total, DIR_BATCH_SIZE, DIR_BATCH_SIZE };
            int n = p_cfs_readdir(g_client_id, cfs_fd, slice, DIR_BATCH_SIZE);
            if (n < 0) {
                int err = -n;
                g_in_cfs_call = 0;
                p_cfs_close(g_client_id, cfs_fd);
                free(dir->entries);
                free(dir);
                errno = err;
                return NULL;
            }
            total += n;
            if (n < DIR_BATCH_SIZE) break;  /* 读完了 */
        }
        g_in_cfs_call = 0;

        dir->count = total;
        dir->pos = 0;
        dir->magic = CFS_DIR_MAGIC;

        return (DIR *)dir;
    }
    return real_opendir(name);
}

struct dirent *readdir(DIR *dirp) {
    if (!dirp) { errno = EBADF; return NULL; }

    /* 检查是否是我们封装的 cfs_dir_t（通过 magic number 可靠区分） */
    if (is_cfs_dir(dirp) && !g_in_cfs_call) {
        cfs_dir_t *dir = (cfs_dir_t *)dirp;

        /* 延迟加载目录项（fdopendir 路径）— 分批读取全部 */
        if (dir->count < 0 && !dir->entries) {
            int capacity = DIR_BATCH_SIZE;
            dir->entries = calloc(capacity, sizeof(struct cfs_dirent));
            if (!dir->entries) { errno = ENOMEM; return NULL; }
            int total = 0;
            g_in_cfs_call = 1;
            for (;;) {
                if (total + DIR_BATCH_SIZE > capacity) {
                    capacity *= 2;
                    struct cfs_dirent *ne = realloc(dir->entries, capacity * sizeof(struct cfs_dirent));
                    if (!ne) { g_in_cfs_call = 0; free(dir->entries); dir->entries = NULL; errno = ENOMEM; return NULL; }
                    dir->entries = ne;
                }
                GoSlice slice = { dir->entries + total, DIR_BATCH_SIZE, DIR_BATCH_SIZE };
                int n = p_cfs_readdir(g_client_id, dir->cfs_fd, slice, DIR_BATCH_SIZE);
                if (n < 0) { n = 0; }
                total += n;
                if (n < DIR_BATCH_SIZE) break;
            }
            g_in_cfs_call = 0;
            dir->count = total;
            dir->pos = 0;
        }

        if (dir->pos >= dir->count) return NULL;

        struct cfs_dirent *e = &dir->entries[dir->pos];
        dir->dent.d_ino  = e->ino;
        dir->dent.d_type = (unsigned char)e->d_type;
        dir->dent.d_off  = dir->pos;
        strncpy(dir->dent.d_name, e->name, sizeof(dir->dent.d_name) - 1);
        dir->dent.d_name[sizeof(dir->dent.d_name) - 1] = '\0';
        dir->dent.d_reclen = sizeof(struct dirent);
        dir->pos++;

        return &dir->dent;
    }

    return real_readdir(dirp);
}

/* readdir64: CPython 3.11 在 Debian/Ubuntu 64-bit 上显式链接此符号 */
struct dirent64 *readdir64(DIR *dirp) {
    if (!dirp) { errno = EBADF; return NULL; }

    if (is_cfs_dir(dirp) && !g_in_cfs_call) {
        cfs_dir_t *dir = (cfs_dir_t *)dirp;

        /* 延迟加载目录项（fdopendir 路径）— 分批读取全部 */
        if (dir->count < 0 && !dir->entries) {
            int capacity = DIR_BATCH_SIZE;
            dir->entries = calloc(capacity, sizeof(struct cfs_dirent));
            if (!dir->entries) { errno = ENOMEM; return NULL; }
            int total = 0;
            g_in_cfs_call = 1;
            for (;;) {
                if (total + DIR_BATCH_SIZE > capacity) {
                    capacity *= 2;
                    struct cfs_dirent *ne = realloc(dir->entries, capacity * sizeof(struct cfs_dirent));
                    if (!ne) { g_in_cfs_call = 0; free(dir->entries); dir->entries = NULL; errno = ENOMEM; return NULL; }
                    dir->entries = ne;
                }
                GoSlice slice = { dir->entries + total, DIR_BATCH_SIZE, DIR_BATCH_SIZE };
                int n = p_cfs_readdir(g_client_id, dir->cfs_fd, slice, DIR_BATCH_SIZE);
                if (n < 0) { n = 0; }
                total += n;
                if (n < DIR_BATCH_SIZE) break;
            }
            g_in_cfs_call = 0;
            dir->count = total;
            dir->pos = 0;
        }

        if (dir->pos >= dir->count) return NULL;

        struct cfs_dirent *e = &dir->entries[dir->pos];
        /* 复用 dent 结构体（dirent64 和 dirent 在 64-bit 系统上布局一致）*/
        dir->dent.d_ino  = e->ino;
        dir->dent.d_type = (unsigned char)e->d_type;
        dir->dent.d_off  = dir->pos;
        strncpy(dir->dent.d_name, e->name, sizeof(dir->dent.d_name) - 1);
        dir->dent.d_name[sizeof(dir->dent.d_name) - 1] = '\0';
        dir->dent.d_reclen = sizeof(struct dirent64);
        dir->pos++;

        return (struct dirent64 *)&dir->dent;
    }

    if (real_readdir64) return real_readdir64(dirp);
    return (struct dirent64 *)real_readdir(dirp);
}

int closedir(DIR *dirp) {
    if (!dirp) { errno = EBADF; return -1; }

    if (is_cfs_dir(dirp)) {
        cfs_dir_t *dir = (cfs_dir_t *)dirp;
        p_cfs_close(g_client_id, dir->cfs_fd);
        free(dir->entries);
        free(dir);
        return 0;
    }

    return real_closedir(dirp);
}

/* ========================================================================
 * 拦截函数：fdopendir（Python os.listdir 底层使用 openat + fdopendir）
 * ======================================================================== */

DIR *fdopendir(int fd) {
    if (!g_preload_enabled || fd < FD_OFFSET) {
        return real_fdopendir(fd);
    }

    int idx = fd - FD_OFFSET;
    if (idx < 0 || idx >= MAX_CFS_FDS || !g_fd_table[idx].valid) {
        return real_fdopendir(fd);
    }

    /* 为 CubeFS 目录 fd 创建 cfs_dir_t 封装（延迟读取目录项）*/
    cfs_dir_t *dir = (cfs_dir_t *)calloc(1, sizeof(cfs_dir_t));
    if (!dir) { errno = ENOMEM; return NULL; }

    dir->cfs_fd = g_fd_table[idx].cfs_fd;
    dir->entries = NULL;  /* 延迟到 readdir 时加载 */
    dir->count = -1;      /* -1 表示尚未加载 */
    dir->pos = 0;
    dir->magic = CFS_DIR_MAGIC;

    /* 将 fd_entry 标记为被 fdopendir 接管（但不释放 cfs_fd）*/
    pthread_rwlock_wrlock(&g_fd_lock);
    g_fd_table[idx].valid = 0;  /* 不再通过 fd_table 访问 */
    pthread_rwlock_unlock(&g_fd_lock);

    return (DIR *)dir;
}

/* ========================================================================
 * 拦截函数：fcntl（防止对 CubeFS fd 的 fcntl 操作传到内核）
 * ======================================================================== */

int fcntl(int fd, int cmd, ...) {
    va_list ap;
    long arg = 0;
    va_start(ap, cmd);
    arg = va_arg(ap, long);
    va_end(ap);

    if (g_preload_enabled && fd >= FD_OFFSET) {
        int idx = fd - FD_OFFSET;
        if (idx >= 0 && idx < MAX_CFS_FDS && g_fd_table[idx].valid) {
            /* 对 CubeFS fd 的 fcntl 返回模拟值 */
            switch (cmd) {
            case F_GETFL:  return O_RDONLY;
            case F_SETFL:  return 0;
            case F_GETFD:  return FD_CLOEXEC;
            case F_SETFD:  return 0;
            case F_DUPFD:
            case F_DUPFD_CLOEXEC:
                errno = EBADF; return -1;
            default:
                errno = EINVAL; return -1;
            }
        }
    }
    if (real_fcntl) return real_fcntl(fd, cmd, arg);
    errno = ENOSYS; return -1;
}

/* ========================================================================
 * 拦截函数：getdents64（Python os.listdir/scandir 使用）
 * ======================================================================== */

ssize_t getdents64(int fd, void *dirp, size_t count) {
    if (!g_preload_enabled || fd < FD_OFFSET) {
        if (real_getdents64) return real_getdents64(fd, dirp, count);
        return syscall(SYS_getdents64, fd, dirp, count);
    }

    int idx = fd - FD_OFFSET;
    if (idx < 0 || idx >= MAX_CFS_FDS || !g_fd_table[idx].valid || !g_fd_table[idx].is_dir) {
        if (real_getdents64) return real_getdents64(fd, dirp, count);
        return syscall(SYS_getdents64, fd, dirp, (unsigned int)count);
    }

    /* 延迟加载目录项 */
    if (!g_fd_table[idx].dir_entries) {
        g_fd_table[idx].dir_entries = (struct cfs_dirent *)malloc(
            sizeof(struct cfs_dirent) * DIR_BATCH_SIZE);
        if (!g_fd_table[idx].dir_entries) { errno = ENOMEM; return -1; }

        GoSlice slice = { g_fd_table[idx].dir_entries, DIR_BATCH_SIZE, DIR_BATCH_SIZE };
        int n = p_cfs_readdir(g_client_id, g_fd_table[idx].cfs_fd, slice, DIR_BATCH_SIZE);
        if (n < 0) { errno = -n; free(g_fd_table[idx].dir_entries); g_fd_table[idx].dir_entries = NULL; return -1; }
        g_fd_table[idx].dir_count = n;
        g_fd_table[idx].dir_pos = 0;
    }

    /* 填充 linux_dirent64 到用户 buffer */
    char *buf = (char *)dirp;
    unsigned int bpos = 0;
    int pos = g_fd_table[idx].dir_pos;
    int total = g_fd_table[idx].dir_count;

    while (pos < total) {
        struct cfs_dirent *e = &g_fd_table[idx].dir_entries[pos];
        size_t namelen = strlen(e->name);
        /* linux_dirent64: d_ino(8) + d_off(8) + d_reclen(2) + d_type(1) + name + null */
        unsigned short reclen = (unsigned short)(8 + 8 + 2 + 1 + namelen + 1);
        /* 对齐到 8 字节 */
        reclen = (reclen + 7) & ~7;

        if (bpos + reclen > count) break;

        struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + bpos);
        d->d_ino = e->ino;
        d->d_off = pos + 1;
        d->d_reclen = reclen;
        d->d_type = (unsigned char)e->d_type;
        memcpy(d->d_name, e->name, namelen + 1);

        bpos += reclen;
        pos++;
    }

    g_fd_table[idx].dir_pos = pos;
    return (ssize_t)bpos;
}

