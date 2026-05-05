#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/video/video.h>
#include <unistd.h>
#include "v4l2_camera.h"
#include <locale.h>

#define VIDEO_WIDTH   3840
#define VIDEO_HEIGHT  2160
#define VIDEO_FPS     30

/* ============================================================================
 * CustomData — 推模式所需的所有上下文
 * ============================================================================ */
typedef struct _CustomData {
    GstElement   *pipeline;
    GstElement   *appsrc;           // 保存 appsrc 指针，供定时器回调 push 使用
    GMainLoop    *loop;
    GstAllocator *allocator;        // DMA-BUF 分配器

    /* V4L2 相关 */
    int                    v4l2_fd;
    struct dmabuf_buffer  *out_buffers;
    int                    num_buffers;
    __u32                  stride;
    __u32                  sizeimage;

    /* 帧计数，用于产生连续 PTS */
    guint64 frame_count;

    /* 退出标志 */
    gboolean running;
} CustomData;


/* ============================================================================
 * bus_call_back — 总线消息回调
 * ============================================================================ */
static gboolean bus_call_back(GstBus *bus, GstMessage *msg, gpointer user_data) {
    CustomData *data = (CustomData *)user_data;

    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError *err = NULL;
            gchar *debug_info = NULL;
            gst_message_parse_error(msg, &err, &debug_info);
            g_printerr("致命错误: 来自元件 %s: %s\n",
                       GST_OBJECT_NAME(msg->src), err->message);
            g_printerr("调试信息: %s\n", debug_info ? debug_info : "无");
            g_clear_error(&err);
            g_free(debug_info);
            g_main_loop_quit(data->loop);
            break;
        }
        case GST_MESSAGE_EOS:
            g_print("End-Of-Stream (EOS): 视频播放结束。\n");
            g_main_loop_quit(data->loop);
            break;
        case GST_MESSAGE_STATE_CHANGED: {
            if (GST_MESSAGE_SRC(msg) == GST_OBJECT(data->pipeline)) {
                GstState old_state, new_state, pending_state;
                gst_message_parse_state_changed(msg, &old_state, &new_state,
                                                &pending_state);
                g_print("管道状态从 %s 变为 %s\n",
                        gst_element_state_get_name(old_state),
                        gst_element_state_get_name(new_state));
            }
            break;
        }
        default:
            break;
    }
    return TRUE;
}


/* ============================================================================
 * push_one_frame — 把一帧 DMA-BUF 数据封装为 GstBuffer，推入 appsrc
 *
 * 这是推模式的核心：V4L2 只给出一个 dma-buf fd，
 * 我们必须通过 GstDmaBufAllocator 把它包装成 GStreamer 认识的 GstMemory，
 * 再加上 GstVideoMeta（告诉编码器 stride / offset / 格式），
 * 最后打上 PTS，调用 gst_app_src_push_buffer() 推入管道。
 * ============================================================================ */
static GstFlowReturn push_one_frame(CustomData *data,
                                    struct dmabuf_buffer *v4l2_buf)
{
    /* ---- 1. 复制 fd（GStreamer 会接管所有权）---- */
    int fd_dup = dup(v4l2_buf->planes[0].fd);
    if (fd_dup < 0) {
        g_printerr("dup DMA-BUF fd 失败！\n");
        return GST_FLOW_ERROR;
    }

    /* ---- 2. fd → GstMemory ---- */
    GstMemory *mem = gst_dmabuf_allocator_alloc(data->allocator,
                                                fd_dup, data->sizeimage);
    if (!mem) {
        g_printerr("从 dmabuf fd 创建 GstMemory 失败！\n");
        close(fd_dup);
        return GST_FLOW_ERROR;
    }

    GstBuffer *buffer = gst_buffer_new();
    gst_buffer_append_memory(buffer, mem);

    /* ---- 3. 添加 GstVideoMeta（告知编码器 NV12 的 stride 和 UV 偏移）---- */
    // NV12: Y 平面在前，UV 交错平面在后
    gint   strides[4] = { data->stride, data->stride, 0, 0 };
    gsize  offsets[4] = { 0, (gsize)data->sizeimage * 2 / 3, 0, 0 };

    gst_buffer_add_video_meta_full(
        buffer,
        GST_VIDEO_FRAME_FLAG_NONE,
        GST_VIDEO_FORMAT_NV12,
        VIDEO_WIDTH,
        VIDEO_HEIGHT,
        2,          // NV12 有 2 个平面
        offsets,
        strides);

    /* ---- 4. 打时间戳 ---- */
    GstClockTime pts = data->frame_count *
                       (GST_SECOND / VIDEO_FPS);
    GST_BUFFER_PTS(buffer)      = pts;
    GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale_int(1, GST_SECOND,
                                                            VIDEO_FPS);
    data->frame_count++;

    /* ---- 5. 推入 appsrc（推模式唯一入口）---- */
    GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(data->appsrc),
                                                buffer);
    if (ret != GST_FLOW_OK) {
        g_printerr("appsrc push buffer 失败: %s (%d)\n",
                   gst_flow_get_name(ret), ret);
    }
    return ret;
}


/* ============================================================================
 * capture_timeout_cb — GLib 定时器回调（推模式的"心跳"）
 *
 * 每隔 10ms 被主循环调用一次：
 *   1. 非阻塞 DQBUF 取一帧（无数据时立刻返回 NULL）
 *   2. 有帧 → push_one_frame → 归还 buffer 给 V4L2 (QBUF)
 *   3. 无帧 → 直接返回 TRUE，等下一次触发
 *
 * 返回 TRUE 让定时器继续；返回 FALSE 停止定时器。
 * ============================================================================ */
static gboolean capture_timeout_cb(gpointer user_data) {
    CustomData *data = (CustomData *)user_data;

    if (!data->running)
        return G_SOURCE_REMOVE;

    /* 非阻塞出队 */
    struct dmabuf_buffer *buf =
        v4l2_dequeue_frame_dmabuf(data->v4l2_fd, data->out_buffers);

    if (buf) {
        GstFlowReturn ret = push_one_frame(data, buf);

        /* 归还 buffer 给 V4L2 驱动（QBUF），以便下次填充 */
        v4l2_queue_frame_dmabuf(data->v4l2_fd, buf);

        if (ret == GST_FLOW_FLUSHING || ret == GST_FLOW_ERROR) {
            g_print("管道已停止，结束推流。\n");
            g_main_loop_quit(data->loop);
            return G_SOURCE_REMOVE;
        }
    }
    /* 如果 buf == NULL（EAGAIN），什么都不做，等下一轮超时 */

    return G_SOURCE_CONTINUE;
}


/* ============================================================================
 * gstreamer_init — 初始化 pipeline，启动推模式主循环
 * ============================================================================ */
int gstreamer_init(void) {
    gst_init(NULL, NULL);

    CustomData data;
    memset(&data, 0, sizeof(data));
    data.running = TRUE;

    /* ---- V4L2 初始化 ---- */
    data.v4l2_fd = v4l2_open_device("/dev/video11");

    unsigned int  out_sizes[1]   = {0};
    unsigned char out_num_planes = 0;
    v4l2_set_format_mplane(data.v4l2_fd, VIDEO_WIDTH, VIDEO_HEIGHT,
                           V4L2_PIX_FMT_NV12, out_sizes, &out_num_planes);
    v4l2_get_format_mplane(data.v4l2_fd, 0, &data.stride, &data.sizeimage);

    size_t plane_sizes[1] = { data.sizeimage };
    data.num_buffers = 4;
    data.out_buffers = (struct dmabuf_buffer *)
        calloc(data.num_buffers, sizeof(struct dmabuf_buffer));
    v4l2_init_dmabuf(data.v4l2_fd, data.num_buffers, 1,
                     plane_sizes, &data.out_buffers);

    /* ---- 创建 GStreamer 元件 ---- */
    GstElement *appsrc     = gst_element_factory_make("appsrc",     "v4l2out");
    GstElement *capsfilter = gst_element_factory_make("capsfilter", "capsfilter");
    GstElement *mpph264enc = gst_element_factory_make("mpph264enc", "mpph264enc");
    GstElement *rtph264pay = gst_element_factory_make("rtph264pay", "rtph264pay");
    GstElement *udpsink    = gst_element_factory_make("udpsink",    "udpsink");

    if (!appsrc || !capsfilter || !mpph264enc || !rtph264pay || !udpsink) {
        g_printerr("有元件创建失败，请检查插件是否安装！\n");
        return -1;
    }

    /* ---- Caps ---- */
    GstCaps *caps = gst_caps_new_simple("video/x-raw",
        "width",     G_TYPE_INT,  VIDEO_WIDTH,
        "height",    G_TYPE_INT,  VIDEO_HEIGHT,
        "format",    G_TYPE_STRING, "NV12",
        "framerate", GST_TYPE_FRACTION, VIDEO_FPS, 1,
        NULL);
    g_object_set(capsfilter, "caps", caps, NULL);
    gst_caps_unref(caps);

    /* ========================================================================
     * appsrc 推模式关键属性：
     *
     *   "is-live"  = TRUE   → 告诉下游这是个实时源，不要缓冲太多
     *   "format"   = GST_FORMAT_TIME → 我们手动提供 PTS/DURATION
     *   "do-timestamp" = FALSE（默认）→ 不自动打时间戳，用我们设置的
     *   "block"    = TRUE （默认）→ 下游处理不过来时 push 会阻塞（反压）
     * ======================================================================== */
    g_object_set(appsrc,
        "is-live", TRUE,
        "format",  GST_FORMAT_TIME,
        NULL);

    g_object_set(rtph264pay, "config-interval", 1, NULL);
    g_object_set(udpsink, "host", "192.168.1.6", "port", 5000, NULL);

    /* ---- Pipeline ---- */
    GstElement *pipeline = gst_pipeline_new("my_player_pipeline");
    if (!pipeline) {
        g_printerr("pipeline 创建失败！\n");
        return -1;
    }
    data.pipeline = pipeline;
    data.appsrc   = appsrc;

    /* ---- DMA-BUF 分配器 ---- */
    data.allocator = gst_dmabuf_allocator_new();

    /* ---- 组装 pipeline ---- */
    gst_bin_add_many(GST_BIN(pipeline),
                     appsrc, capsfilter, mpph264enc, rtph264pay, udpsink, NULL);

    if (!gst_element_link_many(appsrc, capsfilter, mpph264enc,
                                rtph264pay, udpsink, NULL)) {
        g_printerr("元件连接失败！可能是数据格式不兼容。\n");
        return -1;
    }

    /* ---- 总线监听 ---- */
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    data.loop = loop;

    GstBus *bus = gst_element_get_bus(pipeline);
    gst_bus_add_watch(bus, bus_call_back, &data);
    gst_object_unref(bus);

    /* ---- 启动 V4L2 流 ---- */
    v4l2_start_capturing(data.v4l2_fd, data.num_buffers, data.out_buffers);

    /* ---- 启动 pipeline ---- */
    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        g_printerr("无法将管道设置为 PLAYING 状态！\n");
        gst_object_unref(pipeline);
        return -1;
    }
    g_print("管道已进入 PLAYING 状态，开始推流...\n");

    /* ========================================================================
     * 【推模式核心】注册 GLib 定时器，每 10ms 轮询一次 V4L2
     *
     *   推模式不需要 appsrc 的 "need-data" 信号（那是拉模式）。
     *   我们自己掌控节奏：定时器 → DQBUF → push_buffer → QBUF → 循环。
     * ======================================================================== */
    guint timer_id = g_timeout_add(10,                    // 10ms 间隔
                                   capture_timeout_cb,    // 回调函数
                                   &data);                // 用户数据

    /* ---- 运行主循环（阻塞在此）---- */
    g_main_loop_run(loop);

    /* ---- 清理 ---- */
    g_print("正在停止推流...\n");
    data.running = FALSE;
    g_source_remove(timer_id);

    gst_element_set_state(pipeline, GST_STATE_NULL);
    v4l2_stop_capturing(data.v4l2_fd);
    v4l2_close_device(data.v4l2_fd);
    gst_object_unref(pipeline);
    g_main_loop_unref(loop);
    gst_object_unref(data.allocator);

    // 释放 dmabuf 资源
    for (int i = 0; i < data.num_buffers; i++) {
        close(data.out_buffers[i].planes[0].fd);
    }
    free(data.out_buffers);

    return 0;
}

int main(void) {
    setlocale(LC_ALL, "zh_CN.UTF-8");
    return gstreamer_init();
}