/* SPDX-License-Identifier: GPL-3.0-or-later */
extern "C" {
#include <libavutil/tx.h>
}
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

static void check(bool ok) {
    if (!ok)
        std::abort();
}
template <typename T> static void impulse(AVTXType type, int length) {
    AVTXContext *context = nullptr;
    av_tx_fn transform = nullptr;
    check(av_tx_init(&context, &transform, type, 0, length, nullptr, AV_TX_UNALIGNED) == 0);
    std::vector<T> input(length * 2), output(length * 2);
    input[0] = 1;
    transform(context, output.data(), input.data(), sizeof(T) * 2);
    for (int i = 0; i < length; ++i) {
        check(std::abs(output[i * 2] - 1) < 0.00001);
        check(std::abs(output[i * 2 + 1]) < 0.00001);
    }
    av_tx_uninit(&context);
}
int main() {
    for (int length : {8, 128, 960, 1024, 2048, 32768}) {
        impulse<float>(AV_TX_FLOAT_FFT, length);
        impulse<double>(AV_TX_DOUBLE_FFT, length);
    }
    for (int type = 0; type < AV_TX_NB; ++type) {
        for (int length : {-1, 0, 32769, 65536, 2097152}) {
            AVTXContext *context = nullptr;
            av_tx_fn transform = nullptr;
            check(av_tx_init(&context, &transform, static_cast<AVTXType>(type), 0, length, nullptr,
                             AV_TX_UNALIGNED) < 0);
            check(context == nullptr && transform == nullptr);
        }
    }
    puts("Transform budget: supported FFT output and oversized transform rejection passed");
}
