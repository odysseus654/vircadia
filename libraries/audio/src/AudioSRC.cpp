//
//  AudioSRC.cpp
//  libraries/audio/src
//
//  Created by Ken Cooke on 9/18/15.
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include <assert.h>
#include <string.h>
#include <algorithm>

#include "AudioSRC.h"

//
// prototype lowpass filter
//
static const int PROTOTYPE_TAPS = 96;       // filter taps per phase
static const int PROTOTYPE_PHASES = 32;     // oversampling factor
static const int PROTOTYPE_COEFS = PROTOTYPE_TAPS * PROTOTYPE_PHASES;
extern const float prototypeFilter[PROTOTYPE_COEFS];

//
// polyphase filter
//
static const int SRC_PHASEBITS = 8;
static const int SRC_PHASES = (1 << SRC_PHASEBITS);
static const int SRC_FRACBITS = 32 - SRC_PHASEBITS;
static const uint32_t SRC_FRACMASK = (1 << SRC_FRACBITS) - 1;

static const float QFRAC_TO_FLOAT = 1.0f / (1 << SRC_FRACBITS);
static const float Q32_TO_FLOAT = 1.0f / (1ULL << 32);

// blocking size in frames, chosen so block processing fits in L1 cache
static const int SRC_BLOCK = 1024;

//#define SRC_DITHER
#define RAND16(r) (((r) = (r) * 69069u + 1u) >> 16)

// these are performance sensitive
#define lo32(a)   (((uint32_t* )&(a))[0])
#define hi32(a)   (((int32_t* )&(a))[1])

//#define lo32(a)   ((uint32_t)(a))
//#define hi32(a)   ((int32_t)((a) >> 32))

//static inline uint32_t lo32(int64_t a) {
//    union { 
//        int64_t val; 
//        struct { uint32_t lo; int32_t hi; } reg; 
//    } b = { a };
//    return b.reg.lo;
//}
//static inline int32_t hi32(int64_t a) {
//    union { 
//        int64_t val; 
//        struct { uint32_t lo; int32_t hi; } reg; 
//    } b = { a };
//    return b.reg.hi;
//}

//
// Portable aligned malloc/free
//
static void* aligned_malloc(size_t size, size_t alignment) {
    if ((alignment & (alignment-1)) == 0) {
        void* p = malloc(size + sizeof(void*) + (alignment-1));
        if (p) {
            void* ptr = (void*)(((size_t)p + sizeof(void*) + (alignment-1)) & ~(alignment-1));
            ((void**)ptr)[-1] = p;
            return ptr;
        }
    }
    return nullptr;
}

static void aligned_free(void* ptr) {
    if (ptr) {
        void* p = ((void**)ptr)[-1];
        free(p);
    }
}

// find the greatest common divisor
static int gcd(int a, int b)
{
    while (a != b) {
        if (a > b) {
            a -= b;
        } else {
            b -= a;
        }
    }
    return a;
}

//
// 3rd-order Lagrange interpolation
//
// Lagrange interpolation is maximally flat near dc and well suited
// for further upsampling our heavily-oversampled prototype filter.
//
static void cubicInterpolation(const float* input, float* output, int inputSize, int outputSize, float gain) {

    int64_t step = ((int64_t)inputSize << 32) / outputSize;     // Q32
    int64_t offset = (outputSize < inputSize) ? (step/2) : 0;   // offset to improve small integer ratios

    // Lagrange interpolation using Farrow structure
    for (int j = 0; j < outputSize; j++) {

        int32_t i = hi32(offset);
        uint32_t f = lo32(offset);

        // values outside the window are zero
        float x0 = (i - 1 < 0) ? 0.0f : input[i - 1];
        float x1 = (i - 0 < 0) ? 0.0f : input[i - 0];
        float x2 = (i + 1 < inputSize) ? input[i + 1] : 0.0f;
        float x3 = (i + 2 < inputSize) ? input[i + 2] : 0.0f;

        // compute the polynomial coefficients
        float c0 = (1/6.0f) * (x3 - x0) + (1/2.0f) * (x1 - x2);
        float c1 = (1/2.0f) * (x0 + x2) - x1;
        float c2 = x2 - (1/3.0f) * x0 - (1/2.0f) * x1 - (1/6.0f) * x3;
        float c3 = x1;

        // compute the polynomial
        float frac = f * Q32_TO_FLOAT;
        output[j] = (((c0 * frac + c1) * frac + c2) * frac + c3) * gain;

        offset += step;
    }
}

int AudioSRC::createRationalFilter(int upFactor, int downFactor, float gain) {
    int numTaps = PROTOTYPE_TAPS;
    int numPhases = upFactor;
    int numCoefs = numTaps * numPhases;
    int oldCoefs = numCoefs;

    //
    // When downsampling, we can lower the filter cutoff by downFactor/upFactor using the
    // time-scaling property of the Fourier transform.  The gain is adjusted accordingly.
    //
    if (downFactor > upFactor) {
        numCoefs = ((int64_t)oldCoefs * downFactor) / upFactor;
        numTaps = (numCoefs + upFactor - 1) / upFactor;
        gain *= (float)oldCoefs / numCoefs;
    }

    // interpolate the coefficients of the prototype filter
    float* tempFilter = new float[numTaps * numPhases];
    memset(tempFilter, 0, numTaps * numPhases * sizeof(float));

    cubicInterpolation(prototypeFilter, tempFilter, PROTOTYPE_COEFS, numCoefs, gain);

    // create the polyphase filter
    _polyphaseFilter = (float*)aligned_malloc(numTaps * numPhases * sizeof(float), 32);

    // rearrange into polyphase form, ordered by use
    for (int i = 0; i < numPhases; i++) {
        int phase = (i * downFactor) % upFactor;
        for (int j = 0; j < numTaps; j++) {

            // the filter taps are reversed, so convolution is implemented as dot-product
            float f = tempFilter[(numTaps - j - 1) * numPhases + phase];
            _polyphaseFilter[numTaps * i + j] = f;
        }
    }

    delete[] tempFilter;

    // precompute the input steps
    _stepTable = new int[numPhases];

    for (int i = 0; i < numPhases; i++) {
        _stepTable[i] = (((int64_t)(i+1) * downFactor) / upFactor) - (((int64_t)(i+0) * downFactor) / upFactor);
    }

    return numTaps;
}

int AudioSRC::createIrrationalFilter(int upFactor, int downFactor, float gain) {
    int numTaps = PROTOTYPE_TAPS;
    int numPhases = upFactor;
    int numCoefs = numTaps * numPhases;
    int oldCoefs = numCoefs;

    //
    // When downsampling, we can lower the filter cutoff by downFactor/upFactor using the
    // time-scaling property of the Fourier transform.  The gain is adjusted accordingly.
    //
    if (downFactor > upFactor) {
        numCoefs = ((int64_t)oldCoefs * downFactor) / upFactor;
        numTaps = (numCoefs + upFactor - 1) / upFactor;
        gain *= (float)oldCoefs / numCoefs;
    }

    // interpolate the coefficients of the prototype filter
    float* tempFilter = new float[numTaps * numPhases];
    memset(tempFilter, 0, numTaps * numPhases * sizeof(float));

    cubicInterpolation(prototypeFilter, tempFilter, PROTOTYPE_COEFS, numCoefs, gain);

    // create the polyphase filter, with extra phase at the end to simplify coef interpolation
    _polyphaseFilter = (float*)aligned_malloc(numTaps * (numPhases + 1) * sizeof(float), 32);

    // rearrange into polyphase form, ordered by fractional delay
    for (int phase = 0; phase < numPhases; phase++) {
        for (int j = 0; j < numTaps; j++) {

            // the filter taps are reversed, so convolution is implemented as dot-product
            float f = tempFilter[(numTaps - j - 1) * numPhases + phase];
            _polyphaseFilter[numTaps * phase + j] = f;
        }
    }

    delete[] tempFilter;

    // by construction, the last tap of the first phase must be zero
    assert(_polyphaseFilter[numTaps - 1] == 0.0f);

    // so the extra phase is just the first, shifted by one
    _polyphaseFilter[numTaps * numPhases + 0] = 0.0f;
    for (int j = 1; j < numTaps; j++) {
        _polyphaseFilter[numTaps * numPhases + j] = _polyphaseFilter[j-1];
    }

    return numTaps;
}

int AudioSRC::multirateFilter1(const float* input0, float* output0, int inputFrames) {
    int outputFrames = 0;

    if (_step == 0) {   // rational

        int32_t i = hi32(_offset);

        while (i < inputFrames) {

            const float* c0 = &_polyphaseFilter[_numTaps * _phase];

            float acc0 = 0.0f;

            for (int j = 0; j < _numTaps; j++) {

                float coef = c0[j];

                acc0 += input0[i + j] * c0[j];
            }

            output0[outputFrames] = acc0;
            outputFrames += 1;

            i += _stepTable[_phase];
            if (++_phase == _upFactor) {
                _phase = 0;
            }
        }
        _offset = (int64_t)(i - inputFrames) << 32;

    } else {    // irrational

        while (hi32(_offset) < inputFrames) {

            int32_t i = hi32(_offset);
            uint32_t f = lo32(_offset);

            uint32_t phase = f >> SRC_FRACBITS;
            float frac = (f & SRC_FRACMASK) * QFRAC_TO_FLOAT;

            const float* c0 = &_polyphaseFilter[_numTaps * (phase + 0)];
            const float* c1 = &_polyphaseFilter[_numTaps * (phase + 1)];

            float acc0 = 0.0f;

            for (int j = 0; j < _numTaps; j++) {

                float coef = c0[j] + frac * (c1[j] - c0[j]);

                acc0 += input0[i + j] * coef;
            }

            output0[outputFrames] = acc0;
            outputFrames += 1;

            _offset += _step;
        }
        _offset -= (int64_t)inputFrames << 32;
    }

    return outputFrames;
}

int AudioSRC::multirateFilter2(const float* input0, const float* input1, float* output0, float* output1, int inputFrames) {
    int outputFrames = 0;

    if (_step == 0) {   // rational

        int32_t i = hi32(_offset);

        while (i < inputFrames) {

            const float* c0 = &_polyphaseFilter[_numTaps * _phase];

            float acc0 = 0.0f;
            float acc1 = 0.0f;

            for (int j = 0; j < _numTaps; j++) {

                float coef = c0[j];

                acc0 += input0[i + j] * c0[j];
                acc1 += input1[i + j] * c0[j];
            }

            output0[outputFrames] = acc0;
            output1[outputFrames] = acc1;
            outputFrames += 1;

            i += _stepTable[_phase];
            if (++_phase == _upFactor) {
                _phase = 0;
            }
        }
        _offset = (int64_t)(i - inputFrames) << 32;

    } else {    // irrational

        while (hi32(_offset) < inputFrames) {

            int32_t i = hi32(_offset);
            uint32_t f = lo32(_offset);

            uint32_t phase = f >> SRC_FRACBITS;
            float frac = (f & SRC_FRACMASK) * QFRAC_TO_FLOAT;

            const float* c0 = &_polyphaseFilter[_numTaps * (phase + 0)];
            const float* c1 = &_polyphaseFilter[_numTaps * (phase + 1)];

            float acc0 = 0.0f;
            float acc1 = 0.0f;

            for (int j = 0; j < _numTaps; j++) {

                float coef = c0[j] + frac * (c1[j] - c0[j]);

                acc0 += input0[i + j] * coef;
                acc1 += input1[i + j] * coef;
            }

            output0[outputFrames] = acc0;
            output1[outputFrames] = acc1;
            outputFrames += 1;

            _offset += _step;
        }
        _offset -= (int64_t)inputFrames << 32;
    }

    return outputFrames;
}

// convert int16_t to float
// deinterleave stereo samples
void AudioSRC::convertInputFromInt16(const int16_t* input, float** outputs, int numFrames) {
    for (int i = 0; i < numFrames; i++) {
        for (int j = 0; j < _numChannels; j++) {

            float f = (float)input[_numChannels*i + j];
            outputs[j][i] = f * (1.0f/32768.0f);
        }
    }
}

// convert float to int16_t
// interleave stereo samples
void AudioSRC::convertOutputToInt16(float** inputs, int16_t* output, int numFrames) {
    for (int i = 0; i < numFrames; i++) {
        for (int j = 0; j < _numChannels; j++) {

            float f = inputs[j][i] * 32768.0f;

#ifdef SRC_DITHER
            // TPDF dither in [-1.0f, 1.0f]
            static uint32_t rz = 1;
            int r0 = RAND16(rz);
            int r1 = RAND16(rz);
            f += (r0 - r1) * (1.0f/65536.0f);

            // round
            f += (f < 0.0f ? -0.5f : +0.5f);
#endif
            // saturate
            f = std::min(f, 32767.0f);
            f = std::max(f, -32768.0f);

            output[_numChannels * i + j] = (int16_t)f;
        }
    }
}

int AudioSRC::processFloat(float** inputs, float** outputs, int inputFrames) {
    int outputFrames = 0;

    int nh = std::min(_numHistory, inputFrames);    // number of frames from history buffer
    int ni = inputFrames - nh;                      // number of frames from remaining input

    if (_numChannels == 1) {

        // refill history buffers
        memcpy(_history[0] + _numHistory, _inputs[0], nh * sizeof(float));

        // process history buffer
        outputFrames += multirateFilter1(_history[0], _outputs[0], nh);

        // process remaining input
        if (ni) {
            outputFrames += multirateFilter1(_inputs[0], _outputs[0] + outputFrames, ni);
        }

        // shift history buffers
        if (ni) {
            memcpy(_history[0], _inputs[0] + ni, _numHistory * sizeof(float));
        } else {
            memmove(_history[0], _history[0] + nh, _numHistory * sizeof(float));
        }

    } else if (_numChannels == 2) {

        // refill history buffers
        memcpy(_history[0] + _numHistory, _inputs[0], nh * sizeof(float));
        memcpy(_history[1] + _numHistory, _inputs[1], nh * sizeof(float));

        // process history buffer
        outputFrames += multirateFilter2(_history[0], _history[1], _outputs[0], _outputs[1], nh);

        // process remaining input
        if (ni) {
            outputFrames += multirateFilter2(_inputs[0], _inputs[1], _outputs[0] + outputFrames, _outputs[1] + outputFrames, ni);
        }

        // shift history buffers
        if (ni) {
            memcpy(_history[0], inputs[0] + ni, _numHistory * sizeof(float));
            memcpy(_history[1], inputs[1] + ni, _numHistory * sizeof(float));
        } else {
            memmove(_history[0], _history[0] + nh, _numHistory * sizeof(float));
            memmove(_history[1], _history[1] + nh, _numHistory * sizeof(float));
        }
    }

    return outputFrames;
}

AudioSRC::AudioSRC(int inputSampleRate, int outputSampleRate, int numChannels) {
    assert(inputSampleRate > 0);
    assert(outputSampleRate > 0);
    assert(numChannels > 0);
    assert(numChannels <= MAX_CHANNELS);

    _inputSampleRate = inputSampleRate;
    _outputSampleRate = outputSampleRate;
    _numChannels = numChannels;

    // reduce to the smallest rational fraction
    int divisor = gcd(inputSampleRate, outputSampleRate);
    _upFactor = outputSampleRate / divisor;
    _downFactor = inputSampleRate / divisor;
    _step = 0;  // rational mode

    // if the number of phases is too large, use irrational mode
    if (_upFactor > 640) {
        _upFactor = SRC_PHASES;
        _downFactor = ((int64_t)SRC_PHASES * _inputSampleRate) / _outputSampleRate;
        _step = ((int64_t)_inputSampleRate << 32) / _outputSampleRate;
    }

    _polyphaseFilter = nullptr;
    _stepTable = nullptr;

    // create the polyphase filter
    if (_step == 0) {
        _numTaps = createRationalFilter(_upFactor, _downFactor, 1.0f);
    } else {
        _numTaps = createIrrationalFilter(_upFactor, _downFactor, 1.0f);
    }

    //printf("up=%d down=%.3f taps=%d\n", _upFactor, _downFactor + (lo32(_step)<<SRC_PHASEBITS) * Q32_TO_FLOAT, _numTaps);

    // filter history buffers
    _numHistory = _numTaps - 1;
    _history[0] = new float[2 * _numHistory];
    _history[1] = new float[2 * _numHistory];

    // format conversion buffers
    _inputs[0] = new float[SRC_BLOCK];
    _inputs[1] = new float[SRC_BLOCK];
    _outputs[0] = new float[SRC_BLOCK];
    _outputs[1] = new float[SRC_BLOCK];

    // input blocking size, such that input and output are both guaranteed not to exceed SRC_BLOCK frames
    _inputBlock = std::min(SRC_BLOCK, getMaxInput(SRC_BLOCK));

    // reset the state
    _offset = 0;
    _phase = 0;
    memset(_history[0], 0, 2 * _numHistory * sizeof(float));
    memset(_history[1], 0, 2 * _numHistory * sizeof(float));
}

AudioSRC::~AudioSRC() {
    aligned_free(_polyphaseFilter);
    delete[] _stepTable;

    delete[] _history[0];
    delete[] _history[1];

    delete[] _inputs[0];
    delete[] _inputs[1];
    delete[] _outputs[0];
    delete[] _outputs[1];
}

//
// This version handles input/output as interleaved int16_t
//
int AudioSRC::render(const int16_t* input, int16_t* output, int inputFrames) {
    int outputFrames = 0;

    while (inputFrames) {

        int ni = std::min(inputFrames, _inputBlock);

        convertInputFromInt16(input, _inputs, ni);

        int no = processFloat(_inputs, _outputs, ni);
        assert(no <= SRC_BLOCK);

        convertOutputToInt16(_outputs, output, no);

        input += _numChannels * ni;
        output += _numChannels * no;
        inputFrames -= ni;
        outputFrames += no;
    }

    return outputFrames;
}

// the min output frames that will be produced by inputFrames
int AudioSRC::getMinOutput(int inputFrames) {
    if (_step == 0) {
        return (int)(((int64_t)inputFrames * _upFactor) / _downFactor);
    } else {
        return (int)(((int64_t)inputFrames << 32) / _step);
    }
}

// the max output frames that will be produced by inputFrames
int AudioSRC::getMaxOutput(int inputFrames) {
    if (_step == 0) {
        return (int)(((int64_t)inputFrames * _upFactor + _downFactor-1) / _downFactor);
    } else {
        return (int)((((int64_t)inputFrames << 32) + _step-1) / _step);
    }
}

// the min input frames that will produce at least outputFrames
int AudioSRC::getMinInput(int outputFrames) {
    if (_step == 0) {
        return (int)(((int64_t)outputFrames * _downFactor + _upFactor-1) / _upFactor);
    } else {
        return (int)(((int64_t)outputFrames * _step + 0xffffffffu) >> 32);
    }
}

// the max input frames that will produce at most outputFrames
int AudioSRC::getMaxInput(int outputFrames) {
    if (_step == 0) {
        return (int)(((int64_t)outputFrames * _downFactor) / _upFactor);
    } else {
        return (int)(((int64_t)outputFrames * _step) >> 32);
    }
}

//
// prototype lowpass filter
//
// Minimum-phase equiripple FIR
// taps = 96, oversampling = 32
//
// passband = 0.918
// stopband = 1.010
// passband ripple = +-0.01dB
// stopband attn = -125dB (-70dB at 1.000)
//
const float prototypeFilter[PROTOTYPE_COEFS] = {
     0.00000000e+00f,  1.55021703e-05f,  1.46054865e-05f,  2.07057160e-05f,  2.91335519e-05f,  4.00091078e-05f, 
     5.33544450e-05f,  7.03618468e-05f,  9.10821639e-05f,  1.16484613e-04f,  1.47165999e-04f,  1.84168304e-04f, 
     2.28429617e-04f,  2.80913884e-04f,  3.42940399e-04f,  4.15773039e-04f,  5.01023255e-04f,  6.00234953e-04f, 
     7.15133271e-04f,  8.47838855e-04f,  1.00032516e-03f,  1.17508881e-03f,  1.37452550e-03f,  1.60147614e-03f, 
     1.85886458e-03f,  2.14985024e-03f,  2.47783071e-03f,  2.84666764e-03f,  3.26016878e-03f,  3.72252797e-03f, 
     4.23825900e-03f,  4.81207874e-03f,  5.44904143e-03f,  6.15447208e-03f,  6.93399929e-03f,  7.79337059e-03f, 
     8.73903392e-03f,  9.77729117e-03f,  1.09149561e-02f,  1.21591316e-02f,  1.35171164e-02f,  1.49965439e-02f, 
     1.66053136e-02f,  1.83515384e-02f,  2.02435362e-02f,  2.22899141e-02f,  2.44995340e-02f,  2.68813362e-02f, 
     2.94443254e-02f,  3.21979928e-02f,  3.51514690e-02f,  3.83143719e-02f,  4.16960560e-02f,  4.53060504e-02f, 
     4.91538115e-02f,  5.32486197e-02f,  5.75998650e-02f,  6.22164253e-02f,  6.71072811e-02f,  7.22809789e-02f, 
     7.77457552e-02f,  8.35095233e-02f,  8.95796944e-02f,  9.59631768e-02f,  1.02666457e-01f,  1.09695215e-01f, 
     1.17054591e-01f,  1.24748885e-01f,  1.32781656e-01f,  1.41155521e-01f,  1.49872243e-01f,  1.58932534e-01f, 
     1.68335961e-01f,  1.78081143e-01f,  1.88165339e-01f,  1.98584621e-01f,  2.09333789e-01f,  2.20406193e-01f, 
     2.31793899e-01f,  2.43487398e-01f,  2.55475740e-01f,  2.67746404e-01f,  2.80285305e-01f,  2.93076743e-01f, 
     3.06103423e-01f,  3.19346351e-01f,  3.32784916e-01f,  3.46396772e-01f,  3.60158039e-01f,  3.74043042e-01f, 
     3.88024564e-01f,  4.02073759e-01f,  4.16160177e-01f,  4.30251886e-01f,  4.44315429e-01f,  4.58315954e-01f, 
     4.72217175e-01f,  4.85981675e-01f,  4.99570709e-01f,  5.12944586e-01f,  5.26062401e-01f,  5.38882630e-01f, 
     5.51362766e-01f,  5.63459860e-01f,  5.75130384e-01f,  5.86330458e-01f,  5.97016050e-01f,  6.07143161e-01f, 
     6.16667840e-01f,  6.25546499e-01f,  6.33735979e-01f,  6.41193959e-01f,  6.47878856e-01f,  6.53750084e-01f, 
     6.58768549e-01f,  6.62896349e-01f,  6.66097381e-01f,  6.68337353e-01f,  6.69583869e-01f,  6.69807061e-01f, 
     6.68979117e-01f,  6.67075139e-01f,  6.64072812e-01f,  6.59952827e-01f,  6.54699116e-01f,  6.48298688e-01f, 
     6.40742160e-01f,  6.32023668e-01f,  6.22141039e-01f,  6.11095903e-01f,  5.98893921e-01f,  5.85544600e-01f, 
     5.71061707e-01f,  5.55463040e-01f,  5.38770639e-01f,  5.21010762e-01f,  5.02213839e-01f,  4.82414572e-01f, 
     4.61651859e-01f,  4.39968628e-01f,  4.17412000e-01f,  3.94032951e-01f,  3.69886464e-01f,  3.45031084e-01f, 
     3.19529091e-01f,  2.93446187e-01f,  2.66851164e-01f,  2.39815999e-01f,  2.12415399e-01f,  1.84726660e-01f, 
     1.56829293e-01f,  1.28804933e-01f,  1.00736965e-01f,  7.27100355e-02f,  4.48100810e-02f,  1.71237415e-02f, 
    -1.02620228e-02f, -3.72599591e-02f, -6.37832871e-02f, -8.97457733e-02f, -1.15062201e-01f, -1.39648782e-01f, 
    -1.63423488e-01f, -1.86306368e-01f, -2.08220103e-01f, -2.29090072e-01f, -2.48845046e-01f, -2.67417270e-01f, 
    -2.84742946e-01f, -3.00762597e-01f, -3.15421127e-01f, -3.28668542e-01f, -3.40459849e-01f, -3.50755400e-01f, 
    -3.59521402e-01f, -3.66729768e-01f, -3.72358475e-01f, -3.76391839e-01f, -3.78820421e-01f, -3.79641287e-01f, 
    -3.78858203e-01f, -3.76481336e-01f, -3.72527677e-01f, -3.67020780e-01f, -3.59990760e-01f, -3.51474372e-01f, 
    -3.41514630e-01f, -3.30160971e-01f, -3.17468898e-01f, -3.03499788e-01f, -2.88320749e-01f, -2.72004315e-01f, 
    -2.54628056e-01f, -2.36274454e-01f, -2.17030464e-01f, -1.96986952e-01f, -1.76238733e-01f, -1.54883647e-01f, 
    -1.33022496e-01f, -1.10758449e-01f, -8.81964466e-02f, -6.54430504e-02f, -4.26055475e-02f, -1.97916415e-02f, 
     2.89108184e-03f,  2.53355868e-02f,  4.74362201e-02f,  6.90887518e-02f,  9.01914308e-02f,  1.10644978e-01f, 
     1.30353494e-01f,  1.49224772e-01f,  1.67170735e-01f,  1.84107975e-01f,  1.99958067e-01f,  2.14648181e-01f, 
     2.28111323e-01f,  2.40286622e-01f,  2.51119890e-01f,  2.60563701e-01f,  2.68577740e-01f,  2.75129027e-01f, 
     2.80192144e-01f,  2.83749177e-01f,  2.85790223e-01f,  2.86312986e-01f,  2.85323221e-01f,  2.82834421e-01f, 
     2.78867915e-01f,  2.73452721e-01f,  2.66625431e-01f,  2.58429983e-01f,  2.48917457e-01f,  2.38145826e-01f, 
     2.26179680e-01f,  2.13089734e-01f,  1.98952740e-01f,  1.83850758e-01f,  1.67870897e-01f,  1.51104879e-01f, 
     1.33648388e-01f,  1.15600665e-01f,  9.70639763e-02f,  7.81429119e-02f,  5.89439889e-02f,  3.95749746e-02f, 
     2.01442353e-02f,  7.60241152e-04f, -1.84690990e-02f, -3.74370397e-02f, -5.60385970e-02f, -7.41711039e-02f, 
    -9.17348686e-02f, -1.08633632e-01f, -1.24775254e-01f, -1.40071993e-01f, -1.54441372e-01f, -1.67806284e-01f, 
    -1.80095654e-01f, -1.91244732e-01f, -2.01195605e-01f, -2.09897310e-01f, -2.17306320e-01f, -2.23386736e-01f, 
    -2.28110407e-01f, -2.31457193e-01f, -2.33415044e-01f, -2.33980051e-01f, -2.33156463e-01f, -2.30956673e-01f, 
    -2.27401097e-01f, -2.22518148e-01f, -2.16343899e-01f, -2.08921985e-01f, -2.00303365e-01f, -1.90545790e-01f, 
    -1.79713804e-01f, -1.67877977e-01f, -1.55114789e-01f, -1.41505907e-01f, -1.27137921e-01f, -1.12101628e-01f, 
    -9.64915640e-02f, -8.04054232e-02f, -6.39434707e-02f, -4.72078814e-02f, -3.03021635e-02f, -1.33305082e-02f, 
     3.60284977e-03f,  2.03942507e-02f,  3.69413014e-02f,  5.31433810e-02f,  6.89024656e-02f,  8.41234679e-02f, 
     9.87150268e-02f,  1.12589969e-01f,  1.25665865e-01f,  1.37865538e-01f,  1.49117506e-01f,  1.59356490e-01f, 
     1.68523664e-01f,  1.76567229e-01f,  1.83442499e-01f,  1.89112308e-01f,  1.93547212e-01f,  1.96725586e-01f, 
     1.98633878e-01f,  1.99266486e-01f,  1.98625999e-01f,  1.96723008e-01f,  1.93576075e-01f,  1.89211557e-01f, 
     1.83663562e-01f,  1.76973516e-01f,  1.69190033e-01f,  1.60368490e-01f,  1.50570805e-01f,  1.39864815e-01f, 
     1.28324021e-01f,  1.16026978e-01f,  1.03056879e-01f,  8.95008829e-02f,  7.54496798e-02f,  6.09968238e-02f, 
     4.62380664e-02f,  3.12708901e-02f,  1.61936956e-02f,  1.10531988e-03f, -1.38957653e-02f, -2.87119784e-02f, 
    -4.32472742e-02f, -5.74078385e-02f, -7.11026311e-02f, -8.42439713e-02f, -9.67481917e-02f, -1.08536049e-01f, 
    -1.19533350e-01f, -1.29671345e-01f, -1.38887238e-01f, -1.47124498e-01f, -1.54333373e-01f, -1.60470968e-01f, 
    -1.65501755e-01f, -1.69397631e-01f, -1.72138140e-01f, -1.73710602e-01f, -1.74110159e-01f, -1.73339798e-01f, 
    -1.71410274e-01f, -1.68340111e-01f, -1.64155335e-01f, -1.58889414e-01f, -1.52582850e-01f, -1.45283122e-01f, 
    -1.37044042e-01f, -1.27925722e-01f, -1.17993860e-01f, -1.07319421e-01f, -9.59781808e-02f, -8.40500777e-02f, 
    -7.16188049e-02f, -5.87710561e-02f, -4.55961475e-02f, -3.21851919e-02f, -1.86306406e-02f, -5.02554942e-03f, 
     8.53698384e-03f,  2.19645467e-02f,  3.51659468e-02f,  4.80518693e-02f,  6.05355056e-02f,  7.25330700e-02f, 
     8.39645094e-02f,  9.47537898e-02f,  1.04829753e-01f,  1.14126254e-01f,  1.22582788e-01f,  1.30144907e-01f, 
     1.36764459e-01f,  1.42400029e-01f,  1.47017076e-01f,  1.50588312e-01f,  1.53093700e-01f,  1.54520736e-01f, 
     1.54864367e-01f,  1.54127119e-01f,  1.52318991e-01f,  1.49457408e-01f,  1.45567062e-01f,  1.40679709e-01f, 
     1.34833933e-01f,  1.28074855e-01f,  1.20453893e-01f,  1.12028129e-01f,  1.02860307e-01f,  9.30178765e-02f, 
     8.25730032e-02f,  7.16016450e-02f,  6.01833134e-02f,  4.84002546e-02f,  3.63370724e-02f,  2.40800037e-02f, 
     1.17163168e-02f, -6.66217400e-04f, -1.29801121e-02f, -2.51385315e-02f, -3.70562030e-02f, -4.86497748e-02f, 
    -5.98384928e-02f, -7.05447859e-02f, -8.06947592e-02f, -9.02187441e-02f, -9.90517313e-02f, -1.07133911e-01f, 
    -1.14410951e-01f, -1.20834483e-01f, -1.26362422e-01f, -1.30959116e-01f, -1.34595787e-01f, -1.37250547e-01f, 
    -1.38908600e-01f, -1.39562374e-01f, -1.39211442e-01f, -1.37862602e-01f, -1.35529795e-01f, -1.32233909e-01f, 
    -1.28002721e-01f, -1.22870611e-01f, -1.16878278e-01f, -1.10072477e-01f, -1.02505698e-01f, -9.42356124e-02f, 
    -8.53248753e-02f, -7.58404912e-02f, -6.58532924e-02f, -5.54376360e-02f, -4.46705953e-02f, -3.36315414e-02f, 
    -2.24015972e-02f, -1.10628991e-02f,  3.01894735e-04f,  1.16101918e-02f,  2.27801642e-02f,  3.37311642e-02f, 
     4.43845430e-02f,  5.46640016e-02f,  6.44962637e-02f,  7.38115400e-02f,  8.25440784e-02f,  9.06325572e-02f, 
     9.80206066e-02f,  1.04657146e-01f,  1.10496723e-01f,  1.15499920e-01f,  1.19633523e-01f,  1.22870824e-01f, 
     1.25191729e-01f,  1.26582959e-01f,  1.27038061e-01f,  1.26557494e-01f,  1.25148528e-01f,  1.22825305e-01f, 
     1.19608512e-01f,  1.15525479e-01f,  1.10609643e-01f,  1.04900592e-01f,  9.84435537e-02f,  9.12890948e-02f, 
     8.34927732e-02f,  7.51146973e-02f,  6.62190194e-02f,  5.68735547e-02f,  4.71491262e-02f,  3.71191855e-02f, 
     2.68591932e-02f,  1.64459573e-02f,  5.95731808e-03f, -4.52874940e-03f, -1.49344723e-02f, -2.51829130e-02f, 
    -3.51986373e-02f, -4.49081427e-02f, -5.42404654e-02f, -6.31276969e-02f, -7.15054163e-02f, -7.93132713e-02f, 
    -8.64953327e-02f, -9.30005042e-02f, -9.87829011e-02f, -1.03802223e-01f, -1.08023943e-01f, -1.11419636e-01f, 
    -1.13967111e-01f, -1.15650603e-01f, -1.16460855e-01f, -1.16395152e-01f, -1.15457368e-01f, -1.13657871e-01f, 
    -1.11013433e-01f, -1.07547117e-01f, -1.03288073e-01f, -9.82712708e-02f, -9.25372646e-02f, -8.61318657e-02f, 
    -7.91057486e-02f, -7.15141053e-02f, -6.34161588e-02f, -5.48747791e-02f, -4.59559696e-02f, -3.67282941e-02f, 
    -2.72624874e-02f, -1.76307914e-02f, -7.90648674e-03f,  1.83670340e-03f,  1.15251424e-02f,  2.10858716e-02f, 
     3.04471304e-02f,  3.95388944e-02f,  4.82933904e-02f,  5.66456655e-02f,  6.45340054e-02f,  7.19003487e-02f, 
     7.86908695e-02f,  8.48562395e-02f,  9.03519908e-02f,  9.51389501e-02f,  9.91834077e-02f,  1.02457361e-01f, 
     1.04938834e-01f,  1.06611872e-01f,  1.07466724e-01f,  1.07499917e-01f,  1.06714213e-01f,  1.05118588e-01f, 
     1.02728167e-01f,  9.95640680e-02f,  9.56532488e-02f,  9.10282406e-02f,  8.57269309e-02f,  7.97922261e-02f, 
     7.32717395e-02f,  6.62174249e-02f,  5.86850536e-02f,  5.07339959e-02f,  4.24265058e-02f,  3.38274345e-02f, 
     2.50036502e-02f,  1.60234844e-02f,  6.95628026e-03f, -2.12820655e-03f, -1.11602438e-02f, -2.00708281e-02f, 
    -2.87920337e-02f, -3.72576320e-02f, -4.54035426e-02f, -5.31684173e-02f, -6.04938939e-02f, -6.73253212e-02f, 
    -7.36119310e-02f, -7.93072981e-02f, -8.43697556e-02f, -8.87625537e-02f, -9.24542939e-02f, -9.54189981e-02f, 
    -9.76364402e-02f, -9.90921435e-02f, -9.97776003e-02f, -9.96902366e-02f, -9.88334463e-02f, -9.72165780e-02f, 
    -9.48547668e-02f, -9.17688999e-02f, -8.79853312e-02f, -8.35357688e-02f, -7.84569594e-02f, -7.27903677e-02f, 
    -6.65818940e-02f, -5.98814932e-02f, -5.27427333e-02f, -4.52224733e-02f, -3.73802459e-02f, -2.92780037e-02f, 
    -2.09794209e-02f, -1.25495498e-02f, -4.05425988e-03f,  4.44034349e-03f,  1.28682571e-02f,  2.11643361e-02f, 
     2.92645357e-02f,  3.71066200e-02f,  4.46305203e-02f,  5.17788267e-02f,  5.84972389e-02f,  6.47349496e-02f, 
     7.04450836e-02f,  7.55849928e-02f,  8.01165748e-02f,  8.40066506e-02f,  8.72270848e-02f,  8.97550618e-02f, 
     9.15732179e-02f,  9.26698315e-02f,  9.30387881e-02f,  9.26796720e-02f,  9.15978025e-02f,  8.98040443e-02f, 
     8.73148489e-02f,  8.41520461e-02f,  8.03426093e-02f,  7.59185468e-02f,  7.09165136e-02f,  6.53776255e-02f, 
     5.93470480e-02f,  5.28736293e-02f,  4.60095655e-02f,  3.88099545e-02f,  3.13323302e-02f,  2.36362162e-02f, 
     1.57827398e-02f,  7.83395091e-03f, -1.47413782e-04f, -8.09864153e-03f, -1.59574406e-02f, -2.36623595e-02f, 
    -3.11534717e-02f, -3.83725840e-02f, -4.52638947e-02f, -5.17743411e-02f, -5.78539729e-02f, -6.34564348e-02f, 
    -6.85392092e-02f, -7.30640654e-02f, -7.69971954e-02f, -8.03096220e-02f, -8.29772975e-02f, -8.49813524e-02f, 
    -8.63081836e-02f, -8.69495746e-02f, -8.69027157e-02f, -8.61702687e-02f, -8.47602668e-02f, -8.26860569e-02f, 
    -7.99661981e-02f, -7.66242997e-02f, -7.26887788e-02f, -6.81926752e-02f, -6.31733712e-02f, -5.76722279e-02f, 
    -5.17343061e-02f, -4.54080069e-02f, -3.87446321e-02f, -3.17980032e-02f, -2.46239897e-02f, -1.72801497e-02f, 
    -9.82518156e-03f, -2.31845300e-03f,  5.18037510e-03f,  1.26119044e-02f,  1.99174857e-02f,  2.70395921e-02f, 
     3.39223499e-02f,  4.05119404e-02f,  4.67570465e-02f,  5.26092142e-02f,  5.80232695e-02f,  6.29576539e-02f, 
     6.73747113e-02f,  7.12410320e-02f,  7.45276905e-02f,  7.72104218e-02f,  7.92698394e-02f,  8.06915952e-02f, 
     8.14664004e-02f,  8.15901977e-02f,  8.10640907e-02f,  7.98943315e-02f,  7.80922975e-02f,  7.56743792e-02f, 
     7.26617861e-02f,  6.90804346e-02f,  6.49606433e-02f,  6.03370049e-02f,  5.52479503e-02f,  4.97355660e-02f, 
     4.38451300e-02f,  3.76248662e-02f,  3.11254263e-02f,  2.43995757e-02f,  1.75017105e-02f,  1.04874823e-02f, 
     3.41321948e-03f, -3.66433362e-03f, -1.06886566e-02f, -1.76037566e-02f, -2.43547422e-02f, -3.08881238e-02f, 
    -3.71523818e-02f, -4.30982377e-02f, -4.86791529e-02f, -5.38515978e-02f, -5.85754991e-02f, -6.28144137e-02f, 
    -6.65359631e-02f, -6.97119559e-02f, -7.23186409e-02f, -7.43369897e-02f, -7.57526047e-02f, -7.65560812e-02f, 
    -7.67428560e-02f, -7.63134051e-02f, -7.52730583e-02f, -7.36321241e-02f, -7.14055927e-02f, -6.86132027e-02f, 
    -6.52791213e-02f, -6.14318004e-02f, -5.71037475e-02f, -5.23312158e-02f, -4.71539306e-02f, -4.16147519e-02f, 
    -3.57593331e-02f, -2.96357023e-02f, -2.32939478e-02f, -1.67857228e-02f, -1.01639251e-02f, -3.48213128e-03f, 
     3.20566951e-03f,  9.84566549e-03f,  1.63845318e-02f,  2.27699627e-02f,  2.89509937e-02f,  3.48784838e-02f, 
     4.05054571e-02f,  4.57875191e-02f,  5.06831561e-02f,  5.51541055e-02f,  5.91656321e-02f,  6.26867948e-02f, 
     6.56907214e-02f,  6.81547545e-02f,  7.00607045e-02f,  7.13948753e-02f,  7.21482790e-02f,  7.23165894e-02f, 
     7.19002973e-02f,  7.09044846e-02f,  6.93390331e-02f,  6.72183039e-02f,  6.45611568e-02f,  6.13906537e-02f, 
     5.77340810e-02f,  5.36223917e-02f,  4.90902973e-02f,  4.41756853e-02f,  3.89195025e-02f,  3.33653266e-02f, 
     2.75589553e-02f,  2.15482187e-02f,  1.53823433e-02f,  9.11173206e-03f,  2.78750380e-03f, -3.53899736e-03f, 
    -9.81648845e-03f, -1.59942887e-02f, -2.20226002e-02f, -2.78530676e-02f, -3.34389835e-02f, -3.87358558e-02f, 
    -4.37015752e-02f, -4.82968641e-02f, -5.24856104e-02f, -5.62350079e-02f, -5.95160314e-02f, -6.23034090e-02f, 
    -6.45760369e-02f, -6.63170246e-02f, -6.75138263e-02f, -6.81583864e-02f, -6.82471093e-02f, -6.77809819e-02f, 
    -6.67654439e-02f, -6.52104027e-02f, -6.31301405e-02f, -6.05431381e-02f, -5.74719510e-02f, -5.39430121e-02f, 
    -4.99864152e-02f, -4.56356108e-02f, -4.09271785e-02f, -3.59005358e-02f, -3.05975021e-02f, -2.50620982e-02f, 
    -1.93400931e-02f, -1.34786109e-02f, -7.52582921e-03f, -1.53047296e-03f,  4.45846396e-03f,  1.03922252e-02f, 
     1.62226043e-02f,  2.19024111e-02f,  2.73857927e-02f,  3.26286453e-02f,  3.75889120e-02f,  4.22270162e-02f, 
     4.65060678e-02f,  5.03922602e-02f,  5.38550360e-02f,  5.68673912e-02f,  5.94061299e-02f,  6.14518959e-02f, 
     6.29894927e-02f,  6.40078422e-02f,  6.45002081e-02f,  6.44641312e-02f,  6.39014463e-02f,  6.28183549e-02f, 
     6.12252434e-02f,  5.91366226e-02f,  5.65710713e-02f,  5.35509478e-02f,  5.01023211e-02f,  4.62546289e-02f, 
     4.20405644e-02f,  3.74956324e-02f,  3.26580309e-02f,  2.75681921e-02f,  2.22685138e-02f,  1.68029869e-02f, 
     1.12168479e-02f,  5.55616360e-03f, -1.32475496e-04f, -5.80242145e-03f, -1.14072870e-02f, -1.69013632e-02f, 
    -2.22399629e-02f, -2.73798231e-02f, -3.22793559e-02f, -3.68992177e-02f, -4.12022700e-02f, -4.51542301e-02f, 
    -4.87237130e-02f, -5.18825743e-02f, -5.46061242e-02f, -5.68733215e-02f, -5.86668721e-02f, -5.99735198e-02f, 
    -6.07838952e-02f, -6.10928895e-02f, -6.08993923e-02f, -6.02064781e-02f, -5.90213291e-02f, -5.73550887e-02f, 
    -5.52228853e-02f, -5.26435817e-02f, -4.96396897e-02f, -4.62371294e-02f, -4.24650256e-02f, -3.83554628e-02f, 
    -3.39432096e-02f, -2.92654225e-02f, -2.43613233e-02f, -1.92718970e-02f, -1.40395616e-02f, -8.70771728e-03f, 
    -3.32056777e-03f,  2.07744785e-03f,  7.44190391e-03f,  1.27287222e-02f,  1.78946228e-02f,  2.28975002e-02f, 
     2.76965843e-02f,  3.22530140e-02f,  3.65299534e-02f,  4.04930363e-02f,  4.41105069e-02f,  4.73536159e-02f, 
     5.01967201e-02f,  5.26175750e-02f,  5.45974724e-02f,  5.61213729e-02f,  5.71780843e-02f,  5.77601946e-02f, 
     5.78643759e-02f,  5.74910914e-02f,  5.66448597e-02f,  5.53340158e-02f,  5.35707338e-02f,  5.13708843e-02f, 
     4.87538683e-02f,  4.57425137e-02f,  4.23627999e-02f,  3.86437075e-02f,  3.46169024e-02f,  3.03165387e-02f, 
     2.57788894e-02f,  2.10421222e-02f,  1.61459251e-02f,  1.11311994e-02f,  6.03970466e-03f,  9.13695817e-04f, 
    -4.20433431e-03f, -9.27218149e-03f, -1.42480682e-02f, -1.90911878e-02f, -2.37618648e-02f, -2.82220093e-02f, 
    -3.24353766e-02f, -3.63678336e-02f, -3.99876924e-02f, -4.32659237e-02f, -4.61764207e-02f, -4.86961602e-02f, 
    -5.08054551e-02f, -5.24880386e-02f, -5.37312181e-02f, -5.45260166e-02f, -5.48671104e-02f, -5.47530531e-02f, 
    -5.41860463e-02f, -5.31721475e-02f, -5.17210363e-02f, -4.98459868e-02f, -4.75637647e-02f, -4.48944406e-02f, 
    -4.18612746e-02f, -3.84904206e-02f, -3.48107925e-02f, -3.08537797e-02f, -2.66529685e-02f, -2.22438695e-02f, 
    -1.76636682e-02f, -1.29507560e-02f, -8.14466071e-03f, -3.28544776e-03f,  1.58643018e-03f,  6.43050440e-03f, 
     1.12067405e-02f,  1.58756642e-02f,  2.03989020e-02f,  2.47393345e-02f,  2.88614617e-02f,  3.27317634e-02f, 
     3.63187992e-02f,  3.95936470e-02f,  4.25300387e-02f,  4.51045672e-02f,  4.72968940e-02f,  4.90899703e-02f, 
     5.04700047e-02f,  5.14267809e-02f,  5.19535643e-02f,  5.20472034e-02f,  5.17082287e-02f,  5.09406434e-02f, 
     4.97521048e-02f,  4.81537188e-02f,  4.61599131e-02f,  4.37884262e-02f,  4.10600706e-02f,  3.79985488e-02f, 
     3.46302622e-02f,  3.09841217e-02f,  2.70912412e-02f,  2.29847199e-02f,  1.86992847e-02f,  1.42711599e-02f, 
     9.73752669e-03f,  5.13643650e-03f,  5.06379454e-04f, -4.11408166e-03f, -8.68649476e-03f, -1.31729621e-02f, 
    -1.75363807e-02f, -2.17408089e-02f, -2.57516979e-02f, -2.95362143e-02f, -3.30635093e-02f, -3.63049622e-02f, 
    -3.92344048e-02f, -4.18283298e-02f, -4.40661418e-02f, -4.59301913e-02f, -4.74060505e-02f, -4.84825511e-02f, 
    -4.91518827e-02f, -4.94096235e-02f, -4.92548579e-02f, -4.86900251e-02f, -4.77210458e-02f, -4.63571741e-02f, 
    -4.46108878e-02f, -4.24979107e-02f, -4.00368564e-02f, -3.72492987e-02f, -3.41594108e-02f, -3.07938448e-02f, 
    -2.71814552e-02f, -2.33531198e-02f, -1.93413598e-02f, -1.51802063e-02f, -1.09048013e-02f, -6.55114338e-03f, 
    -2.15581014e-03f,  2.24443555e-03f,  6.61280814e-03f,  1.09129453e-02f,  1.51091980e-02f,  1.91667630e-02f, 
     2.30522168e-02f,  2.67335907e-02f,  3.01807365e-02f,  3.33655579e-02f,  3.62622051e-02f,  3.88473226e-02f, 
     4.11002204e-02f,  4.30030300e-02f,  4.45408790e-02f,  4.57019705e-02f,  4.64777109e-02f,  4.68627135e-02f, 
     4.68549093e-02f,  4.64554958e-02f,  4.56689373e-02f,  4.45029599e-02f,  4.29683919e-02f,  4.10791386e-02f, 
     3.88520159e-02f,  3.63066475e-02f,  3.34652385e-02f,  3.03523892e-02f,  2.69949681e-02f,  2.34217263e-02f, 
     1.96632025e-02f,  1.57513974e-02f,  1.17194459e-02f,  7.60145677e-03f,  3.43215481e-03f, -7.53454950e-04f, 
    -4.92025229e-03f, -9.03345904e-03f, -1.30587503e-02f, -1.69627406e-02f, -2.07130441e-02f, -2.42787472e-02f, 
    -2.76304969e-02f, -3.07408842e-02f, -3.35845310e-02f, -3.61384026e-02f, -3.83819804e-02f, -4.02973364e-02f, 
    -4.18693911e-02f, -4.30859849e-02f, -4.39379525e-02f, -4.44192202e-02f, -4.45268207e-02f, -4.42609489e-02f, 
    -4.36249417e-02f, -4.26251693e-02f, -4.12710965e-02f, -3.95751119e-02f, -3.75524034e-02f, -3.52209020e-02f, 
    -3.26010732e-02f, -2.97156826e-02f, -2.65897306e-02f, -2.32501339e-02f, -1.97255230e-02f, -1.60459906e-02f, 
    -1.22428645e-02f, -8.34840613e-03f, -4.39555788e-03f, -4.17641093e-04f,  3.55186529e-03f,  7.47969548e-03f, 
     1.13330289e-02f,  1.50796895e-02f,  1.86886063e-02f,  2.21298440e-02f,  2.53750227e-02f,  2.83974776e-02f, 
     3.11724713e-02f,  3.36774564e-02f,  3.58921485e-02f,  3.77988281e-02f,  3.93823848e-02f,  4.06304645e-02f, 
     4.15335460e-02f,  4.20850895e-02f,  4.22814530e-02f,  4.21220657e-02f,  4.16092724e-02f,  4.07484568e-02f, 
     3.95478256e-02f,  3.80185099e-02f,  3.61742882e-02f,  3.40316228e-02f,  3.16093467e-02f,  2.89286854e-02f, 
     2.60129143e-02f,  2.28872072e-02f,  1.95785162e-02f,  1.61151429e-02f,  1.25266872e-02f,  8.84367289e-03f, 
     5.09737541e-03f,  1.31946573e-03f, -2.45819207e-03f, -6.20382907e-03f, -9.88599514e-03f, -1.34739714e-02f, 
    -1.69377975e-02f, -2.02487225e-02f, -2.33793144e-02f, -2.63038233e-02f, -2.89981802e-02f, -3.14404213e-02f, 
    -3.36107546e-02f, -3.54916723e-02f, -3.70682427e-02f, -3.83280672e-02f, -3.92614736e-02f, -3.98615776e-02f, 
    -4.01243243e-02f, -4.00484517e-02f, -3.96356708e-02f, -3.88903731e-02f, -3.78198781e-02f, -3.64341365e-02f, 
    -3.47457457e-02f, -3.27698392e-02f, -3.05238882e-02f, -2.80276282e-02f, -2.53028218e-02f, -2.23730957e-02f, 
    -1.92637467e-02f, -1.60015029e-02f, -1.26142882e-02f, -9.13104283e-03f, -5.58138981e-03f, -1.99542434e-03f, 
     1.59649307e-03f,  5.16408174e-03f,  8.67737144e-03f,  1.21068581e-02f,  1.54239205e-02f,  1.86009100e-02f, 
     2.16114772e-02f,  2.44306994e-02f,  2.70354163e-02f,  2.94042665e-02f,  3.15179985e-02f,  3.33595356e-02f, 
     3.49141593e-02f,  3.61696229e-02f,  3.71161871e-02f,  3.77468512e-02f,  3.80571878e-02f,  3.80455485e-02f, 
     3.77129900e-02f,  3.70632810e-02f,  3.61028508e-02f,  3.48407199e-02f,  3.32884428e-02f,  3.14600053e-02f, 
     2.93716228e-02f,  2.70417408e-02f,  2.44907277e-02f,  2.17407576e-02f,  1.88156734e-02f,  1.57406803e-02f, 
     1.25421761e-02f,  9.24754692e-03f,  5.88488640e-03f,  2.48280587e-03f, -9.29864758e-04f, -4.32426314e-03f, 
    -7.67179184e-03f, -1.09442952e-02f, -1.41143886e-02f, -1.71555974e-02f, -2.00425787e-02f, -2.27514891e-02f, 
    -2.52599054e-02f, -2.75472706e-02f, -2.95949315e-02f, -3.13863062e-02f, -3.29069832e-02f, -3.41450096e-02f, 
    -3.50907101e-02f, -3.57369992e-02f, -3.60793163e-02f, -3.61156751e-02f, -3.58467080e-02f, -3.52755740e-02f, 
    -3.44080617e-02f, -3.32523628e-02f, -3.18191314e-02f, -3.01213186e-02f, -2.81740846e-02f, -2.59946393e-02f, 
    -2.36021125e-02f, -2.10173975e-02f, -1.82629132e-02f, -1.53624700e-02f, -1.23410560e-02f, -9.22456599e-03f, 
    -6.03967755e-03f, -2.81350877e-03f,  4.26514319e-04f,  3.65292660e-03f,  6.83848944e-03f,  9.95638508e-03f, 
     1.29804234e-02f,  1.58853076e-02f,  1.86468203e-02f,  2.12420277e-02f,  2.36494909e-02f,  2.58493792e-02f, 
     2.78237450e-02f,  2.95565060e-02f,  3.10338053e-02f,  3.22438572e-02f,  3.31772716e-02f,  3.38269627e-02f, 
     3.41883176e-02f,  3.42591610e-02f,  3.40397435e-02f,  3.35328606e-02f,  3.27436351e-02f,  3.16796573e-02f, 
     3.03507246e-02f,  2.87689689e-02f,  2.69484839e-02f,  2.49054827e-02f,  2.26579086e-02f,  2.02254442e-02f, 
     1.76292617e-02f,  1.48918382e-02f,  1.20368159e-02f,  9.08872468e-03f,  6.07283273e-03f,  3.01489838e-03f, 
    -5.90212194e-05f, -3.12287666e-03f, -6.15069532e-03f, -9.11695091e-03f, -1.19967033e-02f, -1.47657868e-02f, 
    -1.74011004e-02f, -1.98807214e-02f, -2.21841025e-02f, -2.42922632e-02f, -2.61879368e-02f, -2.78557311e-02f, 
    -2.92821801e-02f, -3.04559562e-02f, -3.13678907e-02f, -3.20110632e-02f, -3.23808087e-02f, -3.24749193e-02f, 
    -3.22933847e-02f, -3.18386269e-02f, -3.11153366e-02f, -3.01304804e-02f, -2.88932552e-02f, -2.74148734e-02f, 
    -2.57086673e-02f, -2.37898314e-02f, -2.16752343e-02f, -1.93835013e-02f, -1.69345799e-02f, -1.43497284e-02f, 
    -1.16513243e-02f, -8.86259097e-03f, -6.00748525e-03f, -3.11044903e-03f, -1.96143386e-04f,  2.71056658e-03f, 
     5.58512222e-03f,  8.40318833e-03f,  1.11410160e-02f,  1.37756382e-02f,  1.62850338e-02f,  1.86482666e-02f, 
     2.08457445e-02f,  2.28593437e-02f,  2.46725329e-02f,  2.62705694e-02f,  2.76405329e-02f,  2.87715470e-02f, 
     2.96547092e-02f,  3.02833419e-02f,  3.06529059e-02f,  3.07610441e-02f,  3.06076742e-02f,  3.01949567e-02f, 
     2.95271502e-02f,  2.86107876e-02f,  2.74543883e-02f,  2.60685701e-02f,  2.44657863e-02f,  2.26603655e-02f, 
     2.06682557e-02f,  1.85070033e-02f,  1.61954603e-02f,  1.37537720e-02f,  1.12030588e-02f,  8.56537064e-03f, 
     5.86336215e-03f,  3.12021752e-03f,  3.59345288e-04f, -2.39571357e-03f, -5.12158252e-03f, -7.79518527e-03f, 
    -1.03939536e-02f, -1.28961026e-02f, -1.52805838e-02f, -1.75275761e-02f, -1.96183935e-02f, -2.15357712e-02f, 
    -2.32639542e-02f, -2.47888545e-02f, -2.60981899e-02f, -2.71814567e-02f, -2.80302370e-02f, -2.86380088e-02f, 
    -2.90003996e-02f, -2.91151172e-02f, -2.89819544e-02f, -2.86028697e-02f, -2.79818317e-02f, -2.71249297e-02f, 
    -2.60401957e-02f, -2.47375751e-02f, -2.32288414e-02f, -2.15275091e-02f, -1.96486443e-02f, -1.76087964e-02f, 
    -1.54258426e-02f, -1.31187994e-02f, -1.07076937e-02f, -8.21335282e-03f, -5.65730582e-03f, -3.06143405e-03f, 
    -4.47990175e-04f,  2.16074548e-03f,  4.74260737e-03f,  7.27569124e-03f,  9.73864733e-03f,  1.21106824e-02f, 
     1.43719841e-02f,  1.65036001e-02f,  1.84878471e-02f,  2.03083286e-02f,  2.19500531e-02f,  2.33996493e-02f, 
     2.46453861e-02f,  2.56773512e-02f,  2.64874345e-02f,  2.70694463e-02f,  2.74192279e-02f,  2.75344951e-02f, 
     2.74150667e-02f,  2.70627089e-02f,  2.64811913e-02f,  2.56761950e-02f,  2.46553112e-02f,  2.34279326e-02f, 
     2.20051823e-02f,  2.03998041e-02f,  1.86260730e-02f,  1.66996483e-02f,  1.46373888e-02f,  1.24573628e-02f, 
     1.01784699e-02f,  7.82046099e-03f,  5.40366356e-03f,  2.94886537e-03f,  4.77074685e-04f, -1.99056008e-03f, 
    -4.43309957e-03f, -6.82975366e-03f, -9.16032780e-03f, -1.14051392e-02f, -1.35453571e-02f, -1.55631186e-02f, 
    -1.74416221e-02f, -1.91653203e-02f, -2.07200521e-02f, -2.20931290e-02f, -2.32734389e-02f, -2.42515770e-02f, 
    -2.50198790e-02f, -2.55724740e-02f, -2.59053977e-02f, -2.60165073e-02f, -2.59056121e-02f, -2.55744100e-02f, 
    -2.50263861e-02f, -2.42670139e-02f, -2.33034172e-02f, -2.21444752e-02f, -2.08007704e-02f, -1.92843016e-02f, 
    -1.76086143e-02f, -1.57885066e-02f, -1.38399632e-02f, -1.17800468e-02f, -9.62665505e-03f, -7.39846180e-03f, 
    -5.11473979e-03f, -2.79509520e-03f, -4.59475153e-04f,  1.87219411e-03f,  4.18004886e-03f,  6.44446028e-03f, 
     8.64630036e-03f,  1.07670050e-02f,  1.27887263e-02f,  1.46946183e-02f,  1.64687696e-02f,  1.80965074e-02f, 
     1.95644657e-02f,  2.08606409e-02f,  2.19745569e-02f,  2.28973400e-02f,  2.36217678e-02f,  2.41423032e-02f, 
     2.44552329e-02f,  2.45585559e-02f,  2.44521268e-02f,  2.41375247e-02f,  2.36181843e-02f,  2.28991883e-02f, 
     2.19873596e-02f,  2.08911372e-02f,  1.96204854e-02f,  1.81868423e-02f,  1.66029686e-02f,  1.48829260e-02f, 
     1.30418196e-02f,  1.10957823e-02f,  9.06176569e-03f,  6.95742371e-03f,  4.80095797e-03f,  2.61094572e-03f, 
     4.06163422e-04f, -1.79448120e-03f, -3.97227507e-03f, -6.10867089e-03f, -8.18559133e-03f, -1.01855447e-02f, 
    -1.20916775e-02f, -1.38880736e-02f, -1.55597947e-02f, -1.70929424e-02f, -1.84749792e-02f, -1.96945768e-02f, 
    -2.07419008e-02f, -2.16086011e-02f, -2.22879060e-02f, -2.27746496e-02f, -2.30653527e-02f, -2.31582122e-02f, 
    -2.30530853e-02f, -2.27516002e-02f, -2.22569518e-02f, -2.15740851e-02f, -2.07094459e-02f, -1.96710504e-02f, 
    -1.84683607e-02f, -1.71122258e-02f, -1.56147530e-02f, -1.39891960e-02f, -1.22499260e-02f, -1.04121226e-02f, 
    -8.49187069e-03f, -6.50583812e-03f, -4.47121574e-03f, -2.40553061e-03f, -3.26560349e-04f,  1.74792849e-03f, 
     3.80020986e-03f,  5.81284812e-03f,  7.76878436e-03f,  9.65152189e-03f,  1.14452321e-02f,  1.31348903e-02f, 
     1.47064602e-02f,  1.61469015e-02f,  1.74443880e-02f,  1.85883329e-02f,  1.95694960e-02f,  2.03800747e-02f, 
     2.10137416e-02f,  2.14657028e-02f,  2.17327470e-02f,  2.18132189e-02f,  2.17071096e-02f,  2.14159688e-02f, 
     2.09429396e-02f,  2.02927056e-02f,  1.94714591e-02f,  1.84867806e-02f,  1.73476996e-02f,  1.60644888e-02f, 
     1.46486021e-02f,  1.31126305e-02f,  1.14700918e-02f,  9.73543186e-03f,  7.92379251e-03f,  6.05090462e-03f, 
     4.13301608e-03f,  2.18669055e-03f,  2.28581333e-04f, -1.72441072e-03f, -3.65572200e-03f, -5.54887990e-03f, 
    -7.38782061e-03f, -9.15706782e-03f, -1.08417082e-02f, -1.24276657e-02f, -1.39017311e-02f, -1.52516970e-02f, 
    -1.64664949e-02f, -1.75361817e-02f, -1.84521823e-02f, -1.92071599e-02f, -1.97953056e-02f, -2.02121243e-02f, 
    -2.04547147e-02f, -2.05216098e-02f, -2.04128534e-02f, -2.01300439e-02f, -1.96761990e-02f, -1.90558123e-02f, 
    -1.82748056e-02f, -1.73404276e-02f, -1.62612067e-02f, -1.50469098e-02f, -1.37084115e-02f, -1.22575769e-02f, 
    -1.07072432e-02f, -9.07102930e-03f, -7.36320826e-03f, -5.59869147e-03f, -3.79270806e-03f, -1.96092013e-03f, 
    -1.19027325e-04f,  1.71713152e-03f,  3.53191747e-03f,  5.30986343e-03f,  7.03590331e-03f,  8.69547560e-03f, 
     1.02746006e-02f,  1.17601122e-02f,  1.31396009e-02f,  1.44016653e-02f,  1.55359973e-02f,  1.65332483e-02f, 
     1.73855033e-02f,  1.80859434e-02f,  1.86291305e-02f,  1.90110277e-02f,  1.92289384e-02f,  1.92815880e-02f, 
     1.91691688e-02f,  1.88932135e-02f,  1.84567183e-02f,  1.78639790e-02f,  1.71206377e-02f,  1.62336473e-02f, 
     1.52110920e-02f,  1.40622274e-02f,  1.27973510e-02f,  1.14277163e-02f,  9.96541843e-03f,  8.42333112e-03f, 
     6.81491991e-03f,  5.15420944e-03f,  3.45559138e-03f,  1.73374462e-03f,  3.49154958e-06f, -1.72033182e-03f, 
    -3.42300908e-03f, -5.09002877e-03f, -6.70728983e-03f, -8.26110592e-03f, -9.73843101e-03f, -1.11269177e-02f, 
    -1.24149972e-02f, -1.35920411e-02f, -1.46483675e-02f, -1.55754162e-02f, -1.63657097e-02f, -1.70130158e-02f, 
    -1.75123254e-02f, -1.78599156e-02f, -1.80533642e-02f, -1.80916471e-02f, -1.79749596e-02f, -1.77049199e-02f, 
    -1.72844059e-02f, -1.67175734e-02f, -1.60098348e-02f, -1.51677846e-02f, -1.41991369e-02f, -1.31126308e-02f, 
    -1.19180614e-02f, -1.06260158e-02f, -9.24795820e-03f, -7.79599691e-03f, -6.28282689e-03f, -4.72166017e-03f, 
    -3.12602130e-03f, -1.50971188e-03f,  1.13358008e-04f,  1.72924640e-03f,  3.32419869e-03f,  4.88457483e-03f, 
     6.39719332e-03f,  7.84928507e-03f,  9.22860374e-03f,  1.05236737e-02f,  1.17237027e-02f,  1.28187631e-02f, 
     1.37999219e-02f,  1.46591627e-02f,  1.53896448e-02f,  1.59855771e-02f,  1.64423748e-02f,  1.67566705e-02f, 
     1.69263151e-02f,  1.69504088e-02f,  1.68293192e-02f,  1.65646048e-02f,  1.61591292e-02f,  1.56168830e-02f, 
     1.49430466e-02f,  1.41438870e-02f,  1.32267343e-02f,  1.21999194e-02f,  1.10726150e-02f,  9.85491162e-03f, 
     8.55755480e-03f,  7.19198626e-03f,  5.77013714e-03f,  4.30443841e-03f,  2.80758857e-03f,  1.29252809e-03f, 
    -2.27683018e-04f, -1.74000213e-03f, -3.23153173e-03f, -4.68956247e-03f, -6.10171563e-03f, -7.45612506e-03f, 
    -8.74136426e-03f, -9.94672023e-03f, -1.10621909e-02f, -1.20785406e-02f, -1.29874795e-02f, -1.37816456e-02f, 
    -1.44546479e-02f, -1.50012468e-02f, -1.54172106e-02f, -1.56995155e-02f, -1.58462779e-02f, -1.58567437e-02f, 
    -1.57313825e-02f, -1.54717967e-02f, -1.50807184e-02f, -1.45620705e-02f, -1.39207297e-02f, -1.31627253e-02f, 
    -1.22950111e-02f, -1.13254027e-02f, -1.02626834e-02f, -9.11627932e-03f, -7.89634415e-03f, -6.61364765e-03f, 
    -5.27939952e-03f, -3.90525708e-03f, -2.50314317e-03f, -1.08517576e-03f,  3.36418391e-04f,  1.74945190e-03f, 
     3.14186033e-03f,  4.50178261e-03f,  5.81769448e-03f,  7.07851939e-03f,  8.27365386e-03f,  9.39310326e-03f, 
     1.04276320e-02f,  1.13686527e-02f,  1.22085379e-02f,  1.29404450e-02f,  1.35585678e-02f,  1.40580446e-02f, 
     1.44350939e-02f,  1.46869568e-02f,  1.48120098e-02f,  1.48096348e-02f,  1.46804295e-02f,  1.44259781e-02f, 
     1.40489668e-02f,  1.35531325e-02f,  1.29432014e-02f,  1.22248563e-02f,  1.14046959e-02f,  1.04901687e-02f, 
     9.48948107e-03f,  8.41156632e-03f,  7.26596347e-03f,  6.06280447e-03f,  4.81257444e-03f,  3.52622627e-03f, 
     2.21492506e-03f,  8.89983592e-04f, -4.37153812e-04f, -1.75513167e-03f, -3.05265494e-03f, -4.31872834e-03f, 
    -5.54261874e-03f, -6.71396264e-03f, -7.82302244e-03f, -8.86045250e-03f, -9.81773278e-03f, -1.06869351e-02f, 
    -1.14610023e-02f, -1.21336754e-02f, -1.26995953e-02f, -1.31543908e-02f, -1.34945718e-02f, -1.37177266e-02f, 
    -1.38224110e-02f, -1.38082286e-02f, -1.36757739e-02f, -1.34266887e-02f, -1.30635886e-02f, -1.25900369e-02f, 
    -1.20105709e-02f, -1.13305978e-02f, -1.05563538e-02f, -9.69485926e-03f, -8.75389081e-03f, -7.74181164e-03f, 
    -6.66761679e-03f, -5.54076187e-03f, -4.37111830e-03f, -3.16893052e-03f, -1.94457115e-03f, -7.08705149e-04f, 
     5.28079290e-04f,  1.75515870e-03f,  2.96204304e-03f,  4.13848585e-03f,  5.27451557e-03f,  6.36060039e-03f, 
     7.38755863e-03f,  8.34692530e-03f,  9.23070802e-03f,  1.00316534e-02f,  1.07432528e-02f,  1.13597680e-02f, 
     1.18763350e-02f,  1.22889283e-02f,  1.25944631e-02f,  1.27907515e-02f,  1.28765994e-02f,  1.28517102e-02f, 
     1.27167966e-02f,  1.24734480e-02f,  1.21242371e-02f,  1.16725839e-02f,  1.11228281e-02f,  1.04800592e-02f, 
     9.75022575e-03f,  8.93990424e-03f,  8.05644990e-03f,  7.10768601e-03f,  6.10205625e-03f,  5.04843878e-03f, 
     3.95605458e-03f,  2.83441418e-03f,  1.69331277e-03f,  5.42568186e-04f, -6.07877124e-04f, -1.74818575e-03f, 
    -2.86860405e-03f, -3.95962685e-03f, -5.01201657e-03f, -6.01690058e-03f, -6.96589716e-03f, -7.85110424e-03f, 
    -8.66518231e-03f, -9.40145619e-03f, -1.00540095e-02f, -1.06175123e-02f, -1.10876024e-02f, -1.14606062e-02f, 
    -1.17337519e-02f, -1.19051415e-02f, -1.19737311e-02f, -1.19393909e-02f, -1.18028751e-02f, -1.15657387e-02f, 
    -1.12305357e-02f, -1.08005049e-02f, -1.02797519e-02f, -9.67318729e-03f, -8.98632838e-03f, -8.22543877e-03f, 
    -7.39737215e-03f, -6.50950785e-03f, -5.56975395e-03f, -4.58632875e-03f, -3.56792674e-03f, -2.52340823e-03f, 
    -1.46183597e-03f, -3.92391156e-04f,  6.75701684e-04f,  1.73331709e-03f,  2.77141530e-03f,  3.78118353e-03f, 
     4.75407672e-03f,  5.68193005e-03f,  6.55698994e-03f,  7.37195674e-03f,  8.12013345e-03f,  8.79539509e-03f, 
     9.39225030e-03f,  9.90597190e-03f,  1.03324819e-02f,  1.06685242e-02f,  1.09116177e-02f,  1.10600973e-02f, 
     1.11130936e-02f,  1.10705983e-02f,  1.09333788e-02f,  1.07030445e-02f,  1.03819949e-02f,  9.97335332e-03f, 
     9.48107464e-03f,  8.90968434e-03f,  8.26449756e-03f,  7.55132972e-03f,  6.77664458e-03f,  5.94731079e-03f, 
     5.07073939e-03f,  4.15462520e-03f,  3.20700306e-03f,  2.23616222e-03f,  1.25050340e-03f,  2.58592562e-04f, 
    -7.31105992e-04f, -1.71003848e-03f, -2.66991104e-03f, -3.60254805e-03f, -4.50009626e-03f, -5.35500152e-03f, 
    -6.16013372e-03f, -6.90880302e-03f, -7.59484887e-03f, -8.21267759e-03f, -8.75730297e-03f, -9.22437062e-03f, 
    -9.61022818e-03f, -9.91196266e-03f, -1.01273334e-02f, -1.02549146e-02f, -1.02939949e-02f, -1.02446487e-02f, 
    -1.01077102e-02f, -9.88473930e-03f, -9.57804506e-03f, -9.19065219e-03f, -8.72623997e-03f, -8.18914967e-03f, 
    -7.58431711e-03f, -6.91725624e-03f, -6.19393169e-03f, -5.42085678e-03f, -4.60486090e-03f, -3.75314479e-03f, 
    -2.87318400e-03f, -1.97263669e-03f, -1.05936420e-03f, -1.41184633e-04f,  7.73935206e-04f,  1.67818033e-03f, 
     2.56387121e-03f,  3.42348245e-03f,  4.24972968e-03f,  5.03575853e-03f,  5.77493594e-03f,  6.46117800e-03f, 
     7.08885263e-03f,  7.65282423e-03f,  8.14856911e-03f,  8.57214716e-03f,  8.92027019e-03f,  9.19029194e-03f, 
     9.38027470e-03f,  9.48895025e-03f,  9.51578399e-03f,  9.46091429e-03f,  9.32518284e-03f,  9.11016180e-03f, 
     8.81806173e-03f,  8.45171440e-03f,  8.01466407e-03f,  7.51094572e-03f,  6.94521826e-03f,  6.32261691e-03f, 
     5.64875255e-03f,  4.92963671e-03f,  4.17165548e-03f,  3.38149573e-03f,  2.56610069e-03f,  1.73253154e-03f, 
     8.88083719e-04f,  4.00140997e-05f, -8.04377007e-04f, -1.63786496e-03f, -2.45336348e-03f, -3.24394120e-03f, 
    -4.00297149e-03f, -4.72406012e-03f, -5.40122825e-03f, -6.02886353e-03f, -6.60184564e-03f, -7.11547043e-03f, 
    -7.56567204e-03f, -7.94886879e-03f, -8.26207948e-03f, -8.50298133e-03f, -8.66984745e-03f, -8.76158174e-03f, 
    -8.77778600e-03f, -8.71866903e-03f, -8.58510255e-03f, -8.37858953e-03f, -8.10125332e-03f, -7.75580633e-03f, 
    -7.34555568e-03f, -6.87431135e-03f, -6.34642360e-03f, -5.76669768e-03f, -5.14031767e-03f, -4.47294897e-03f, 
    -3.77043291e-03f, -3.03903272e-03f, -2.28511456e-03f, -1.51527024e-03f, -7.36178447e-04f,  4.54225562e-05f, 
     8.22859022e-04f,  1.58943109e-03f,  2.33866278e-03f,  3.06420334e-03f,  3.75990680e-03f,  4.42002538e-03f, 
     5.03901750e-03f,  5.61180111e-03f,  6.13366220e-03f,  6.60043272e-03f,  7.00831931e-03f,  7.35414500e-03f, 
     7.63524392e-03f,  7.84953557e-03f,  7.99547645e-03f,  8.07218955e-03f,  8.07933095e-03f,  8.01721906e-03f, 
     7.88666864e-03f,  7.68919343e-03f,  7.42679720e-03f,  7.10202788e-03f,  6.71802523e-03f,  6.27832934e-03f, 
     5.78702253e-03f,  5.24853339e-03f,  4.66776048e-03f,  4.04985033e-03f,  3.40032055e-03f,  2.72486114e-03f, 
     2.02943382e-03f,  1.32005555e-03f,  6.02922229e-04f, -1.15810889e-04f, -8.29962401e-04f, -1.53344695e-03f, 
    -2.22024937e-03f, -2.88460828e-03f, -3.52090915e-03f, -4.12386103e-03f, -4.68844782e-03f, -5.21000854e-03f, 
    -5.68433641e-03f, -6.10753890e-03f, -6.47629357e-03f, -6.78770430e-03f, -7.03936807e-03f, -7.22944790e-03f, 
    -7.35662441e-03f, -7.42012069e-03f, -7.41971164e-03f, -7.35573757e-03f, -7.22905724e-03f, -7.04107429e-03f, 
    -6.79370122e-03f, -6.48940038e-03f, -6.13102314e-03f, -5.72192873e-03f, -5.26590521e-03f, -4.76707464e-03f, 
    -4.22993214e-03f, -3.65930825e-03f, -3.06022345e-03f, -2.43797793e-03f, -1.79803310e-03f, -1.14594988e-03f, 
    -4.87389180e-04f,  1.71985886e-04f,  8.26505744e-04f,  1.47057292e-03f,  2.09875564e-03f,  2.70572827e-03f, 
     3.28638788e-03f,  3.83592350e-03f,  4.34975506e-03f,  4.82368759e-03f,  5.25383132e-03f,  5.63677359e-03f, 
     5.96942535e-03f,  6.24924092e-03f,  6.47405650e-03f,  6.64226721e-03f,  6.75269253e-03f,  6.80469430e-03f, 
     6.79815717e-03f,  6.73340631e-03f,  6.61130455e-03f,  6.43322863e-03f,  6.20094526e-03f,  5.91677710e-03f, 
     5.58340169e-03f,  5.20393196e-03f,  4.78187614e-03f,  4.32106320e-03f,  3.82565711e-03f,  3.30005613e-03f, 
     2.74895362e-03f,  2.17719303e-03f,  1.58978015e-03f,  9.91844057e-04f,  3.88540330e-04f, -2.14916878e-04f, 
    -8.13361192e-04f, -1.40168257e-03f, -1.97489740e-03f, -2.52818059e-03f, -3.05688539e-03f, -3.55662656e-03f, 
    -4.02326574e-03f, -4.45296958e-03f, -4.84228652e-03f, -5.18803438e-03f, -5.48755315e-03f, -5.73848611e-03f, 
    -5.93891991e-03f, -6.08745626e-03f, -6.18305471e-03f, -6.22520840e-03f, -6.21382472e-03f, -6.14928419e-03f, 
    -6.03244633e-03f, -5.86455879e-03f, -5.64736180e-03f, -5.38296537e-03f, -5.07389363e-03f, -4.72301916e-03f, 
    -4.33361321e-03f, -3.90915761e-03f, -3.45353173e-03f, -2.97077347e-03f, -2.46516689e-03f, -1.94119584e-03f, 
    -1.40340595e-03f, -8.56512644e-04f, -3.05232133e-04f,  2.45691031e-04f,  7.91538060e-04f,  1.32763724e-03f, 
     1.84949345e-03f,  2.35267547e-03f,  2.83299113e-03f,  3.28645035e-03f,  3.70931698e-03f,  4.09812665e-03f, 
     4.44973511e-03f,  4.76135341e-03f,  5.03050354e-03f,  5.25513155e-03f,  5.43353323e-03f,  5.56447821e-03f, 
     5.64705544e-03f,  5.68083601e-03f,  5.66583437e-03f,  5.60238431e-03f,  5.49135375e-03f,  5.33391723e-03f, 
     5.13169207e-03f,  4.88664671e-03f,  4.60113202e-03f,  4.27780860e-03f,  3.91964875e-03f,  3.52989866e-03f, 
     3.11212090e-03f,  2.66999053e-03f,  2.20744344e-03f,  1.72859110e-03f,  1.23756351e-03f,  7.38678150e-04f, 
     2.36236760e-04f, -2.65462378e-04f, -7.62072815e-04f, -1.24943395e-03f, -1.72337956e-03f, -2.17993754e-03f, 
    -2.61530935e-03f, -3.02588421e-03f, -3.40825196e-03f, -3.75935360e-03f, -4.07630652e-03f, -4.35660760e-03f, 
    -4.59808398e-03f, -4.79883718e-03f, -4.95743843e-03f, -5.07271280e-03f, -5.14393833e-03f, -5.17077608e-03f, 
    -5.15318763e-03f, -5.09164480e-03f, -4.98686807e-03f, -4.84002285e-03f, -4.65260103e-03f, -4.42642977e-03f, 
    -4.16366446e-03f, -3.86678300e-03f, -3.53847751e-03f, -3.18177292e-03f, -2.79986847e-03f, -2.39618401e-03f, 
    -1.97429017e-03f, -1.53788782e-03f, -1.09083664e-03f, -6.36973406e-04f, -1.80264329e-04f,  2.75399352e-04f, 
     7.26104424e-04f,  1.16802598e-03f,  1.59744046e-03f,  2.01073128e-03f,  2.40446819e-03f,  2.77538562e-03f, 
     3.12044615e-03f,  3.43683203e-03f,  3.72202393e-03f,  3.97374850e-03f,  4.19002854e-03f,  4.36925418e-03f, 
     4.51006070e-03f,  4.61152219e-03f,  4.67293053e-03f,  4.69404975e-03f,  4.67490366e-03f,  4.61589307e-03f, 
     4.51775252e-03f,  4.38154991e-03f,  4.20868532e-03f,  4.00082377e-03f,  3.75997274e-03f,  3.48836415e-03f, 
     3.18851504e-03f,  2.86314343e-03f,  2.51519536e-03f,  2.14776743e-03f,  1.76411750e-03f,  1.36763070e-03f, 
     9.61751835e-04f,  5.50052405e-04f,  1.36015058e-04f, -2.76720943e-04f, -6.84698152e-04f, -1.08442387e-03f, 
    -1.47253691e-03f, -1.84578853e-03f, -2.20105818e-03f, -2.53544188e-03f, -2.84616998e-03f, -3.13076058e-03f, 
    -3.38689733e-03f, -3.61260297e-03f, -3.80606518e-03f, -3.96589267e-03f, -4.09087232e-03f, -4.18013173e-03f, 
    -4.23315965e-03f, -4.24970953e-03f, -4.22981560e-03f, -4.17392494e-03f, -4.08267808e-03f, -3.95709577e-03f, 
    -3.79845153e-03f, -3.60829670e-03f, -3.38844338e-03f, -3.14094669e-03f, -2.86809742e-03f, -2.57237442e-03f, 
    -2.25643831e-03f, -1.92312165e-03f, -1.57535841e-03f, -1.21624129e-03f, -8.48868370e-04f, -4.76457354e-04f, 
    -1.02227062e-04f,  2.70659894e-04f,  6.38948957e-04f,  9.99596773e-04f,  1.34950884e-03f,  1.68579412e-03f, 
     2.00565112e-03f,  2.30644176e-03f,  2.58570970e-03f,  2.84121989e-03f,  3.07087670e-03f,  3.27296771e-03f, 
     3.44584695e-03f,  3.58825627e-03f,  3.69915439e-03f,  3.77779535e-03f,  3.82369144e-03f,  3.83666312e-03f, 
     3.81678507e-03f,  3.76444486e-03f,  3.68027755e-03f,  3.56519883e-03f,  3.42038694e-03f,  3.24725992e-03f, 
     3.04745181e-03f,  2.82287635e-03f,  2.57555610e-03f,  2.30778342e-03f,  2.02193938e-03f,  1.72060684e-03f, 
     1.40642226e-03f,  1.08218540e-03f,  7.50708128e-04f,  4.14852040e-04f,  7.75468400e-05f, -2.58336678e-04f, 
    -5.89954675e-04f, -9.14464553e-04f, -1.22917409e-03f, -1.53142096e-03f, -1.81874942e-03f, -2.08875765e-03f, 
    -2.33925204e-03f, -2.56824046e-03f, -2.77387464e-03f, -2.95457151e-03f, -3.10891286e-03f, -3.23576957e-03f, 
    -3.33422309e-03f, -3.40361730e-03f, -3.44352432e-03f, -3.45380945e-03f, -3.43454926e-03f, -3.38612359e-03f, 
    -3.30910238e-03f, -3.20434413e-03f, -3.07289782e-03f, -2.91605448e-03f, -2.73534798e-03f, -2.53242439e-03f, 
    -2.30918427e-03f, -2.06766744e-03f, -1.81002532e-03f, -1.53857461e-03f, -1.25572213e-03f, -9.63956082e-04f, 
    -6.65804929e-04f, -3.63875198e-04f, -6.07622519e-05f,  2.40955893e-04f,  5.38685581e-04f,  8.29936911e-04f, 
     1.11224977e-03f,  1.38328230e-03f,  1.64080028e-03f,  1.88265574e-03f,  2.10694670e-03f,  2.31181334e-03f, 
     2.49567938e-03f,  2.65707799e-03f,  2.79477329e-03f,  2.90778929e-03f,  2.99526804e-03f,  3.05666792e-03f, 
     3.09159989e-03f,  3.09996074e-03f,  3.08183486e-03f,  3.03757314e-03f,  2.96768997e-03f,  2.87296391e-03f, 
     2.75438271e-03f,  2.61305979e-03f,  2.45041225e-03f,  2.26792371e-03f,  2.06728115e-03f,  1.85034398e-03f, 
     1.61901728e-03f,  1.37543970e-03f,  1.12168235e-03f,  8.60048928e-04f,  5.92781787e-04f,  3.22217129e-04f, 
     5.06437951e-05f, -2.19547817e-04f, -4.86132510e-04f, -7.46817210e-04f, -9.99443627e-04f, -1.24188233e-03f, 
    -1.47217245e-03f, -1.68839648e-03f, -1.88883105e-03f, -2.07184785e-03f, -2.23601745e-03f, -2.38006048e-03f, 
    -2.50288118e-03f, -2.60358292e-03f, -2.68144174e-03f, -2.73595307e-03f, -2.76679595e-03f, -2.77388624e-03f, 
    -2.75729794e-03f, -2.71735188e-03f, -2.65451985e-03f, -2.56952130e-03f, -2.46319204e-03f, -2.33660956e-03f, 
    -2.19096493e-03f, -2.02765268e-03f, -1.84815939e-03f, -1.65412932e-03f, -1.44731483e-03f, -1.22956426e-03f, 
    -1.00280075e-03f, -7.69022668e-04f, -5.30268510e-04f, -2.88586883e-04f, -4.60956253e-05f,  1.95186584e-04f, 
     4.33161045e-04f,  6.65873263e-04f,  8.91328897e-04f,  1.10770620e-03f,  1.31316296e-03f,  1.50610067e-03f, 
     1.68489795e-03f,  1.84814923e-03f,  1.99458512e-03f,  2.12304250e-03f,  2.23258384e-03f,  2.32237953e-03f, 
     2.39181962e-03f,  2.44043032e-03f,  2.46796938e-03f,  2.47430968e-03f,  2.45957831e-03f,  2.42401283e-03f, 
     2.36808884e-03f,  2.29238471e-03f,  2.19773378e-03f,  2.08501666e-03f,  1.95534528e-03f,  1.80993801e-03f, 
     1.65014053e-03f,  1.47739854e-03f,  1.29329221e-03f,  1.09944593e-03f,  8.97596290e-04f,  6.89486470e-04f, 
     4.76967544e-04f,  2.61847472e-04f,  4.59979030e-05f, -1.68770369e-04f, -3.80612759e-04f, -5.87744421e-04f, 
    -7.88452414e-04f, -9.81081718e-04f, -1.16402219e-03f, -1.33580811e-03f, -1.49504859e-03f, -1.64047131e-03f, 
    -1.77095587e-03f, -1.88548340e-03f, -1.98318254e-03f, -2.06335667e-03f, -2.12544333e-03f, -2.16903096e-03f, 
    -2.19389731e-03f, -2.19994674e-03f, -2.18726700e-03f, -2.15609170e-03f, -2.10683457e-03f, -2.04002290e-03f, 
    -1.95633800e-03f, -1.85665258e-03f, -1.74189023e-03f, -1.61313165e-03f, -1.47159921e-03f, -1.31856217e-03f, 
    -1.15541374e-03f, -9.83590913e-04f, -8.04645529e-04f, -6.20138811e-04f, -4.31664744e-04f, -2.40859759e-04f, 
    -4.93718861e-05f,  1.41183920e-04f,  3.29184443e-04f,  5.13049545e-04f,  6.91252710e-04f,  8.62329668e-04f, 
     1.02486089e-03f,  1.17753306e-03f,  1.31912530e-03f,  1.44851584e-03f,  1.56468190e-03f,  1.66675270e-03f, 
     1.75393226e-03f,  1.82562545e-03f,  1.88129935e-03f,  1.92062935e-03f,  1.94336360e-03f,  1.94946381e-03f, 
     1.93898469e-03f,  1.91211060e-03f,  1.86925265e-03f,  1.81081128e-03f,  1.73745800e-03f,  1.64989979e-03f, 
     1.54896085e-03f,  1.43565148e-03f,  1.31095906e-03f,  1.17607031e-03f,  1.03219054e-03f,  8.80596006e-04f, 
     7.22634695e-04f,  5.59715925e-04f,  3.93223384e-04f,  2.24602808e-04f,  5.53223372e-05f, -1.13204206e-04f, 
    -2.79527886e-04f, -4.42273875e-04f, -6.00090187e-04f, -7.51646708e-04f, -8.95738714e-04f, -1.03117771e-03f, 
    -1.15687770e-03f, -1.27187587e-03f, -1.37523688e-03f, -1.46618576e-03f, -1.54403989e-03f, -1.60825931e-03f, 
    -1.65836399e-03f, -1.69405240e-03f, -1.71514183e-03f, -1.72154028e-03f, -1.71331327e-03f, -1.69063272e-03f, 
    -1.65381037e-03f, -1.60326168e-03f, -1.53948863e-03f, -1.46318779e-03f, -1.37503217e-03f, -1.27591969e-03f, 
    -1.16672308e-03f, -1.04846883e-03f, -9.22232848e-04f, -7.89108246e-04f, -6.50329911e-04f, -5.07057241e-04f, 
    -3.60579584e-04f, -2.12138548e-04f, -6.30166060e-05f,  8.55107333e-05f,  2.32212191e-04f,  3.75851456e-04f, 
     5.15213418e-04f,  6.49182851e-04f,  7.76642588e-04f,  8.96585347e-04f,  1.00803198e-03f,  1.11010987e-03f, 
     1.20203475e-03f,  1.28308439e-03f,  1.35268783e-03f,  1.41030687e-03f,  1.45558664e-03f,  1.48819124e-03f, 
     1.50798717e-03f,  1.51486502e-03f,  1.50888467e-03f,  1.49022209e-03f,  1.45906012e-03f,  1.41583581e-03f, 
     1.36095722e-03f,  1.29499749e-03f,  1.21859138e-03f,  1.13249419e-03f,  1.03745344e-03f,  9.34384957e-04f, 
     8.24209226e-04f,  7.07921644e-04f,  5.86535461e-04f,  4.61118668e-04f,  3.32797940e-04f,  2.02615430e-04f, 
     7.17560319e-05f, -5.87215139e-05f, -1.87700771e-04f, -3.14093799e-04f, -4.36855019e-04f, -5.54982470e-04f, 
    -6.67514567e-04f, -7.73539543e-04f, -8.72216549e-04f, -9.62754726e-04f, -1.04446836e-03f, -1.11673823e-03f, 
    -1.17901020e-03f, -1.23084835e-03f, -1.27191263e-03f, -1.30189831e-03f, -1.32066941e-03f, -1.32816613e-03f, 
    -1.32437715e-03f, -1.30944714e-03f, -1.28360668e-03f, -1.24710492e-03f, -1.20038313e-03f, -1.14391116e-03f, 
    -1.07822250e-03f, -1.00394823e-03f, -9.21799577e-04f, -8.32520513e-04f, -7.36916195e-04f, -6.35853312e-04f, 
    -5.30218398e-04f, -4.20950684e-04f, -3.08981087e-04f, -1.95310152e-04f, -8.08721649e-05f,  3.33481785e-05f, 
     1.46369769e-04f,  2.57271691e-04f,  3.65123878e-04f,  4.69053422e-04f,  5.68205019e-04f,  6.61777482e-04f, 
     7.49035427e-04f,  8.29295760e-04f,  9.01919035e-04f,  9.66370937e-04f,  1.02218113e-03f,  1.06892877e-03f, 
     1.10630552e-03f,  1.13406370e-03f,  1.15204451e-03f,  1.16019052e-03f,  1.15848806e-03f,  1.14706630e-03f, 
     1.12606449e-03f,  1.09574589e-03f,  1.05645362e-03f,  1.00859266e-03f,  9.52601766e-04f,  8.89057609e-04f, 
     8.18535938e-04f,  7.41697389e-04f,  6.59241262e-04f,  5.71884368e-04f,  4.80414698e-04f,  3.85677252e-04f, 
     2.88406796e-04f,  1.89536836e-04f,  8.98491837e-05f, -9.79888746e-06f, -1.08531507e-04f, -2.05575498e-04f, 
    -3.00092231e-04f, -3.91327952e-04f, -4.78537671e-04f, -5.61003964e-04f, -6.38090388e-04f, -7.09209697e-04f, 
    -7.73747838e-04f, -8.31297964e-04f, -8.81364804e-04f, -9.23641236e-04f, -9.57793553e-04f, -9.83624619e-04f, 
    -1.00098424e-03f, -1.00979404e-03f, -1.01003977e-03f, -1.00180772e-03f, -9.85219816e-04f, -9.60506778e-04f, 
    -9.27905874e-04f, -8.87790902e-04f, -8.40553609e-04f, -7.86632276e-04f, -7.26559669e-04f, -6.60872173e-04f, 
    -5.90177860e-04f, -5.15099219e-04f, -4.36341554e-04f, -3.54526447e-04f, -2.70436804e-04f, -1.84757234e-04f, 
    -9.82406108e-05f, -1.16228429e-05f,  7.44116225e-05f,  1.59099493e-04f,  2.41739119e-04f,  3.21707034e-04f, 
     3.98276352e-04f,  4.70887555e-04f,  5.38973046e-04f,  6.01940918e-04f,  6.59368174e-04f,  7.10783030e-04f, 
     7.55802336e-04f,  7.94127086e-04f,  8.25478803e-04f,  8.49639386e-04f,  8.66487952e-04f,  8.75935969e-04f, 
     8.77948893e-04f,  8.72611584e-04f,  8.59994515e-04f,  8.40271458e-04f,  8.13696181e-04f,  7.80491851e-04f, 
     7.41053306e-04f,  6.95727202e-04f,  6.44936090e-04f,  5.89181503e-04f,  5.28946796e-04f,  4.64790448e-04f, 
     3.97272420e-04f,  3.27000597e-04f,  2.54559578e-04f,  1.80597276e-04f,  1.05760446e-04f,  3.06209047e-05f, 
    -4.41172003e-05f, -1.17884760e-04f, -1.90032814e-04f, -2.60000039e-04f, -3.27213235e-04f, -3.91110007e-04f, 
    -4.51226928e-04f, -5.07042112e-04f, -5.58194586e-04f, -6.04189222e-04f, -6.44816381e-04f, -6.79653847e-04f, 
    -7.08557315e-04f, -7.31282579e-04f, -7.47702169e-04f, -7.57731688e-04f, -7.61359812e-04f, -7.58589885e-04f, 
    -7.49503361e-04f, -7.34226582e-04f, -7.12935677e-04f, -6.85882645e-04f, -6.53307567e-04f, -6.15569562e-04f, 
    -5.72978650e-04f, -5.25977418e-04f, -4.74963705e-04f, -4.20426590e-04f, -3.62819514e-04f, -3.02647353e-04f, 
    -2.40497241e-04f, -1.76810216e-04f, -1.12210871e-04f, -4.71976690e-05f,  1.76624641e-05f,  8.18440593e-05f, 
     1.44804207e-04f,  2.06021410e-04f,  2.65025446e-04f,  3.21327783e-04f,  3.74487008e-04f,  4.24062432e-04f, 
     4.69715655e-04f,  5.11042943e-04f,  5.47794530e-04f,  5.79655168e-04f,  6.06446384e-04f,  6.27934546e-04f, 
     6.44010762e-04f,  6.54614698e-04f,  6.59636425e-04f,  6.59157826e-04f,  6.53158826e-04f,  6.41794049e-04f, 
     6.25154916e-04f,  6.03470855e-04f,  5.76917242e-04f,  5.45789736e-04f,  5.10368292e-04f,  4.70998661e-04f, 
     4.28021656e-04f,  3.81834126e-04f,  3.32863326e-04f,  2.81489629e-04f,  2.28231239e-04f,  1.73484261e-04f, 
     1.17756607e-04f,  6.14881351e-05f,  5.17778269e-06f, -5.07352374e-05f, -1.05745987e-04f, -1.59454662e-04f, 
    -2.11394268e-04f, -2.61151905e-04f, -3.08351703e-04f, -3.52598590e-04f, -3.93545002e-04f, -4.30916147e-04f, 
    -4.64387406e-04f, -4.93756593e-04f, -5.18755281e-04f, -5.39265493e-04f, -5.55137934e-04f, -5.66259303e-04f, 
    -5.72606783e-04f, -5.74140344e-04f, -5.70903292e-04f, -5.62934741e-04f, -5.50388898e-04f, -5.33351962e-04f, 
    -5.12028510e-04f, -4.86612455e-04f, -4.57392981e-04f, -4.24578939e-04f, -3.88503808e-04f, -3.49487518e-04f, 
    -3.07895836e-04f, -2.64036522e-04f, -2.18356445e-04f, -1.71198300e-04f, -1.22998901e-04f, -7.41392080e-05f, 
    -2.50280393e-05f,  2.38852047e-05f,  7.22663332e-05f,  1.19659647e-04f,  1.65718806e-04f,  2.10055385e-04f, 
     2.52324173e-04f,  2.92190427e-04f,  3.29337577e-04f,  3.63510150e-04f,  3.94385715e-04f,  4.21803288e-04f, 
     4.45519433e-04f,  4.65391876e-04f,  4.81270460e-04f,  4.93057625e-04f,  5.00688030e-04f,  5.04121708e-04f, 
     5.03379627e-04f,  4.98485604e-04f,  4.89499566e-04f,  4.76539317e-04f,  4.59760023e-04f,  4.39274612e-04f, 
     4.15334876e-04f,  3.88103885e-04f,  3.57902146e-04f,  3.24908089e-04f,  2.89490480e-04f,  2.51922687e-04f, 
     2.12512220e-04f,  1.71637404e-04f,  1.29609890e-04f,  8.67866183e-05f,  4.35312276e-05f,  1.98808307e-07f, 
    -4.28589070e-05f, -8.52865394e-05f, -1.26765698e-04f, -1.66922292e-04f, -2.05456466e-04f, -2.42095652e-04f, 
    -2.76487494e-04f, -3.08425602e-04f, -3.37638832e-04f, -3.63923042e-04f, -3.87022898e-04f, -4.06875144e-04f, 
    -4.23245129e-04f, -4.36071615e-04f, -4.45236993e-04f, -4.50724682e-04f, -4.52491230e-04f, -4.50548104e-04f, 
    -4.44936790e-04f, -4.35725612e-04f, -4.22987381e-04f, -4.06882738e-04f, -3.87548587e-04f, -3.65123104e-04f, 
    -3.39860288e-04f, -3.11947486e-04f, -2.81618569e-04f, -2.49166817e-04f, -2.14824344e-04f, -1.78876370e-04f, 
    -1.41684861e-04f, -1.03466427e-04f, -6.45996088e-05f, -2.53738050e-05f,  1.39035721e-05f,  5.28977578e-05f, 
     9.13010773e-05f,  1.28809554e-04f,  1.65139924e-04f,  2.00005346e-04f,  2.33095696e-04f,  2.64232233e-04f, 
     2.93070034e-04f,  3.19508024e-04f,  3.43252648e-04f,  3.64165224e-04f,  3.82074036e-04f,  3.96868082e-04f, 
     4.08408250e-04f,  4.16671952e-04f,  4.21556517e-04f,  4.23035822e-04f,  4.21172111e-04f,  4.15928838e-04f, 
     4.07377025e-04f,  3.95568598e-04f,  3.80628038e-04f,  3.62729177e-04f,  3.41921136e-04f,  3.18489958e-04f, 
     2.92497406e-04f,  2.64266550e-04f,  2.33955571e-04f,  2.01809261e-04f,  1.68092145e-04f,  1.33141461e-04f, 
     9.71043460e-05f,  6.03452880e-05f,  2.31264055e-05f, -1.43105089e-05f, -5.15607083e-05f, -8.84833364e-05f, 
    -1.24679461e-04f, -1.59910519e-04f, -1.93952723e-04f, -2.26496145e-04f, -2.57307566e-04f, -2.86175538e-04f, 
    -3.12853472e-04f, -3.37140613e-04f, -3.58914997e-04f, -3.77932329e-04f, -3.94117065e-04f, -4.07317063e-04f, 
    -4.17422308e-04f, -4.24419479e-04f, -4.28161231e-04f, -4.28700484e-04f, -4.26016659e-04f, -4.20088126e-04f, 
    -4.11009185e-04f, -3.98835037e-04f, -3.83585114e-04f, -3.65493072e-04f, -3.44616197e-04f, -3.21064387e-04f, 
    -2.95119418e-04f, -2.66863117e-04f, -2.36549174e-04f, -2.04391686e-04f, -1.70585806e-04f, -1.35432614e-04f, 
    -9.91006984e-05f, -6.19152828e-05f, -2.41012311e-05f,  1.40621144e-05f,  5.22867497e-05f,  9.03199843e-05f, 
     1.27917614e-04f,  1.64740292e-04f,  2.00634478e-04f,  2.35261402e-04f,  2.68377430e-04f,  2.99818019e-04f, 
     3.29273634e-04f,  3.56562766e-04f,  3.81532332e-04f,  4.03948113e-04f,  4.23655375e-04f,  4.40488930e-04f, 
     4.54376777e-04f,  4.65137195e-04f,  4.72679704e-04f,  4.77014073e-04f,  4.77982201e-04f,  4.75625277e-04f, 
     4.69878507e-04f,  4.60802987e-04f,  4.48367418e-04f,  4.32641679e-04f,  4.13709630e-04f,  3.91634147e-04f, 
     3.66512902e-04f,  3.38481392e-04f,  3.07634938e-04f,  2.74189182e-04f,  2.38229594e-04f,  1.99985879e-04f, 
     1.59632210e-04f,  1.17351364e-04f,  7.33404728e-05f,  2.78844831e-05f, -1.89099461e-05f, -6.67343638e-05f, 
    -1.15367449e-04f, -1.64649983e-04f, -2.14224348e-04f, -2.64019844e-04f, -3.13654244e-04f, -3.62990333e-04f, 
    -4.11800705e-04f, -4.59821928e-04f, -5.06946486e-04f, -5.52847863e-04f, -5.97397068e-04f, -6.40454770e-04f, 
    -6.81765968e-04f, -7.21210131e-04f, -7.58634477e-04f, -7.93939572e-04f, -8.26964876e-04f, -8.57585335e-04f, 
    -8.85733438e-04f, -9.11351007e-04f, -9.34300512e-04f, -9.54617442e-04f, -9.72159416e-04f, -9.87012089e-04f, 
    -9.99095133e-04f, -1.00846242e-03f, -1.01506022e-03f, -1.01897105e-03f, -1.02021427e-03f, -1.01887259e-03f, 
    -1.01497557e-03f, -1.00861358e-03f, -9.99877741e-04f, -9.88823136e-04f, -9.75617693e-04f, -9.60303769e-04f, 
    -9.43035535e-04f, -9.23922797e-04f, -9.03105429e-04f, -8.80708716e-04f, -8.56853281e-04f, -8.31685264e-04f, 
    -8.05348207e-04f, -7.77961627e-04f, -7.49713086e-04f, -7.20674604e-04f, -6.91032783e-04f, -6.60888020e-04f, 
    -6.30372917e-04f, -5.99673349e-04f, -5.68830563e-04f, -5.38013304e-04f, -5.07353303e-04f, -4.76915043e-04f, 
    -4.46832926e-04f, -4.17179291e-04f, -3.88083307e-04f, -3.59575024e-04f, -3.31820735e-04f, -3.04804303e-04f, 
    -2.78616041e-04f, -2.53335964e-04f, -2.28986996e-04f, -2.05619529e-04f, -1.83318449e-04f, -1.61979425e-04f, 
    -1.41791423e-04f, -1.22648816e-04f, -1.04625498e-04f, -8.77122910e-05f, -7.18653457e-05f, -5.71787106e-05f, 
    -4.34807639e-05f, -3.09618857e-05f, -1.94074401e-05f, -8.88017971e-06f,  6.09625220e-07f,  9.14020334e-06f, 
     1.67805558e-05f,  2.35369965e-05f,  2.94278194e-05f,  3.45049751e-05f,  3.88373828e-05f,  4.24291966e-05f, 
     4.53445665e-05f,  4.76965834e-05f,  4.93395567e-05f,  5.05392111e-05f,  5.12257065e-05f,  5.14579340e-05f, 
     5.12651750e-05f,  5.07312551e-05f,  4.98486765e-05f,  4.87082573e-05f,  4.73439631e-05f,  4.56740817e-05f, 
     4.38653618e-05f,  4.19399075e-05f,  3.99125668e-05f,  3.77616021e-05f,  3.56135997e-05f,  3.33554815e-05f, 
     3.11656899e-05f,  2.89038150e-05f,  2.67281634e-05f,  2.46192762e-05f,  2.24899205e-05f,  2.04698700e-05f, 
     1.84927655e-05f,  1.66762886e-05f,  1.49393771e-05f,  1.32258081e-05f,  1.16985586e-05f,  1.01874391e-05f, 
     8.99882100e-06f,  7.61267073e-06f,  6.57702907e-06f,  5.59829210e-06f,  4.27698546e-06f,  1.03248674e-05f, 
};
