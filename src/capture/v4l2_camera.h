#ifndef _V4L2_CAMERA_h_
#define _V4L2_CAMERA_h_

#include <linux/videodev2.h>

// 适配多平面格式，YUV420M 最多 3 个平面 (Y, U, V)
#define MAX_V4L2_PLANES 3

// 单个平面的信息
struct dmabuf_plane {
    int     fd;      // 该平面的 DMA-BUF fd
    size_t  length;  // 该平面的内存大小
    void    *start;  // mmap 的虚拟地址 (如果纯硬件零拷贝，可为 NULL)
};

// 完整的一帧图像 buffer 管理结构
struct dmabuf_buffer {
    int                 index;       // 对应 V4L2 的 buffer index
    int                 num_planes;  // 实际使用的平面数量
    struct dmabuf_plane planes[MAX_V4L2_PLANES]; // 包含的平面数组
};


int                     v4l2_open_device(const char *dev_name);
void                    v4l2_print_formats_mplane(int v4l2_fd);
void                    v4l2_set_format_mplane(int v4l2_fd, __u32 req_width, __u32 req_height, __u32 req_pixelformat, __u32 *out_sizes, __u8 *out_num_planes);
void                    v4l2_get_format_mplane(int v4l2_fd, int plane_index,__u32 *stride, __u32 *sizeimage);
void                    v4l2_init_dmabuf(int v4l2_fd, int req_count, int num_planes, size_t *plane_sizes, struct dmabuf_buffer **out_buffers);
void                    v4l2_queue_frame_dmabuf(int v4l2_fd, struct dmabuf_buffer *buf_info);
struct dmabuf_buffer*   v4l2_dequeue_frame_dmabuf(int v4l2_fd, struct dmabuf_buffer *buffers);
void                    v4l2_start_capturing(int v4l2_fd, unsigned int count, struct dmabuf_buffer *buf_info);
void                    v4l2_stop_capturing(int v4l2_fd);
void                    v4l2_close_device(int v4l2_fd);




#endif