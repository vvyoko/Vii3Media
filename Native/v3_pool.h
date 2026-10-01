#ifndef V3_POOL_H
#define V3_POOL_H

#include <windows.h>
#include <stdint.h>

/*
 * 逐行变换的轻量线程池（持久线程 + 原子领段）。
 *
 * 只用于「共享只读数据、各段写出的目标区域互不相交」的逐行变换
 * （色调映射 / half 转换 / 旋转都满足：按源行分段 → 目标行或列区间互不相交）。
 *
 * 线程随池创建与销毁 —— 每帧新建线程的开销会超过变换本身。
 * 工作分配用原子领段：主线程与 worker 各自抢下一个段号，抢完为止，
 * 最后一个完成的线程唤醒主线程；段边界取偶（与 YUV420 色度 2 行对齐一致）。
 *
 * 用法：调用方结构里放一个 V3Pool（须零初始化，如 av_mallocz），
 * 打开时 v3_pool_create(&p, n)、关闭时 v3_pool_destroy(&p)、
 * 变换时 v3_pool_run(&p, 行数, 宽度, 分带函数, 上下文)。
 */

#define V3_POOL_MAX_THREADS 16
#define V3_POOL_MIN_BAND_ROWS 32

/*
 * 物理核数（不是逻辑处理器数）。超线程对逐像素转换的增益很小，
 * 而辅助线程按逻辑核开容易与解码线程互相超订。检测失败返回 0（调用方退回默认值）。
 */
static inline int v3_cpu_physical_cores(void)
{
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX buf[64];
    DWORD len = (DWORD)sizeof(buf);
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, buf, &len))
    {
        return 0;
    }
    int cores = 0;
    DWORD off = 0;
    while (off < len)
    {
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *e =
            (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)((uint8_t *)buf + off);
        if (e->Size == 0)
        {
            break;
        }
        if (e->Relationship == RelationProcessorCore)
        {
            cores++;
        }
        off += e->Size;
    }
    return cores;
}

typedef void (*V3BandFn)(void *ctx, int y0, int y1);

typedef struct
{
    HANDLE sem;   /* 计数信号量：主线程每帧按需唤醒 worker */
    HANDLE done;  /* 所有段跑完时通知主线程 */
    HANDLE th[V3_POOL_MAX_THREADS];
    int nworker;
    volatile LONG next;      /* 下一个待领取的段号 */
    volatile LONG remaining; /* 未完成的参与线程数（含主线程） */
    volatile LONG stop;
    V3BandFn fn;
    void *ctx;
    int chunk;
    int nband;
    int total;
} V3Pool;

static inline DWORD WINAPI v3_pool_proc(LPVOID param)
{
    V3Pool *p = (V3Pool *)param;
    for (;;)
    {
        WaitForSingleObject(p->sem, INFINITE);
        if (p->stop)
        {
            return 0;
        }
        for (;;)
        {
            LONG i = InterlockedIncrement(&p->next) - 1;
            if (i >= (LONG)p->nband)
            {
                break;
            }
            int b = (int)i * p->chunk;
            int e = b + p->chunk;
            if (e > p->total)
            {
                e = p->total;
            }
            if (b < p->total)
            {
                p->fn(p->ctx, b, e);
            }
        }
        if (InterlockedDecrement(&p->remaining) == 0)
        {
            SetEvent(p->done);
        }
    }
}

/* 建池：worker 数 = nthreads-1（主线程也算一份）。任何失败都退化为单线程。 */
static inline void v3_pool_create(V3Pool *p, int nthreads)
{
    p->nworker = 0;
    if (nthreads < 2)
    {
        return;
    }
    if (nthreads > V3_POOL_MAX_THREADS)
    {
        nthreads = V3_POOL_MAX_THREADS;
    }
    p->sem = CreateSemaphore(NULL, 0, V3_POOL_MAX_THREADS, NULL);
    p->done = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (p->sem == NULL || p->done == NULL)
    {
        return;
    }
    for (int i = 0; i < nthreads - 1; i++)
    {
        HANDLE h = CreateThread(NULL, 0, v3_pool_proc, p, 0, NULL);
        if (h == NULL)
        {
            break;
        }
        p->th[p->nworker++] = h;
    }
}

static inline void v3_pool_destroy(V3Pool *p)
{
    if (p->nworker > 0)
    {
        p->stop = 1;
        ReleaseSemaphore(p->sem, p->nworker, NULL);
        for (int i = 0; i < p->nworker; i++)
        {
            WaitForSingleObject(p->th[i], INFINITE);
            CloseHandle(p->th[i]);
        }
        p->nworker = 0;
    }
    if (p->sem != NULL)
    {
        CloseHandle(p->sem);
        p->sem = NULL;
    }
    if (p->done != NULL)
    {
        CloseHandle(p->done);
        p->done = NULL;
    }
}

/* 在池上跑一次逐行并行；池未建立 / 像素太少则单线程直接跑。 */
static inline void v3_pool_run(V3Pool *p, int total, int width, V3BandFn fn, void *ctx)
{
    if (total <= 0)
    {
        return;
    }
    /* 小帧不值得并行：唤醒与同步的固定开销会盖过收益。 */
    if ((long long)total * (long long)width < 640LL * 480)
    {
        fn(ctx, 0, total);
        return;
    }
    int want = p->nworker + 1;
    if (want > 1)
    {
        int maxparts = total / V3_POOL_MIN_BAND_ROWS;
        if (maxparts < 1)
        {
            maxparts = 1;
        }
        if (want > maxparts)
        {
            want = maxparts;
        }
    }
    if (want <= 1 || p->nworker <= 0)
    {
        fn(ctx, 0, total);
        return;
    }

    int chunk = (total + want - 1) / want;
    chunk = (chunk + 1) & ~1; /* 取偶 */
    if (chunk < 2)
    {
        chunk = 2;
    }
    int nband = (total + chunk - 1) / chunk;
    int use = p->nworker;
    if (use > nband - 1)
    {
        use = nband - 1;
    }
    if (use <= 0)
    {
        fn(ctx, 0, total);
        return;
    }

    p->fn = fn;
    p->ctx = ctx;
    p->chunk = chunk;
    p->total = total;
    p->nband = nband;
    InterlockedExchange(&p->next, 0);
    InterlockedExchange(&p->remaining, use + 1);

    ReleaseSemaphore(p->sem, use, NULL); /* 唤醒 use 个 worker（其余下帧再用） */

    for (;;)
    {
        LONG i = InterlockedIncrement(&p->next) - 1;
        if (i >= (LONG)nband)
        {
            break;
        }
        int b = (int)i * chunk;
        int e = b + chunk;
        if (e > total)
        {
            e = total;
        }
        fn(ctx, b, e); /* 主线程也参与领段 */
    }
    if (InterlockedDecrement(&p->remaining) == 0)
    {
        SetEvent(p->done);
    }
    WaitForSingleObject(p->done, INFINITE);
}

#endif /* V3_POOL_H */
