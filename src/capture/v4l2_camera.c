#define _GNU_SOURCE

#include <stdio.h>      // 标准输入输出
#include <stdlib.h>     // 标准库，包含 exit() 等
#include <string.h>     // 字符串操作函数，如 memset()
#include <assert.h>     // 断言
#include <fcntl.h>      // 文件控制操作，包含 O_RDWR 等宏
#include <unistd.h>     // UNIX 标准函数，包含 close(), read() 等
#include <errno.h>      // 错误码定义，如 EINTR, EAGAIN
#include <sys/stat.h>   // 文件状态查询
#include <sys/types.h>  // 基本系统数据类型
#include <sys/ioctl.h>  // ioctl 系统调用
#include <sys/mman.h>   // 核心：定义了 PROT_* / MAP_* 宏和 mmap 函数
#include <linux/videodev2.h> // V4L2 的核心头文件！所有的 V4L2 宏和结构体都在这里
#include "v4l2_camera.h"
#include "dma_buffer.h"

/**
 * @brief 专家级 ioctl 封装函数（防御性编程）
 * @details 处理因为系统信号打断导致的 EINTR 错误，保证命令必定送达内核
 */
static int xioctl(int fh, int request, void *arg) {
    int r;
    do {
        // 执行 ioctl
        r = ioctl(fh, request, arg);
    } while (-1 == r && EINTR == errno); // 如果失败且原因是 EINTR，则继续重试

    return r;
}

/**
 * @brief 打开并校验设备节点
 * @param dev_name 设备路径，例如 "/dev/video0"
 * @return 成功返回设备文件描述符(fd)，失败退出程序
 */
int v4l2_open_device(const char *dev_name) {
    struct stat st;
    int fd;

    // 1. 检查设备节点是否存在
    if (-1 == stat(dev_name, &st)) {
        fprintf(stderr, "无法识别 '%s': %d, %s\n", dev_name, errno, strerror(errno));
        exit(EXIT_FAILURE);
    }

    // 2. 检查它是不是一个字符设备 (V4L2 设备必须是字符设备)
    if (!S_ISCHR(st.st_mode)) {
        fprintf(stderr, "%s 不是一个字符设备文件\n", dev_name);
        exit(EXIT_FAILURE);
    }

    // 3. 打开设备 (避坑：必须使用 O_RDWR)
    // O_NONBLOCK 表示非阻塞模式。在这个教程的后续，我们会用 select/poll 来等待数据，
    // 配合 O_NONBLOCK 是最高效的工业级做法，避免进程在内核态死锁。
    fd = open(dev_name, O_RDWR, 0);
    if (-1 == fd) {
        fprintf(stderr, "无法打开 '%s': %d, %s\n", dev_name, errno, strerror(errno));
        exit(EXIT_FAILURE);
    }

    // 4. 查询设备能力 (Capabilities)
    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (-1 == xioctl(fd, VIDIOC_QUERYCAP, &cap)) {
        if (EINVAL == errno) {
            fprintf(stderr, "%s 不是一个标准的 V4L2 设备\n", dev_name);
            exit(EXIT_FAILURE);
        } else {
            perror("VIDIOC_QUERYCAP 失败");
            exit(EXIT_FAILURE);
        }
    }

    // 打印设备基本信息，这在调试时非常有用
    printf("--- 设备信息 ---\n");
    printf("驱动名称: %s\n", cap.driver);
    printf("设备名称: %s\n", cap.card);
    printf("总线信息: %s\n", cap.bus_info);
    printf("V4L2版本: %u.%u.%u\n", (cap.version >> 16) & 0xFF, (cap.version >> 8) & 0xFF, cap.version & 0xFF);

    // 5. 校验设备是否具备我们想要的能力 (视频捕获和流式 I/O)
    __u32 caps = cap.capabilities;
    // 避坑：兼容现代 Linux 内核的写法，优先检查 device_caps
    if (cap.capabilities & V4L2_CAP_DEVICE_CAPS) {
        caps = cap.device_caps;
        printf("检测到现代 V4L2 设备，使用 device_caps 校验\n");
    }

    if (caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) {
        printf("成功：设备支持 V4L2_CAP_VIDEO_CAPTURE_MPLANE (多平面捕获)\n");
    } else if (caps & V4L2_CAP_VIDEO_CAPTURE) {
        printf("成功：设备支持 V4L2_CAP_VIDEO_CAPTURE (单平面捕获)\n");
    } else {
        fprintf(stderr, "失败：%s 不是一个视频捕获设备 (既不支持单平面也不支持多平面)\n", dev_name);
        exit(EXIT_FAILURE);
    }

    // 检查是否支持流式 I/O (STREAMING)
    // V4L2 内存模型有 read/write 和 streaming 两种，
    // 现代应用 99.9% 都使用 streaming (MMAP 或 DMABUF)，效率远高于 read/write
    if (!(caps & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "%s 不支持 Streaming I/O，本教程不支持老旧的 read/write 模式\n", dev_name);
        exit(EXIT_FAILURE);
    }

    printf("设备校验通过，支持视频捕获与流式I/O！\n");
    printf("----------------\n");

    return fd;
}


// 假设之前部分的 xioctl 和 头文件都已经包含了
// 包含所需的标准头文件...

/**
 * @brief 枚举并打印摄像头支持的所有多平面 (MPLANE) 像素格式
 * @param fd 打开的设备文件描述符
 */
void v4l2_print_formats_mplane(int v4l2_fd) {
    struct v4l2_fmtdesc fmtdesc;
    memset(&fmtdesc, 0, sizeof(fmtdesc));
    
    // 【核心改动 1】：类型必须更改为多平面宏
    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmtdesc.index = 0; // 从 0 开始遍历

    printf("--- 摄像头支持的多平面像素格式 ---\n");
    while (xioctl(v4l2_fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0) {
        char fourcc[5] = {0};
        fourcc[0] = (fmtdesc.pixelformat >> 0) & 0xFF;
        fourcc[1] = (fmtdesc.pixelformat >> 8) & 0xFF;
        fourcc[2] = (fmtdesc.pixelformat >> 16) & 0xFF;
        fourcc[3] = (fmtdesc.pixelformat >> 24) & 0xFF;

        printf("索引 [%d]: 描述 = '%s', FourCC = [%s]%s\n", 
               fmtdesc.index, 
               fmtdesc.description, 
               fourcc,
               (fmtdesc.flags & V4L2_FMT_FLAG_COMPRESSED) ? " (压缩格式)" : "");
        
        fmtdesc.index++;
    }
    printf("----------------------------\n");
}

/**
 * @brief 协商并设置多平面视频格式 (分辨率和像素格式)
 * @param fd 文件描述符
 * @param req_width 期望的宽度
 * @param req_height 期望的高度
 * @param req_pixelformat 期望的格式 (如 V4L2_PIX_FMT_NV12)
 * @param out_sizes 传入一个数组，用于接收每个平面的大小
 * @param out_num_planes 用于接收驱动最终分配的平面数量
 */
void v4l2_set_format_mplane(int v4l2_fd, __u32 req_width, __u32 req_height, __u32 req_pixelformat, 
                             __u32 *out_sizes, __u8 *out_num_planes) {
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));

    // 【核心改动 2】：指定多平面类型
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    
    // 【核心改动 3】：必须使用 .pix_mp 这个联合体成员，绝不能再用 .pix
    fmt.fmt.pix_mp.width       = req_width;
    fmt.fmt.pix_mp.height      = req_height;
    fmt.fmt.pix_mp.pixelformat = req_pixelformat;
    fmt.fmt.pix_mp.field       = V4L2_FIELD_ANY;

    // 执行协商/设置格式操作
    if (-1 == xioctl(v4l2_fd, VIDIOC_S_FMT, &fmt)) {
        perror("VIDIOC_S_FMT (MPLANE) 失败");
        exit(EXIT_FAILURE);
    }

    printf("--- MPLANE 格式协商结果 ---\n");
    printf("你请求的: %u x %u\n", req_width, req_height);
    printf("驱动最终给的: %u x %u\n", fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height);
    
    // 获取驱动确定的平面总数 (比如 NV12M 通常是 2 个平面)
    __u8 num_planes = fmt.fmt.pix_mp.num_planes;
    printf("返回的平面数量 (num_planes): %u\n", num_planes);

    if (fmt.fmt.pix_mp.width != req_width || fmt.fmt.pix_mp.height != req_height) {
        fprintf(stderr, "警告: 摄像头不支持你请求的分辨率，驱动已自动修改为最接近的可用分辨率！\n");
    }

    if (fmt.fmt.pix_mp.pixelformat != req_pixelformat) {
        char req_fcc[5], real_fcc[5];
        snprintf(req_fcc, 5, "%c%c%c%c", req_pixelformat&0xFF, (req_pixelformat>>8)&0xFF, (req_pixelformat>>16)&0xFF, (req_pixelformat>>24)&0xFF);
        snprintf(real_fcc, 5, "%c%c%c%c", fmt.fmt.pix_mp.pixelformat&0xFF, (fmt.fmt.pix_mp.pixelformat>>8)&0xFF, (fmt.fmt.pix_mp.pixelformat>>16)&0xFF, (fmt.fmt.pix_mp.pixelformat>>24)&0xFF);
        
        fprintf(stderr, "致命错误: 请求像素格式 %s，但驱动强制使用了 %s。程序无法继续。\n", req_fcc, real_fcc);
        exit(EXIT_FAILURE);
    }

    // 将平面数量传出
    if (out_num_planes) {
        *out_num_planes = num_planes;
    }

    // 【核心改动 4】：遍历平面数组 `plane_fmt` 获取各平面的内存步长和大小
    for (__u8 i = 0; i < num_planes; i++) {
        printf("平面 [%u] 步长 (Bytes per line): %u\n", i, fmt.fmt.pix_mp.plane_fmt[i].bytesperline);
        printf("平面 [%u] 大小 (Size image): %u bytes\n", i, fmt.fmt.pix_mp.plane_fmt[i].sizeimage);
        
        // 将各个平面的内存大小传出去，后续 REQBUFS 分配内存时要用！
        if (out_sizes) {
            out_sizes[i] = fmt.fmt.pix_mp.plane_fmt[i].sizeimage;
        }
    }
    printf("--------------------\n");
}

/**
 * @brief 获取多平面视频格式 (分辨率和像素格式)
 * @param fd 文件描述符
 * @param plane_index 第几个平面
 * @param stride 步长
 * @param sizeimage 总字节数
 */
void v4l2_get_format_mplane(int v4l2_fd, int plane_index,__u32 *stride, __u32 *sizeimage) {
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));

    // 多平面 API (MPLANE)：
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    ioctl(v4l2_fd, VIDIOC_G_FMT, &fmt);
    *stride = fmt.fmt.pix_mp.plane_fmt[0].bytesperline; // ✨ 第一平面的真实步长
    *sizeimage = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
}

/**
 * @brief 初始化 DMABUF 和 V4L2 队列
 * @param v4l2_fd     视频设备节点
 * @param req_count   请求的 buffer 数量 (如 4, 8)
 * @param num_planes  平面数量 (NV12=1, NV12M=2, I420=3)
 * @param plane_sizes 每个平面的大小数组 (长度应等于 num_planes)
 * @param out_buffers 输出：我们维护的管理结构体数组
 */
void v4l2_init_dmabuf(int v4l2_fd, int req_count, int num_planes, 
                        size_t *plane_sizes, struct dmabuf_buffer *out_buffers) {
    
    if (num_planes > MAX_V4L2_PLANES) {
        fprintf(stderr, "请求的平面数量超过宏定义最大值\n");
        exit(EXIT_FAILURE);
    }

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count  = req_count;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; 
    req.memory = V4L2_MEMORY_DMABUF; 

    if (-1 == ioctl(v4l2_fd, VIDIOC_REQBUFS, &req)) {
        perror("VIDIOC_REQBUFS 失败");
        exit(EXIT_FAILURE);
    }

    // *out_buffers = calloc(req.count, sizeof(struct dmabuf_buffer));
    
    for (unsigned int i = 0; i < req.count; ++i) {
        out_buffers[i].index = i;
        out_buffers[i].num_planes = num_planes;

        // 遍历分配每一个平面的内存
        for (int p = 0; p < num_planes; ++p) {
            int dma_fd = dmabuf_alloc(plane_sizes[p]);
            if (dma_fd < 0) exit(EXIT_FAILURE);

            out_buffers[i].planes[p].fd = dma_fd;
            out_buffers[i].planes[p].length = plane_sizes[p];

            // 零拷贝优化：如果后续100%只走 MPP/RKNN，这里可以直接注释掉 mmap
            // (*out_buffers)[i].planes[p].start = mmap(NULL, plane_sizes[p], 
            //                                          PROT_READ | PROT_WRITE, MAP_SHARED, dma_fd, 0);
        }
    }
}


/**
 * @brief 将指定的 buffer 入队 (归还给 V4L2)
 * @param v4l2_fd   视频节点
 * @param buf_info  要归还的 buffer 结构体指针
 */
void v4l2_queue_frame_dmabuf(int v4l2_fd, struct dmabuf_buffer *buf_info) {
    struct v4l2_buffer buf;
    struct v4l2_plane planes[MAX_V4L2_PLANES]; 
    
    memset(&buf, 0, sizeof(buf));
    memset(&planes, 0, sizeof(planes));

    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_DMABUF;
    buf.index  = buf_info->index;
    
    buf.m.planes = planes;
    buf.length   = buf_info->num_planes; 

    // 填充所有平面的 fd
    for (int p = 0; p < buf_info->num_planes; ++p) {
        buf.m.planes[p].m.fd = buf_info->planes[p].fd;
        buf.m.planes[p].length = buf_info->planes[p].length;
    }

    if (-1 == ioctl(v4l2_fd, VIDIOC_QBUF, &buf)) {
        perror("VIDIOC_QBUF 失败");
        exit(EXIT_FAILURE);
    }
}

/**
 * @brief 出队获取一帧数据
 * @param v4l2_fd   视频节点
 * @param buffers   dmabuf_buffers数组
 * @return 成功返回对应 buffer 的指针，没数据返回 NULL
 */
struct dmabuf_buffer* v4l2_dequeue_frame_dmabuf(int v4l2_fd, struct dmabuf_buffer *buffers) {
    struct v4l2_buffer buf;
    struct v4l2_plane planes[MAX_V4L2_PLANES];
    
    memset(&buf, 0, sizeof(buf));
    memset(&planes, 0, sizeof(planes));

    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_DMABUF;
    buf.m.planes = planes;
    // 注意：DQBUF 时也必须告诉内核 planes 数组的最大容量，否则内核会报错
    buf.length   = MAX_V4L2_PLANES; 

    if (-1 == ioctl(v4l2_fd, VIDIOC_DQBUF, &buf)) {
        if (errno == EAGAIN) return NULL; // 非阻塞模式下暂时没数据
        perror("DQBUF 失败"); 
        exit(EXIT_FAILURE);
    }

    // 成功出队，返回对应的结构体指针给上层
    return &buffers[buf.index];
}


/**
 * @brief 开启视频流
 * @param fd 设备文件描述符
 * @param count 我们申请的缓冲区总数
 */
void v4l2_start_capturing(int v4l2_fd, unsigned int count, struct dmabuf_buffer *buf_info) {
    // 1. 在开启流之前，必须把所有缓冲区塞入内核的"空闲队列"
    for (unsigned int i = 0; i < count; ++i) {
        // 入队
        v4l2_queue_frame_dmabuf(v4l2_fd, &buf_info[i]);
    }

    // 2. 发送 STREAMON 命令，启动 DMA 传输
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (-1 == xioctl(v4l2_fd, VIDIOC_STREAMON, &type)) {
        perror("VIDIOC_STREAMON 失败");
        exit(EXIT_FAILURE);
    }
    printf("视频流已启动 (STREAMON)\n");
}

/**
 * @brief 关闭视频流
 * @param fd 设备文件描述符
 */
void v4l2_stop_capturing(int v4l2_fd) {
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    // 即使失败通常也无伤大雅，因为程序都要退出了
    if (-1 == xioctl(v4l2_fd, VIDIOC_STREAMOFF, &type)) {
        perror("VIDIOC_STREAMOFF 失败");
    }
    printf("视频流已停止 (STREAMOFF)\n");
}


// 释放设备的简单封装
void v4l2_close_device(int v4l2_fd) {
    if (-1 == close(v4l2_fd)) {
        perror("关闭设备失败");
    }
}

