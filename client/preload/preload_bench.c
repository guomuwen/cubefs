/*
 * preload_bench.c — 多线程文件读取性能基准测试
 *
 * 通过 LD_PRELOAD 配合 libcfs_preload.so 使用，测试 CubeFS preload 模式
 * 下的文件读取 IOPS 和带宽。
 *
 * 编译: gcc -O2 -o preload_bench preload_bench.c -lpthread
 * 用法: LD_PRELOAD=libcfs_preload.so ... ./preload_bench -d /mnt/cfs/.../train -w 64 -n 10000
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdatomic.h>

#define MAX_FILES    4200000
#define MAX_PATH_LEN 512
#define READ_BUF_SIZE (256 * 1024)  /* 256KB read buffer */

/* 全局配置 */
static char g_dir[MAX_PATH_LEN] = "";
static int  g_workers = 64;
static int  g_max_files = 100000;
static int  g_epochs = 1;
static int  g_verbose = 0;
static char g_pattern[MAX_PATH_LEN] = "";  /* 文件名模式, e.g. "img_%07d_of_4200000.tfrecord" */
static int  g_pattern_total = 0;           /* 模式总数 */

/* 文件列表 */
static char **g_file_list = NULL;
static int    g_file_count = 0;

/* 统计 */
static atomic_long g_total_files;
static atomic_long g_total_bytes;
static atomic_long g_total_errors;
static atomic_int  g_running;

/* 时间工具 */
static double now_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

/* 扫描目录收集文件列表 */
static int scan_files(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "opendir(%s) failed: %s\n", dir, strerror(errno));
        return -1;
    }

    g_file_list = (char **)malloc(sizeof(char *) * MAX_FILES);
    if (!g_file_list) { closedir(d); return -1; }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && g_file_count < MAX_FILES) {
        if (ent->d_name[0] == '.') continue;
        /* 只收集普通文件 */
        char path[MAX_PATH_LEN];
        snprintf(path, MAX_PATH_LEN, "%s/%s", dir, ent->d_name);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            g_file_list[g_file_count] = strdup(path);
            g_file_count++;
        }
    }
    closedir(d);

    fprintf(stderr, "Scanned %d files from %s\n", g_file_count, dir);
    return 0;
}

/* 工作线程 */
typedef struct {
    int id;
    int start_idx;
    int end_idx;
    long files_read;
    long bytes_read;
    long errors;
} worker_t;

static void *worker_func(void *arg) {
    worker_t *w = (worker_t *)arg;
    char *buf = (char *)malloc(READ_BUF_SIZE);
    if (!buf) { w->errors++; return NULL; }

    for (int i = w->start_idx; i < w->end_idx && atomic_load(&g_running); i++) {
        const char *path = g_file_list[i % g_file_count];
        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            w->errors++;
            atomic_fetch_add(&g_total_errors, 1);
            continue;
        }

        long total = 0;
        ssize_t n;
        while ((n = read(fd, buf, READ_BUF_SIZE)) > 0) {
            total += n;
        }
        close(fd);

        w->files_read++;
        w->bytes_read += total;
        atomic_fetch_add(&g_total_files, 1);
        atomic_fetch_add(&g_total_bytes, total);

        if (g_verbose && w->files_read % 1000 == 0) {
            fprintf(stderr, "[W%02d] %ld files done\n", w->id, w->files_read);
        }
    }

    free(buf);
    return NULL;
}

/* 监控线程 */
static void *monitor_func(void *arg) {
    (void)arg;
    double start = now_sec();
    long prev_files = 0;
    long prev_bytes = 0;

    while (atomic_load(&g_running)) {
        usleep(1000000);  /* 1 秒 */
        long cur_files = atomic_load(&g_total_files);
        long cur_bytes = atomic_load(&g_total_bytes);
        long cur_errors = atomic_load(&g_total_errors);
        double elapsed = now_sec() - start;

        long delta_files = cur_files - prev_files;
        long delta_bytes = cur_bytes - prev_bytes;
        double delta_mb = delta_bytes / (1024.0 * 1024.0);

        fprintf(stderr, "[%5.1fs] Files=%ld  IOPS=%ld  BW=%.1fMB/s  Errors=%ld\n",
                elapsed, cur_files, delta_files, delta_mb, cur_errors);
        fflush(stderr);

        prev_files = cur_files;
        prev_bytes = cur_bytes;
    }
    return NULL;
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [options]\n"
            "  -d <dir>      Directory containing files (required)\n"
            "  -w <workers>  Number of worker threads (default: 64)\n"
            "  -n <count>    Max files to read per epoch (default: 100000)\n"
            "  -e <epochs>   Number of epochs (default: 1)\n"
            "  -p <pattern>  File name pattern, e.g. 'img_%%07d_of_4200000.tfrecord'\n"
            "  -t <total>    Total files for pattern (used with -p)\n"
            "  -v            Verbose output\n",
            prog);
}

int main(int argc, char *argv[]) {
    int opt;
    while ((opt = getopt(argc, argv, "d:w:n:e:p:t:vh")) != -1) {
        switch (opt) {
        case 'd': strncpy(g_dir, optarg, MAX_PATH_LEN - 1); break;
        case 'w': g_workers = atoi(optarg); break;
        case 'n': g_max_files = atoi(optarg); break;
        case 'e': g_epochs = atoi(optarg); break;
        case 'p': strncpy(g_pattern, optarg, MAX_PATH_LEN - 1); break;
        case 't': g_pattern_total = atoi(optarg); break;
        case 'v': g_verbose = 1; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    if (g_dir[0] == '\0') {
        fprintf(stderr, "Error: -d <dir> is required\n");
        usage(argv[0]);
        return 1;
    }

    fprintf(stderr, "Config: dir=%s workers=%d max_files=%d epochs=%d\n",
            g_dir, g_workers, g_max_files, g_epochs);

    /* 收集文件列表 */
    if (g_pattern[0] != '\0' && g_pattern_total > 0) {
        /* 使用文件名模式直接生成列表（避免 readdir 限制）*/
        int count = g_pattern_total < g_max_files ? g_pattern_total : g_max_files;
        g_file_list = (char **)malloc(sizeof(char *) * count);
        if (!g_file_list) { fprintf(stderr, "malloc failed\n"); return 1; }
        for (int i = 0; i < count; i++) {
            char name[MAX_PATH_LEN];
            snprintf(name, MAX_PATH_LEN, g_pattern, i);
            char path[MAX_PATH_LEN];
            snprintf(path, MAX_PATH_LEN, "%s/%s", g_dir, name);
            g_file_list[i] = strdup(path);
        }
        g_file_count = count;
        fprintf(stderr, "Generated %d file paths from pattern\n", g_file_count);
    } else {
        if (scan_files(g_dir) != 0) return 1;
    }
    if (g_file_count == 0) {
        fprintf(stderr, "No files found in %s\n", g_dir);
        return 1;
    }

    int files_per_epoch = g_max_files < g_file_count ? g_max_files : g_file_count;

    for (int epoch = 0; epoch < g_epochs; epoch++) {
        fprintf(stderr, "\n=== Epoch %d: reading %d files with %d workers ===\n",
                epoch + 1, files_per_epoch, g_workers);

        atomic_store(&g_total_files, 0);
        atomic_store(&g_total_bytes, 0);
        atomic_store(&g_total_errors, 0);
        atomic_store(&g_running, 1);

        /* 分配工作 */
        worker_t *workers = (worker_t *)calloc(g_workers, sizeof(worker_t));
        pthread_t *threads = (pthread_t *)malloc(sizeof(pthread_t) * g_workers);
        pthread_t monitor_thread;

        int files_per_worker = files_per_epoch / g_workers;
        int remaining = files_per_epoch % g_workers;
        int offset = 0;

        for (int i = 0; i < g_workers; i++) {
            workers[i].id = i;
            workers[i].start_idx = offset;
            int count = files_per_worker + (i < remaining ? 1 : 0);
            workers[i].end_idx = offset + count;
            offset += count;
        }

        /* 启动监控 */
        pthread_create(&monitor_thread, NULL, monitor_func, NULL);

        /* 启动工作线程 */
        double t0 = now_sec();
        for (int i = 0; i < g_workers; i++) {
            pthread_create(&threads[i], NULL, worker_func, &workers[i]);
        }

        /* 等待完成 */
        for (int i = 0; i < g_workers; i++) {
            pthread_join(threads[i], NULL);
        }
        double t1 = now_sec();
        atomic_store(&g_running, 0);
        pthread_join(monitor_thread, NULL);

        /* 汇总 */
        double elapsed = t1 - t0;
        long total_files = atomic_load(&g_total_files);
        long total_bytes = atomic_load(&g_total_bytes);
        long total_errors = atomic_load(&g_total_errors);
        double total_mb = total_bytes / (1024.0 * 1024.0);
        double iops = total_files / elapsed;
        double bw = total_mb / elapsed;

        fprintf(stderr, "\n");
        fprintf(stdout, "[Epoch %d] Summary: Files=%ld  Duration=%.1fs  "
                "IOPS=%.0f  BW=%.1fMB/s  TotalData=%.1fMB  Errors=%ld\n",
                epoch + 1, total_files, elapsed, iops, bw, total_mb, total_errors);
        fflush(stdout);

        free(workers);
        free(threads);
    }

    /* 清理 */
    for (int i = 0; i < g_file_count; i++) {
        free(g_file_list[i]);
    }
    free(g_file_list);

    return 0;
}
