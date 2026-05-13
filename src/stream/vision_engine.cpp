#include "vision_engine.hpp"

// 在 cpp 文件中尽情引入底层库
#include <gst/gst.h>
// #include <gst/app/gstappsink.h>
#include <gst/allocators/gstdmabuf.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <iostream>
#include <queue>
#include "v4l2_camera.h"
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>
#include <gst/app/gstappsink.h>
#include <poll.h>

#define VIDEO_WIDTH   3840
#define VIDEO_HEIGHT  2160
#define VIDEO_FPS     30






// 定义隐藏的实现类
class VisionEngine::Impl {
public:
    // GStreamer 资源
    GstElement* pipeline{nullptr};
    GstElement* appsrc{nullptr};
    GstElement* appsink{nullptr};
    GstElement* webrtcbin{nullptr};
    GMainLoop*  main_loop{nullptr};
    GstAllocator* allocator{nullptr};

    int     v4l2_fd{-1};
    __u32   stride{0};
    __u32   sizeimage{0};
    struct  dmabuf_buffer  out_buffers[4]{0};
    int     num_buffers{4};
    guint64 frame_count{0};


    // 线程管理
    std::thread appsrc_thread;
    std::thread loop_thread;         // 专门跑 GMainLoop 处理异步事件
    std::thread inference_thread;    // 专门跑 YOLO 推理
    std::atomic<bool> is_running{false};

    // 外部注册的回调
    VisionEngine::InferenceCallback inference_cb_;

    // appsink 到 inference 线程的图像队列和锁
    std::mutex frame_mutex_;
    std::condition_variable frame_cv_;
    // 假设用一个简单的结构体保存图像地址，实际应用中这里传的是 DMA-BUF fd 或 mmap 的内存指针
    std::queue<void*> frame_queue_; 

    Impl() {
        gst_init(nullptr, nullptr);
    }

    ~Impl() {
        stop();
    }

    bool init() {
        gst_init(NULL, NULL);

        /* ---- V4L2 初始化 ---- */
        v4l2_fd = v4l2_open_device("/dev/video11");
        if (v4l2_fd < 0) {
            return false;
        }
        
        
        
        
        unsigned int  out_sizes[1]   = {0};
        unsigned char out_num_planes = 0;
        v4l2_set_format_mplane(v4l2_fd, VIDEO_WIDTH, VIDEO_HEIGHT,
                            V4L2_PIX_FMT_NV12, out_sizes, &out_num_planes);
        v4l2_get_format_mplane(v4l2_fd, 0, &stride, &sizeimage);

        size_t plane_sizes[1] = { sizeimage };


        v4l2_init_dmabuf(v4l2_fd, num_buffers, 1, plane_sizes, out_buffers);

        /* ---- 创建 GStreamer 元件 ---- */
        appsrc     = gst_element_factory_make("appsrc",     "v4l2out");
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
        g_object_set(udpsink, "host", "192.168.1.5", "port", 5000, NULL);

        /* ---- Pipeline ---- */
        pipeline = gst_pipeline_new("my_player_pipeline");
        if (!pipeline) {
            g_printerr("pipeline 创建失败！\n");
            return -1;
        }
        // data.appsrc   = appsrc;

        /* ---- DMA-BUF 分配器 ---- */
        allocator = gst_dmabuf_allocator_new();

        /* ---- 组装 pipeline ---- */
        gst_bin_add_many(GST_BIN(pipeline),
                        appsrc, capsfilter, mpph264enc, rtph264pay, udpsink, NULL);

        if (!gst_element_link_many(appsrc, capsfilter, mpph264enc,
                                    rtph264pay, udpsink, NULL)) {
            g_printerr("元件连接失败！可能是数据格式不兼容。\n");
            return -1;
        }

        GstBus *bus = gst_element_get_bus(pipeline);
        gst_bus_add_watch(bus, bus_callback_wrapper, this);
        gst_object_unref(bus);

        return true;
    }

    void start() {
        if (is_running) return;
        is_running = true;

        /* ---- 启动 V4L2 流 ---- */
        v4l2_start_capturing(v4l2_fd, num_buffers, out_buffers);

        /* ---- 启动 pipeline ---- */
        GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
        if (ret == GST_STATE_CHANGE_FAILURE) {
            g_printerr("无法将管道设置为 PLAYING 状态！\n");
            gst_object_unref(pipeline);
            // return -1;
        }
        g_print("管道已进入 PLAYING 状态，开始推流...\n");

        appsrc_thread = std::thread(&Impl::appsrc_push_task,this);


        // 启动 GMainLoop 线程 (必须有这个，否则 WebRTC 信令和总线消息不工作)
        main_loop = g_main_loop_new(nullptr, FALSE); // 监听总线
        loop_thread = std::thread([this]() {
            g_main_loop_run(main_loop);
        });

        // 3. 启动旁路推理线程
        inference_thread = std::thread(&Impl::inference_task, this);
    }

    void stop() {
        if (!is_running) return;
        is_running = false;

        // 通知推理线程退出
        frame_cv_.notify_all();
        if (inference_thread.joinable()) {
            inference_thread.join();
        }

        // 停止 GStreamer 和 GMainLoop
        if (pipeline) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
        }
        if (main_loop) {
            g_main_loop_quit(main_loop);
            if (loop_thread.joinable()) {
                loop_thread.join();
            }
            g_main_loop_unref(main_loop);
            main_loop = nullptr;
        }
        // ... 清理其他对象
    }

    // ------------------- 核心机制 -------------------

    // 这是 appsink 的 C 风格回调，我们通过 user_data 把 C++ 的 this 指针传进来
    static GstFlowReturn on_new_sample_static(GstElement* sink, gpointer user_data) {
        auto* self = static_cast<Impl*>(user_data);
        return self->on_new_sample();
    }

    // 真正的 C++ 处理逻辑：获取图像，丢入队列，唤醒推理线程
    GstFlowReturn on_new_sample() {
        GstSample* sample = gst_app_sink_pull_sample(GST_APP_SINK(appsink));
        if (sample) {
            // 解析 sample 获取 DMA-BUF fd 或指针 (此处省略细节)
            void* frame_data = nullptr; // 伪代码
            
            {
                std::lock_guard<std::mutex> lock(frame_mutex_);
                // 如果队列积压太多（推理太慢），丢弃旧帧保证实时性
                if (frame_queue_.size() > 2) {
                    frame_queue_.pop(); 
                }
                frame_queue_.push(frame_data);
            }
            frame_cv_.notify_one(); // 唤醒推理线程
            gst_sample_unref(sample);
        }
        return GST_FLOW_OK;
    }

    // 独立的推理线程
    void inference_task() {
        // 在这里初始化 RKNN 模型
        // init_rknn_model();

        while (is_running) {
            void* frame_data = nullptr;
            {
                std::unique_lock<std::mutex> lock(frame_mutex_);
                frame_cv_.wait(lock, [this]{ return !frame_queue_.empty() || !is_running; });
                if (!is_running) break;

                frame_data = frame_queue_.front();
                frame_queue_.pop();
            }

            // 1. 调用 RKNN 执行 YOLO 推理
            // std::vector<BoundingBox> results = run_rknn(frame_data);
            
            // 测试用的伪数据
            std::vector<BoundingBox> results = {{10, 10, 50, 50, 0, 0.95}};

            // 2. 如果外部注册了回调，把结果甩出去 (甩给 ROS 2)
            if (inference_cb_) {
                inference_cb_(results);
            }
        }
    }


    void appsrc_push_task() {
        struct pollfd poll_fd[1];
        poll_fd[0].fd = v4l2_fd;
        poll_fd[0].events = POLLIN;
        struct dmabuf_buffer *buf = nullptr;
        while (is_running)
        {
            int ret = poll(poll_fd, 1, 500);
            if (ret < 0) {
                perror("poll error");
                break;
            }

            if (poll_fd[0].revents & POLLIN) {
                /* 非阻塞出队 */
                buf = v4l2_dequeue_frame_dmabuf(v4l2_fd, out_buffers);

                if (buf) {
                    GstFlowReturn ret = push_one_frame(buf);

                    /* 归还 buffer 给 V4L2 驱动（QBUF），以便下次填充 */
                    v4l2_queue_frame_dmabuf(v4l2_fd, buf);

                    if (ret == GST_FLOW_FLUSHING || ret == GST_FLOW_ERROR) {
                        g_print("管道已停止，结束推流。\n");
                        g_main_loop_quit(main_loop);
                        is_running = false;
                    }
                }
                /* 如果 buf == NULL（EAGAIN），什么都不做，等下一轮超时 */
            }
        }
    }



private:

    // 静态中转函数（符合 C 语言签名）
    static gboolean bus_callback_wrapper(GstBus *bus, GstMessage *msg, gpointer user_data) {
        // 3. 将 user_data 强转回 Impl 对象的指针 (this)
        Impl* self = static_cast<Impl*>(user_data);
        // 4. 调用真正的普通成员函数
        return self->bus_call_back(bus, msg); 
    }

    /* ============================================================================
    * bus_call_back — 总线消息回调
    * ============================================================================ */
    gboolean bus_call_back(GstBus *bus, GstMessage *msg) {

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
                g_main_loop_quit(main_loop);
                break;
            }
            case GST_MESSAGE_EOS:
                g_print("End-Of-Stream (EOS): 视频播放结束。\n");
                g_main_loop_quit(main_loop);
                break;
            case GST_MESSAGE_STATE_CHANGED: {
                if (GST_MESSAGE_SRC(msg) == GST_OBJECT(pipeline)) {
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
    GstFlowReturn push_one_frame(struct dmabuf_buffer *v4l2_buf)
    {
        /* ---- 1. 复制 fd（GStreamer 会接管所有权）---- */
        int fd_dup = dup(v4l2_buf->planes[0].fd);
        if (fd_dup < 0) {
            g_printerr("dup DMA-BUF fd 失败！\n");
            return GST_FLOW_ERROR;
        }

        /* ---- 2. fd → GstMemory ---- */
        GstMemory *mem = gst_dmabuf_allocator_alloc(allocator,
                                                    fd_dup, sizeimage);
        if (!mem) {
            g_printerr("从 dmabuf fd 创建 GstMemory 失败！\n");
            close(fd_dup);
            return GST_FLOW_ERROR;
        }

        GstBuffer *buffer = gst_buffer_new();
        gst_buffer_append_memory(buffer, mem);

        /* ---- 3. 添加 GstVideoMeta（告知编码器 NV12 的 stride 和 UV 偏移）---- */
        // NV12: Y 平面在前，UV 交错平面在后
        gint   strides[4] = { stride, stride, 0, 0 };
        gsize  offsets[4] = { 0, (gsize)sizeimage * 2 / 3, 0, 0 };

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
        GstClockTime pts = frame_count *
                        (GST_SECOND / VIDEO_FPS);
        GST_BUFFER_PTS(buffer)      = pts;
        GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale_int(1, GST_SECOND,
                                                                VIDEO_FPS);
        frame_count++;

        /* ---- 5. 推入 appsrc（推模式唯一入口）---- */
        GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(appsrc),
                                                    buffer);
        if (ret != GST_FLOW_OK) {
            g_printerr("appsrc push buffer 失败: %s (%d)\n",
                    gst_flow_get_name(ret), ret);
        }
        return ret;
    }





};

// =======================================================================
// 外部调用接口：仅仅是将请求转发给底层的 pimpl_
// =======================================================================

VisionEngine::VisionEngine() : pimpl_(std::make_unique<Impl>()) {}
VisionEngine::~VisionEngine() = default; // 必须在 cpp 文件中实现，因为此时 Impl 才可见

bool VisionEngine::init() { return pimpl_->init(); }
bool VisionEngine::start() { pimpl_->start(); return true; }
void VisionEngine::stop() { pimpl_->stop(); }

void VisionEngine::set_inference_callback(InferenceCallback cb) {
    pimpl_->inference_cb_ = std::move(cb);
}

void VisionEngine::set_remote_description(const std::string& type, const std::string& sdp) {
    // 调用底层的 webrtcbin 相关逻辑...
}