#ifndef _INFERENCE_H_   
#define _INFERENCE_H_

#include <vector>
#include <stdint.h>
#include "rknn_api.h"
#include "postprocess.h"

class rknn_model {
private:
    rknn_context ctx = 0;
    rknn_tensor_mem *input_mem = nullptr;
    rknn_input_output_num io_num;
    std::vector<rknn_tensor_attr> output_attrs;
    std::vector<rknn_tensor_mem *> output_mem;
    int channel = 3;
    int width = 0;
    int height = 0;
    const float nms_threshold = NMS_THRESH;
    const float box_conf_threshold = BOX_THRESH;

public:
    int init(void);
    int run(void);
    void destroy_model(void);
    int get_dma_fd(void);
};

#endif