#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/video/video.h>
#include <unistd.h>
#include "v4l2_camera.h"
#include <locale.h>  // 必须包含这个头文件

#define VIDEO_WIDTH   3840
#define VIDEO_HEIGHT  2160
#define VIDEO_BPP     1.5    // NV12 是 3 字节/像素
#define VIDEO_FPS     30   // 30 帧/秒


// 专门用来给释放回调传参的结构体
typedef struct {
    int v4l2_fd;
    struct dmabuf_buffer *buffer;
} BufferReleaseInfo;


typedef struct _CustomData {
    GstElement *pipeline;
    GMainLoop *loop;
    int v4l2_fd;
    struct dmabuf_buffer *out_buffers;  // 数组
    __u32 stride;
    __u32 sizeimage;
} CustomData;

 GstAllocator *allocator = NULL;

/* 这个函数会在 GStreamer 彻底用完这一帧（比如编码完成）被自动调用！ */
static void on_gst_buffer_free(gpointer user_data, GstMiniObject *obj) {
    BufferReleaseInfo *info = (BufferReleaseInfo *)user_data;
    
    // 此时硬件已经处理完毕，安全归还给 V4L2！
    // g_print("GStreamer 用完了帧，正在安全 QBUF...\n");
    v4l2_queue_frame_dmabuf(info->v4l2_fd, info->buffer);
    
    // 别忘了释放上下文内存
    free(info);
}

/* 总线消息回调函数 */
static gboolean bus_call_back(GstBus *bus, GstMessage *msg, gpointer user_data) {
    CustomData *data = (CustomData *)user_data;

    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError *err = NULL;
            gchar *debug_info = NULL;
            // 解析错误信息：提取出给人看的错误字符串
            gst_message_parse_error(msg, &err, &debug_info);
            g_printerr("致命错误: 来自元件 %s: %s\n", GST_OBJECT_NAME(msg->src), err->message);
            g_printerr("调试信息: %s\n", debug_info ? debug_info : "无");
            g_clear_error(&err);
            g_free(debug_info);
            
            // 发生错误，退出主循环
            g_main_loop_quit(data->loop); 
            break;
        }
        case GST_MESSAGE_EOS:
            g_print("End-Of-Stream (EOS): 视频播放结束。\n");
            g_main_loop_quit(data->loop);
            break;
        case GST_MESSAGE_STATE_CHANGED: {
            // 只有当状态改变来自于 pipeline 本身时才打印，忽略内部元件的琐碎状态改变
            if (GST_MESSAGE_SRC(msg) == GST_OBJECT(data->pipeline /* 传入的pipeline */)) {
                GstState old_state, new_state, pending_state;
                gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);
                g_print("管道状态从 %s 变为 %s\n", 
                        gst_element_state_get_name(old_state), 
                        gst_element_state_get_name(new_state));
            }
            break;
        }
        default:
            // 忽略其他消息
            break;
    }
    // 返回 TRUE 表示继续监听；返回 FALSE 会自动把这个 Watch 移除
    return TRUE; 
}

/* 在 appsrc 的 need-data 回调中 */
static GstBuffer * push_dmabuf_to_appsrc(GstElement *appsrc, int dmabuf_fd, gsize size, int stride, int total_size) {
    static guint64 frame_count = 0;

    // 2. 把裸露的 fd 包装成 GStreamer 认识的 GstMemory 对象
    // 注意：这里的 fd 必须是有效的硬件文件描述符。
    int dmabuf_fd_cp = dup(dmabuf_fd);
    if (dmabuf_fd_cp < 0) {
        g_printerr("复制DMA-BUF fd失败！\n");
        return;
    }
    GstMemory *mem = gst_dmabuf_allocator_alloc(allocator, dmabuf_fd_cp, size);

    // 3. 创建一个空的 GstBuffer，并把这块内存挂载上去
    GstBuffer *buffer = gst_buffer_new();
    gst_buffer_append_memory(buffer, mem);

    // ------------------------------------------------------------------
    // 4. 【高阶灵魂一步：GstVideoMeta (视频元数据)】
    // 硬件内存通常是对齐的 (比如 1920 宽的图像，在硬件里可能按照 2048 字节对齐，这叫 stride/pitch)。
    // 如果你不打上 Video Meta，硬件编码器拿到了 fd 也不知道怎么按行读取像素，直接绿屏/花屏！
    // ------------------------------------------------------------------
    
    // 计算常规的 stride (以 NV12 格式为例，Y分量的 stride 等于 width)
    gint strides[4] = { stride, stride,0 ,0}; 
    gsize offset[4] = { 0, (gsize)total_size * 2 / 3 ,0 ,0}; // NV12 的 UV 分量偏移量

    gst_buffer_add_video_meta_full(
        buffer, 
        GST_VIDEO_FRAME_FLAG_NONE, 
        GST_VIDEO_FORMAT_NV12,  // 你的硬件格式，通常是 NV12 或 I420
        VIDEO_WIDTH, VIDEO_HEIGHT, 
        2,                      // NV12 有两个平面 (Y plane 和 UV plane)
        offset, 
        strides
    );

    GstClockTime pts = frame_count * (GST_SECOND / VIDEO_FPS); 
    GST_BUFFER_PTS(buffer) = pts;
    GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale_int(1, GST_SECOND, VIDEO_FPS);
    frame_count++;

    // 6. 塞入 appsrc
    GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(appsrc), buffer);
    if (ret != GST_FLOW_OK) {
        g_printerr("appsrc 塞入buffer失败: %d\n", ret);
    }

    return buffer;
}



// /* 【核心大招】：当 appsrc 饿了，就会触发这个回调函数来要数据 */
// static void on_need_data(GstElement *appsrc, guint unused_size, gpointer user_data) {
//     CustomData *data = (CustomData *)user_data;
//     struct dmabuf_buffer *buffer = v4l2_dequeue_frame_dmabuf(data->v4l2_fd, data->out_buffers);

//     if (!buffer) {
//         // 非阻塞模式下暂无数据，短暂休眠避免忙循环
//         usleep(5000);  // 5ms
//         g_print("aaa\n");
//         return;
//     }
//     g_print("bbb\n");
//     push_dmabuf_to_appsrc(appsrc, buffer->planes[0].fd, buffer->planes[0].length, data->stride, data->sizeimage);

//     v4l2_queue_frame_dmabuf(data->v4l2_fd, buffer);
// }
static void on_need_data(GstElement *appsrc, guint unused_size, gpointer user_data) {
    CustomData *data = (CustomData *)user_data;
    
    struct dmabuf_buffer *buffer = v4l2_dequeue_frame_dmabuf(data->v4l2_fd, data->out_buffers);
    if (!buffer) {
        // 返回，等待 appsrc 下一次催促
        // 注意：如果你 V4L2 是非阻塞的，建议把 V4L2 改为阻塞模式(去掉 O_NONBLOCK)
        // 因为 need-data 是独立的推流线程，在这里轻微阻塞等一帧画面是合理的，休眠治标不治本。
        return; 
    }

    // 假设你的 push 函数内部调用了 gst_buffer_new() 之类的方法，请让它返回那个 GstBuffer 指针
    GstBuffer *gst_buf = push_dmabuf_to_appsrc(appsrc, buffer->planes[0].fd, buffer->planes[0].length, data->stride, data->sizeimage);

    if (gst_buf) {
        // 【核心大招】：内存托管！
        BufferReleaseInfo *info = malloc(sizeof(BufferReleaseInfo));
        info->v4l2_fd = data->v4l2_fd;
        info->buffer = buffer;

        // 告诉 GStreamer：当你准备销毁 gst_buf 时，请先调用 on_gst_buffer_free
        gst_mini_object_weak_ref(GST_MINI_OBJECT(gst_buf), 
                                 (GstMiniObjectNotify)on_gst_buffer_free, 
                                 info);
    }

    // ⚠️ 绝 对 不 能 在 这 里 QBUF 了 ⚠️
    // v4l2_queue_frame_dmabuf(data->v4l2_fd, buffer); // 删掉删掉删掉！
}


int gstreamer_init(void) {
    gst_init(NULL, NULL);

    CustomData data;
    data.v4l2_fd = v4l2_open_device("/dev/video11");

    unsigned int out_sizes[1] = {0};
    unsigned char out_num_planes = 0;
    v4l2_set_format_mplane(data.v4l2_fd, 3840, 2160, V4L2_PIX_FMT_NV12, out_sizes, &out_num_planes);
    v4l2_get_format_mplane(data.v4l2_fd, 0, &data.stride, &data.sizeimage);
    size_t plane_sizes[1] = {data.sizeimage};
    v4l2_init_dmabuf(data.v4l2_fd, 4, 1, plane_sizes, &data.out_buffers);

    GstElement *appsrc =  gst_element_factory_make("appsrc", "v4l2out");
    GstElement *capsfilter =  gst_element_factory_make("capsfilter", "capsfilter");
    GstElement *mpph264enc =  gst_element_factory_make("mpph264enc", "mpph264enc");
    GstElement *rtph264pay =  gst_element_factory_make("rtph264pay", "rtph264pay");
    GstElement *udpsink =  gst_element_factory_make("udpsink", "udpsink");

    if (!appsrc || !mpph264enc || !rtph264pay || !udpsink) {
        g_printerr("有元件创建失败，检查插件是否安装！\n");
        return -1;
    }

    GstCaps *caps = gst_caps_new_simple ("video/x-raw",
     "width", G_TYPE_INT, VIDEO_WIDTH,
     "height", G_TYPE_INT, VIDEO_HEIGHT,
     "format", G_TYPE_STRING, "NV12",
     "framerate", GST_TYPE_FRACTION, VIDEO_FPS, 1, // 注意帧率是分数类型
     NULL); // 必须以 NULL 结尾！

    g_object_set(capsfilter, "caps", caps, NULL);
    gst_caps_unref(caps);
    g_object_set(appsrc, "is-live", TRUE, "format", GST_FORMAT_TIME, NULL);
    g_object_set(rtph264pay, "config-interval", 1, NULL);
    g_object_set(udpsink, "host", "192.168.1.6", "port", 5000, NULL);

    GstElement *pipeline = gst_pipeline_new("my_player_pipeline");
    if (!pipeline) {
        g_printerr("pipeline创建失败！\n");
        return -1;
    }
    data.pipeline = pipeline;
    

    // 1. 获取系统默认的 DMA-BUF 分配器
    allocator = gst_dmabuf_allocator_new();

    v4l2_start_capturing(data.v4l2_fd, 4, data.out_buffers);
    g_signal_connect(appsrc, "need-data", G_CALLBACK(on_need_data), &data);


    gst_bin_add_many(GST_BIN(pipeline), appsrc, capsfilter, mpph264enc, rtph264pay, udpsink, NULL);

    if (gst_element_link_many(appsrc, capsfilter, mpph264enc, rtph264pay, udpsink, NULL) != TRUE) {
        g_printerr("连接失败！可能是数据格式不兼容。\n");
        return -1;
    }

    // 创建主循环和总线监听（必须在 set_state 之前！）
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    data.loop = loop;

    GstBus *bus = gst_element_get_bus(pipeline);
    gst_bus_add_watch(bus, bus_call_back, &data);
    gst_object_unref(bus);

    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        g_printerr("无法将管道设置为播放状态！\n");
        gst_object_unref(pipeline);
        return -1;
    }

    g_print("管道已进入 PLAYING 状态，开始推流...\n");

    // 运行 GLib 主循环 (阻塞，直到调用 g_main_loop_quit)
    g_main_loop_run(loop);
    g_main_loop_run(loop);

    return 1;
}


int main(void) {
    setlocale(LC_ALL, "zh_CN.UTF-8");
    return gstreamer_init();
}