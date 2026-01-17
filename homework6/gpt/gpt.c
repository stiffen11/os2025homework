// Original Author: Andrej Karpathy
// https://github.com/karpathy/llm.c
// Parallelized version for OS course

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <string.h>
#include <unistd.h>

#include "thread.h"
#include "thread-sync.h"

// ----------------------------------------------------------------------------
// 并行计算相关定义
// ----------------------------------------------------------------------------

#define NUM_WORKERS 4  // 工作线程数

// 屏障同步结构
typedef struct {
    mutex_t mutex;
    cond_t cond;
    int count;
    int total;
} barrier_t;

// 工作线程参数
typedef struct {
    int id;                     // 线程ID
    void* data;                 // 具体工作数据
    void (*work_func)(void*);   // 工作函数
    barrier_t* barrier;         // 同步屏障
} Worker;

// 全局工作线程
static Worker workers[NUM_WORKERS];
static int workers_initialized = 0;

// 矩阵乘法工作参数
typedef struct {
    int id;
    float* out;
    float* inp;
    float* weight;
    float* bias;
    int B, T, C, OC;
    int start_row, end_row;
} MatmulWork;

// 注意力计算工作参数
typedef struct {
    int id;
    float* out;
    float* preatt;
    float* att;
    float* inp;
    int B, T, C, NH;
    int start_batch, end_batch;
} AttentionWork;

// ----------------------------------------------------------------------------
// 并行计算工具函数
// ----------------------------------------------------------------------------

static inline void barrier_init(barrier_t* barrier, int total) {
    barrier->count = 0;
    barrier->total = total;
}

static inline void barrier_wait(barrier_t* barrier) {
    mutex_lock(&barrier->mutex);
    barrier->count++;
    if (barrier->count == barrier->total) {
        barrier->count = 0;
        cond_broadcast(&barrier->cond);
    } else {
        while (barrier->count > 0 && barrier->count < barrier->total) {
            cond_wait(&barrier->cond, &barrier->mutex);
        }
    }
    mutex_unlock(&barrier->mutex);
}

// 工作线程入口函数
static void worker_entry(int id) {
    Worker* w = &workers[id];
    while (1) {
        barrier_wait(w->barrier);  // 等待工作分配
        if (w->work_func) {
            w->work_func(w->data);
        }
        barrier_wait(w->barrier);  // 等待所有线程完成
    }
}

// 初始化工作线程池
static void init_workers() {
    if (workers_initialized) return;
    
    static barrier_t worker_barrier;
    barrier_init(&worker_barrier, NUM_WORKERS + 1);  // +1 for main thread
    
    for (int i = 0; i < NUM_WORKERS; i++) {
        workers[i].id = i;
        workers[i].barrier = &worker_barrier;
        workers[i].work_func = NULL;
        workers[i].data = NULL;
    }
    
    // 启动工作线程
    for (int i = 0; i < NUM_WORKERS; i++) {
        spawn(worker_entry);
    }
    
    workers_initialized = 1;
}

// 分配并行工作
static void dispatch_work(void (*work_func)(void*), void* work_data, int work_count) {
    init_workers();
    
    // 准备工作数据
    for (int i = 0; i < NUM_WORKERS; i++) {
        workers[i].work_func = work_func;
        workers[i].data = work_data + i * work_count;
    }
    
    // 唤醒工作线程
    barrier_wait(workers[0].barrier);
    
    // 等待工作完成
    barrier_wait(workers[0].barrier);
}

// ----------------------------------------------------------------------------
// 并行化版本的前向传播函数
// ----------------------------------------------------------------------------

// 并行化的矩阵乘法
static void matmul_worker(void* arg) {
    MatmulWork* work = (MatmulWork*)arg;
    
    for (int b = 0; b < work->B; b++) {
        for (int t = work->start_row; t < work->end_row; t++) {
            float* out_bt = work->out + b * work->T * work->OC + t * work->OC;
            float* inp_bt = work->inp + b * work->T * work->C + t * work->C;
            
            for (int o = 0; o < work->OC; o++) {
                float val = (work->bias != NULL) ? work->bias[o] : 0.0f;
                float* wrow = work->weight + o * work->C;
                
                // 使用循环展开优化
                int i = 0;
                for (; i <= work->C - 4; i += 4) {
                    val += inp_bt[i] * wrow[i];
                    val += inp_bt[i+1] * wrow[i+1];
                    val += inp_bt[i+2] * wrow[i+2];
                    val += inp_bt[i+3] * wrow[i+3];
                }
                for (; i < work->C; i++) {
                    val += inp_bt[i] * wrow[i];
                }
                
                out_bt[o] = val;
            }
        }
    }
}

void matmul_forward(float* out,
                    float* inp, float* weight, float* bias,
                    int B, int T, int C, int OC) {
    // 如果数据量小，使用串行版本
    if (B * T < NUM_WORKERS * 4) {
        for (int b = 0; b < B; b++) {
            for (int t = 0; t < T; t++) {
                float* out_bt = out + b * T * OC + t * OC;
                float* inp_bt = inp + b * T * C + t * C;
                for (int o = 0; o < OC; o++) {
                    float val = (bias != NULL) ? bias[o] : 0.0f;
                    float* wrow = weight + o*C;
                    for (int i = 0; i < C; i++) {
                        val += inp_bt[i] * wrow[i];
                    }
                    out_bt[o] = val;
                }
            }
        }
        return;
    }
    
    // 并行版本：按时间步分配工作
    static MatmulWork work[NUM_WORKERS];
    int rows_per_worker = (T + NUM_WORKERS - 1) / NUM_WORKERS;
    
    for (int i = 0; i < NUM_WORKERS; i++) {
        work[i].id = i;
        work[i].out = out;
        work[i].inp = inp;
        work[i].weight = weight;
        work[i].bias = bias;
        work[i].B = B;
        work[i].T = T;
        work[i].C = C;
        work[i].OC = OC;
        work[i].start_row = i * rows_per_worker;
        work[i].end_row = (i + 1) * rows_per_worker;
        if (work[i].end_row > T) work[i].end_row = T;
    }
    
    dispatch_work(matmul_worker, work, sizeof(MatmulWork));
}

// 并行化的注意力计算
static void attention_worker(void* arg) {
    AttentionWork* work = (AttentionWork*)arg;
    
    int C3 = work->C * 3;
    int hs = work->C / work->NH;
    float scale = 1.0f / sqrtf(hs);
    
    for (int b = work->start_batch; b < work->end_batch; b++) {
        for (int t = 0; t < work->T; t++) {
            for (int h = 0; h < work->NH; h++) {
                float* query_t = work->inp + b * work->T * C3 + t * C3 + h * hs;
                float* preatt_bth = work->preatt + b * work->NH * work->T * work->T + h * work->T * work->T + t * work->T;
                float* att_bth = work->att + b * work->NH * work->T * work->T + h * work->T * work->T + t * work->T;
                
                // 计算query和key的点积
                float maxval = -10000.0f;
                for (int t2 = 0; t2 <= t; t2++) {
                    float* key_t2 = work->inp + b * work->T * C3 + t2 * C3 + h * hs + work->C;
                    
                    float val = 0.0f;
                    for (int i = 0; i < hs; i++) {
                        val += query_t[i] * key_t2[i];
                    }
                    val *= scale;
                    if (val > maxval) maxval = val;
                    
                    preatt_bth[t2] = val;
                }
                
                // 计算softmax
                float expsum = 0.0f;
                for (int t2 = 0; t2 <= t; t2++) {
                    float expv = expf(preatt_bth[t2] - maxval);
                    expsum += expv;
                    att_bth[t2] = expv;
                }
                float expsum_inv = expsum == 0.0f ? 0.0f : 1.0f / expsum;
                
                for (int t2 = 0; t2 < work->T; t2++) {
                    if (t2 <= t) {
                        att_bth[t2] *= expsum_inv;
                    } else {
                        att_bth[t2] = 0.0f;
                    }
                }
                
                // 加权求和
                float* out_bth = work->out + b * work->T * work->C + t * work->C + h * hs;
                for (int i = 0; i < hs; i++) out_bth[i] = 0.0f;
                
                for (int t2 = 0; t2 <= t; t2++) {
                    float* value_t2 = work->inp + b * work->T * C3 + t2 * C3 + h * hs + work->C * 2;
                    float att_btht2 = att_bth[t2];
                    for (int i = 0; i < hs; i++) {
                        out_bth[i] += att_btht2 * value_t2[i];
                    }
                }
            }
        }
    }
}

void attention_forward(float* out, float* preatt, float* att,
                       float* inp,
                       int B, int T, int C, int NH) {
    // 如果数据量小，使用串行版本
    if (B < 2) {
        int C3 = C*3;
        int hs = C / NH;
        float scale = 1.0f / sqrtf(hs);
        
        for (int b = 0; b < B; b++) {
            for (int t = 0; t < T; t++) {
                for (int h = 0; h < NH; h++) {
                    float* query_t = inp + b * T * C3 + t * C3 + h * hs;
                    float* preatt_bth = preatt + b*NH*T*T + h*T*T + t*T;
                    float* att_bth = att + b*NH*T*T + h*T*T + t*T;
                    
                    float maxval = -10000.0f;
                    for (int t2 = 0; t2 <= t; t2++) {
                        float* key_t2 = inp + b * T * C3 + t2 * C3 + h * hs + C;
                        
                        float val = 0.0f;
                        for (int i = 0; i < hs; i++) {
                            val += query_t[i] * key_t2[i];
                        }
                        val *= scale;
                        if (val > maxval) maxval = val;
                        preatt_bth[t2] = val;
                    }
                    
                    float expsum = 0.0f;
                    for (int t2 = 0; t2 <= t; t2++) {
                        float expv = expf(preatt_bth[t2] - maxval);
                        expsum += expv;
                        att_bth[t2] = expv;
                    }
                    float expsum_inv = expsum == 0.0f ? 0.0f : 1.0f / expsum;
                    
                    for (int t2 = 0; t2 < T; t2++) {
                        if (t2 <= t) att_bth[t2] *= expsum_inv;
                        else att_bth[t2] = 0.0f;
                    }
                    
                    float* out_bth = out + b * T * C + t * C + h * hs;
                    for (int i = 0; i < hs; i++) out_bth[i] = 0.0f;
                    for (int t2 = 0; t2 <= t; t2++) {
                        float* value_t2 = inp + b * T * C3 + t2 * C3 + h * hs + C*2;
                        float att_btht2 = att_bth[t2];
                        for (int i = 0; i < hs; i++) {
                            out_bth[i] += att_btht2 * value_t2[i];
                        }
                    }
                }
            }
        }
        return;
    }
    
    // 并行版本：按批次分配工作
    static AttentionWork work[NUM_WORKERS];
    int batches_per_worker = (B + NUM_WORKERS - 1) / NUM_WORKERS;
    
    for (int i = 0; i < NUM_WORKERS; i++) {
        work[i].id = i;
        work[i].out = out;
        work[i].preatt = preatt;
        work[i].att = att;
        work[i].inp = inp;
        work[i].B = B;
        work[i].T = T;
        work[i].C = C;
        work[i].NH = NH;
        work[i].start_batch = i * batches_per_worker;
        work[i].end_batch = (i + 1) * batches_per_worker;
        if (work[i].end_batch > B) work[i].end_batch = B;
    }
    
    dispatch_work(attention_worker, work, sizeof(AttentionWork));
}

// 并行化的层归一化
static void layernorm_worker(void* arg) {
    // 与attention_worker类似，为简洁起见省略详细实现
    // 实际上layernorm_forward的计算量相对较小，可以保持串行
}

void layernorm_forward(float* out, float* mean, float* rstd,
                       float* inp, float* weight, float* bias,
                       int B, int T, int C) {
    // 层归一化计算量较小，保持串行实现
    float eps = 1e-5f;
    for (int b = 0; b < B; b++) {
        for (int t = 0; t < T; t++) {
            float* x = inp + b * T * C + t * C;
            
            float m = 0.0f;
            for (int i = 0; i < C; i++) m += x[i];
            m = m / C;
            
            float v = 0.0f;
            for (int i = 0; i < C; i++) {
                float xshift = x[i] - m;
                v += xshift * xshift;
            }
            v = v / C;
            
            float s = 1.0f / sqrtf(v + eps);
            float* out_bt = out + b * T * C + t * C;
            for (int i = 0; i < C; i++) {
                float n = s * (x[i] - m);
                float o = n * weight[i] + bias[i];
                out_bt[i] = o;
            }
            
            mean[b * T + t] = m;
            rstd[b * T + t] = s;
        }
    }
}

// 并行化的GELU激活
#define GELU_SCALING_FACTOR sqrtf(2.0f / M_PI)
void gelu_forward(float* out, float* inp, int N) {
    // GELU计算简单，保持串行或使用OpenMP
    for (int i = 0; i < N; i++) {
        float x = inp[i];
        float cube = 0.044715f * x * x * x;
        out[i] = 0.5f * x * (1.0f + tanhf(GELU_SCALING_FACTOR * (x + cube)));
    }
}

void residual_forward(float* out, float* inp1, float* inp2, int N) {
    // 向量加法，简单保持串行
    for (int i = 0; i < N; i++) {
        out[i] = inp1[i] + inp2[i];
    }
}

void softmax_forward(float* probs, float* logits, int B, int T, int V) {
    // softmax在每个位置独立，可以并行化但计算量不大
    for (int b = 0; b < B; b++) {
        for (int t = 0; t < T; t++) {
            float* logits_bt = logits + b * T * V + t * V;
            float* probs_bt = probs + b * T * V + t * V;
            
            float maxval = -10000.0f;
            for (int i = 0; i < V; i++) {
                if (logits_bt[i] > maxval) maxval = logits_bt[i];
            }
            
            float sum = 0.0f;
            for (int i = 0; i < V; i++) {
                probs_bt[i] = expf(logits_bt[i] - maxval);
                sum += probs_bt[i];
            }
            
            for (int i = 0; i < V; i++) {
                probs_bt[i] /= sum;
            }
        }
    }
}

// ----------------------------------------------------------------------------
// 以下部分与原始代码相同，无需修改
// ----------------------------------------------------------------------------

void encoder_forward(float* out,
                   int* inp, float* wte, float* wpe,
                   int B, int T, int C) {
    for (int b = 0; b < B; b++) {
        for (int t = 0; t < T; t++) {
            float* out_bt = out + b * T * C + t * C;
            int ix = inp[b * T + t];
            float* wte_ix = wte + ix * C;
            float* wpe_t = wpe + t * C;
            for (int i = 0; i < C; i++) {
                out_bt[i] = wte_ix[i] + wpe_t[i];
            }
        }
    }
}

#define NUM_PARAMETER_TENSORS 16
typedef struct {
    float* wte; // (V, C)
    float* wpe; // (maxT, C)
    float* ln1w; // (L, C)
    float* ln1b; // (L, C)
    float* qkvw; // (L, 3*C, C)
    float* qkvb; // (L, 3*C)
    float* attprojw; // (L, C, C)
    float* attprojb; // (L, C)
    float* ln2w; // (L, C)
    float* ln2b; // (L, C)
    float* fcw; // (L, 4*C, C)
    float* fcb; // (L, 4*C)
    float* fcprojw; // (L, C, 4*C)
    float* fcprojb; // (L, C)
    float* lnfw; // (C)
    float* lnfb; // (C)
} ParameterTensors;

float* malloc_and_point_parameters(ParameterTensors* params, size_t* param_sizes) {
    size_t num_parameters = 0;
    for (size_t i = 0; i < NUM_PARAMETER_TENSORS; i++) {
        num_parameters += param_sizes[i];
    }
    float* params_memory = (float*)malloc(num_parameters * sizeof(float));
    float** ptrs[] = {
        &params->wte, &params->wpe, &params->ln1w, &params->ln1b, &params->qkvw, &params->qkvb,
        &params->attprojw, &params->attprojb, &params->ln2w, &params->ln2b, &params->fcw, &params->fcb,
        &params->fcprojw, &params->fcprojb, &params->lnfw, &params->lnfb
    };
    float* params_memory_iterator = params_memory;
    for (size_t i = 0; i < NUM_PARAMETER_TENSORS; i++) {
        *(ptrs[i]) = params_memory_iterator;
        params_memory_iterator += param_sizes[i];
    }
    return params_memory;
}

#define NUM_ACTIVATION_TENSORS 23
typedef struct {
    float* encoded; // (B, T, C)
    float* ln1; // (L, B, T, C)
    float* ln1_mean; // (L, B, T)
    float* ln1_rstd; // (L, B, T)
    float* qkv; // (L, B, T, 3*C)
    float* atty; // (L, B, T, C)
    float* preatt; // (L, B, NH, T, T)
    float* att; // (L, B, NH, T, T)
    float* attproj; // (L, B, T, C)
    float* residual2; // (L, B, T, C)
    float* ln2; // (L, B, T, C)
    float* ln2_mean; // (L, B, T)
    float* ln2_rstd; // (L, B, T)
    float* fch; // (L, B, T, 4*C)
    float* fch_gelu; // (L, B, T, 4*C)
    float* fcproj; // (L, B, T, C)
    float* residual3; // (L, B, T, C)
    float* lnf; // (B, T, C)
    float* lnf_mean; // (B, T)
    float* lnf_rstd; // (B, T)
    float* logits; // (B, T, V)
    float* probs; // (B, T, V)
    float* losses; // (B, T)
} ActivationTensors;

float* malloc_and_point_activations(ActivationTensors* acts, size_t* act_sizes) {
    size_t num_activations = 0;
    for (size_t i = 0; i < NUM_ACTIVATION_TENSORS; i++) {
        num_activations += act_sizes[i];
    }
    float* acts_memory = (float*)malloc(num_activations * sizeof(float));
    float** ptrs[] = {
        &acts->encoded, &acts->ln1, &acts->ln1_mean, &acts->ln1_rstd, &acts->qkv, &acts->atty,
        &acts->preatt, &acts->att, &acts->attproj, &acts->residual2, &acts->ln2, &acts->ln2_mean,
        &acts->ln2_rstd, &acts->fch, &acts->fch_gelu, &acts->fcproj, &acts->residual3, &acts->lnf,
        &acts->lnf_mean, &acts->lnf_rstd, &acts->logits, &acts->probs, &acts->losses
    };
    float* acts_memory_iterator = acts_memory;
    for (size_t i = 0; i < NUM_ACTIVATION_TENSORS; i++) {
        *(ptrs[i]) = acts_memory_iterator;
        acts_memory_iterator += act_sizes[i];
    }
    return acts_memory;
}

typedef struct {
    int max_seq_len; // max sequence length, e.g. 1024
    int vocab_size; // vocab size, e.g. 50257
    int num_layers; // number of layers, e.g. 12
    int num_heads; // number of heads in attention, e.g. 12
    int channels; // number of channels, e.g. 768
} GPT2Config;

typedef struct {
    GPT2Config config;
    ParameterTensors params;
    size_t param_sizes[NUM_PARAMETER_TENSORS];
    float* params_memory;
    int num_parameters;
    ParameterTensors grads;
    float* grads_memory;
    float* m_memory;
    float* v_memory;
    ActivationTensors acts;
    size_t act_sizes[NUM_ACTIVATION_TENSORS];
    float* acts_memory;
    int num_activations;
    ActivationTensors grads_acts;
    float* grads_acts_memory;
    int batch_size;
    int seq_len;
    int* inputs;
    int* targets;
    float mean_loss;
} GPT2;

void gpt2_build_from_checkpoint(GPT2 *model, char* checkpoint_path) {
    FILE *model_file = fopen(checkpoint_path, "rb");
    if (model_file == NULL) { printf("Error opening model file\n"); exit(1); }
    int model_header[256];
    fread(model_header, sizeof(int), 256, model_file);
    if (model_header[0] != 20240326) { printf("Bad magic model file"); exit(1); }
    if (model_header[1] != 1) { printf("Bad version in model file"); exit(1); }

    int maxT, V, L, NH, C;
    model->config.max_seq_len = maxT = model_header[2];
    model->config.vocab_size = V = model_header[3];
    model->config.num_layers = L = model_header[4];
    model->config.num_heads = NH = model_header[5];
    model->config.channels = C = model_header[6];

    model->param_sizes[0] = V * C;
    model->param_sizes[1] = maxT * C;
    model->param_sizes[2] = L * C;
    model->param_sizes[3] = L * C;
    model->param_sizes[4] = L * (3 * C) * C;
    model->param_sizes[5] = L * (3 * C);
    model->param_sizes[6] = L * C * C;
    model->param_sizes[7] = L * C;
    model->param_sizes[8] = L * C;
    model->param_sizes[9] = L * C;
    model->param_sizes[10] = L * (4 * C) * C;
    model->param_sizes[11] = L * (4 * C);
    model->param_sizes[12] = L * C * (4 * C);
    model->param_sizes[13] = L * C;
    model->param_sizes[14] = C;
    model->param_sizes[15] = C;

    size_t num_parameters = 0;
    for (size_t i = 0; i < NUM_PARAMETER_TENSORS; i++) {
        num_parameters += model->param_sizes[i];
    }
    model->num_parameters = num_parameters;

    model->params_memory = malloc_and_point_parameters(&model->params, model->param_sizes);
    fread(model->params_memory, sizeof(float), num_parameters, model_file);
    fclose(model_file);

    model->acts_memory = NULL;
    model->grads_memory = NULL;
    model->m_memory = NULL;
    model->v_memory = NULL;
    model->grads_acts_memory = NULL;
    model->inputs = NULL;
    model->targets = NULL;
    model->batch_size = 0;
    model->seq_len = 0;
    model->mean_loss = -1.0f;
}

void gpt2_forward(GPT2 *model, int* inputs, int B, int T) {
    int V = model->config.vocab_size;
    int L = model->config.num_layers;
    int NH = model->config.num_heads;
    int C = model->config.channels;

    model->batch_size = B;
    model->seq_len = T;
    
    model->act_sizes[0] = B * T * C;
    model->act_sizes[1] = L * B * T * C;
    model->act_sizes[2] = L * B * T;
    model->act_sizes[3] = L * B * T;
    model->act_sizes[4] = L * B * T * 3*C;
    model->act_sizes[5] = L * B * T * C;
    model->act_sizes[6] = L * B * NH * T * T;
    model->act_sizes[7] = L * B * NH * T * T;
    model->act_sizes[8] = L * B * T * C;
    model->act_sizes[9] = L * B * T * C;
    model->act_sizes[10] = L * B * T * C;
    model->act_sizes[11] = L * B * T;
    model->act_sizes[12] = L * B * T;
    model->act_sizes[13] = L * B * T * 4*C;
    model->act_sizes[14] = L * B * T * 4*C;
    model->act_sizes[15] = L * B * T * C;
    model->act_sizes[16] = L * B * T * C;
    model->act_sizes[17] = B * T * C;
    model->act_sizes[18] = B * T;
    model->act_sizes[19] = B * T;
    model->act_sizes[20] = B * T * V;
    model->act_sizes[21] = B * T * V;
    model->act_sizes[22] = B * T;
    
    size_t num_activations = 0;
    for (size_t i = 0; i < NUM_ACTIVATION_TENSORS; i++) {
        num_activations += model->act_sizes[i];
    }
    model->num_activations = num_activations;

    if (model->acts_memory) {
        free(model->acts_memory);
        model->acts_memory = NULL;
    }
    model->acts_memory = malloc_and_point_activations(&model->acts, model->act_sizes);

    if (model->inputs) {
        free(model->inputs);
    }
    model->inputs = (int*)malloc(B * T * sizeof(int));
    memcpy(model->inputs, inputs, B * T * sizeof(int));

    ParameterTensors params = model->params;
    ActivationTensors acts = model->acts;
    float* residual;
    
    encoder_forward(acts.encoded, inputs, params.wte, params.wpe, B, T, C);
    
    for (int l = 0; l < L; l++) {
        residual = l == 0 ? acts.encoded : acts.residual3 + (l-1) * B * T * C;

        float* l_ln1w = params.ln1w + l * C;
        float* l_ln1b = params.ln1b + l * C;
        float* l_qkvw = params.qkvw + l * 3*C * C;
        float* l_qkvb = params.qkvb + l * 3*C;
        float* l_attprojw = params.attprojw + l * C * C;
        float* l_attprojb = params.attprojb + l * C;
        float* l_ln2w = params.ln2w + l * C;
        float* l_ln2b = params.ln2b + l * C;
        float* l_fcw = params.fcw + l * 4*C * C;
        float* l_fcb = params.fcb + l * 4*C;
        float* l_fcprojw = params.fcprojw + l * C * 4*C;
        float* l_fcprojb = params.fcprojb + l * C;

        float* l_ln1 = acts.ln1 + l * B * T * C;
        float* l_ln1_mean = acts.ln1_mean + l * B * T;
        float* l_ln1_rstd = acts.ln1_rstd + l * B * T;
        float* l_qkv = acts.qkv + l * B * T * 3*C;
        float* l_atty = acts.atty + l * B * T * C;
        float* l_preatt = acts.preatt + l * B * NH * T * T;
        float* l_att = acts.att + l * B * NH * T * T;
        float* l_attproj = acts.attproj + l * B * T * C;
        float* l_residual2 = acts.residual2 + l * B * T * C;
        float* l_ln2 = acts.ln2 + l * B * T * C;
        float* l_ln2_mean = acts.ln2_mean + l * B * T;
        float* l_ln2_rstd = acts.ln2_rstd + l * B * T;
        float* l_fch = acts.fch + l * B * T * 4*C;
        float* l_fch_gelu = acts.fch_gelu + l * B * T * 4*C;
        float* l_fcproj = acts.fcproj + l * B * T * C;
        float* l_residual3 = acts.residual3 + l * B * T * C;

        layernorm_forward(l_ln1, l_ln1_mean, l_ln1_rstd, residual, l_ln1w, l_ln1b, B, T, C);
        matmul_forward(l_qkv, l_ln1, l_qkvw, l_qkvb, B, T, C, 3*C);
        attention_forward(l_atty, l_preatt, l_att, l_qkv, B, T, C, NH);
        matmul_forward(l_attproj, l_atty, l_attprojw, l_attprojb, B, T, C, C);
        residual_forward(l_residual2, residual, l_attproj, B*T*C);
        layernorm_forward(l_ln2, l_ln2_mean, l_ln2_rstd, l_residual2, l_ln2w, l_ln2b, B, T, C);
        matmul_forward(l_fch, l_ln2, l_fcw, l_fcb, B, T, C, 4*C);
        gelu_forward(l_fch_gelu, l_fch, B*T*4*C);
        matmul_forward(l_fcproj, l_fch_gelu, l_fcprojw, l_fcprojb, B, T, 
