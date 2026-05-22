#include "inference.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rknn_api.h"
#include "postprocess.h"

int rknn_model::init(void) {
    int ret = 0;

    // ==========================================
    // 第一步：初始化 (rknn_init)
    // ==========================================

    int model_size = 0;
    
    // 调用 rknn_init，把模型加载进内存并分配给NPU
    ret = rknn_init(&ctx, (char *)"/home/cat/gst_core/src/model/yolov5s-640-640.rknn", model_size, 0, NULL);
    if (ret < 0) {
        printf("rknn_init fail! ret=%d\n", ret);
        return -1;
    }


    // ==========================================
    // 第二步：查询模型信息 (rknn_query)
    // ==========================================
    // 获取输入输出数量
    
    rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    printf("模型有 %d 个输入, %d 个输出\n", io_num.n_input, io_num.n_output);

    // 获取输入属性（比如分辨率，通道数，数据格式等）
    rknn_tensor_attr input_attrs[io_num.n_input];
    for (int i = 0; i < io_num.n_input; i++) {
        input_attrs[i].index = i;
        rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &(input_attrs[i]), sizeof(rknn_tensor_attr));
        // 这里你可以打印 input_attrs[i].dims 看看是不是 1x640x640x3
        printf("yolo:%d\n",input_attrs[i].dims);

        printf("num:%d\n",io_num.n_input);
    }
    input_mem = rknn_create_mem(ctx, input_attrs[0].size_with_stride);
    // 将这块内存设置为模型的输入
    ret = rknn_set_io_mem(ctx, input_mem, &input_attrs[0]);
    if (ret < 0) {
        printf("rknn_set_io_mem (input) failed! ret=%d\n", ret);
        return -1;
    }

    
    // rknn_tensor_attr output_attrs[io_num.n_output]; 
    output_attrs.resize(io_num.n_output);
    output_mem.resize(io_num.n_output);
    for (int i = 0; i < io_num.n_output; i++) {
        output_attrs[i].index = i;
        rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &(output_attrs[i]), sizeof(rknn_tensor_attr));
        // 这里你可以打印 output_attrs[i].dims 看看是不是 1x640x640x3
        printf("yolo:%d\n",output_attrs[i].dims);

        output_mem[i] = rknn_create_mem(ctx, output_attrs[i].size_with_stride);

        ret = rknn_set_io_mem(ctx, output_mem[i], &output_attrs[i]);
        if (ret < 0) {
            printf("rknn_set_io_mem (input) failed! ret=%d\n", ret);
            return -1;
        }
    }


    if (input_attrs[0].fmt == RKNN_TENSOR_NCHW)
    {
        printf("model is NCHW input fmt\n");
        channel = input_attrs[0].dims[1];
        height = input_attrs[0].dims[2];
        width = input_attrs[0].dims[3];
    }
    else
    {
        printf("model is NHWC input fmt\n");
        height = input_attrs[0].dims[1];
        width = input_attrs[0].dims[2];
        channel = input_attrs[0].dims[3];
    }

    return 0;
}

int rknn_model::run(void) {
    int ret = 0;
    // ==========================================
    // 第四步：执行推理 (rknn_run)
    // ==========================================
    // NPU 开始干活，默认是阻塞的，跑完才返回

    rknn_mem_sync(ctx, input_mem, RKNN_MEMORY_SYNC_TO_DEVICE);

    ret = rknn_run(ctx, NULL);
    if (ret < 0) {
        printf("rknn_run fail! ret=%d\n", ret);
        return ret;
    }

    for (int i = 0; i < io_num.n_output; ++i) {
        rknn_mem_sync(ctx, output_mem[i], RKNN_MEMORY_SYNC_FROM_DEVICE);
    }

    // 后处理
    detect_result_group_t detect_result_group;
    std::vector<float> out_scales;
    std::vector<int32_t> out_zps;
    for (int i = 0; i < io_num.n_output; ++i)
    {
        out_scales.push_back(output_attrs[i].scale);
        out_zps.push_back(output_attrs[i].zp);
    }

    BOX_RECT pads;
    memset(&pads, 0, sizeof(BOX_RECT));
    float scale_w = (float)640.0 / 3840.0;
    float scale_h = (float)640.0 / 2160.0;

    post_process((int8_t *)output_mem[0]->virt_addr, (int8_t *)output_mem[1]->virt_addr, (int8_t *)output_mem[2]->virt_addr, height, width,
    box_conf_threshold, nms_threshold, pads, scale_w, scale_h, out_zps, out_scales, &detect_result_group);
printf("detect_result_group.count=%d\n",detect_result_group.count);
    // 画框和概率
    char text[256];
    for (int i = 0; i < detect_result_group.count; i++)
    {
        detect_result_t *det_result = &(detect_result_group.results[i]);
        sprintf(text, "%s %.1f%%", det_result->name, det_result->prop * 100);
        printf("%s @ (%d %d %d %d) %f\n", det_result->name, det_result->box.left, det_result->box.top,
        det_result->box.right, det_result->box.bottom, det_result->prop);
        int x1 = det_result->box.left;
        int y1 = det_result->box.top;
        int x2 = det_result->box.right;
        int y2 = det_result->box.bottom;

        printf("x1=%d,y1=%d\nx2=%d,y2=%d\n",x1,y1,x2,y2);
    }

    return 0;
}

void rknn_model::destroy_model(void) {
    rknn_destroy_mem(ctx, input_mem);
    for (int i = 0; i < io_num.n_output; i++) {
        rknn_destroy_mem(ctx, output_mem[i]);
    }
    
    // 销毁上下文
    rknn_destroy(ctx);
}

int rknn_model::get_dma_fd(void) {
    if (ctx == 0) {
        printf("rknn 初始化失败!\n");
        return -1;
    }
    return input_mem->fd;
}
