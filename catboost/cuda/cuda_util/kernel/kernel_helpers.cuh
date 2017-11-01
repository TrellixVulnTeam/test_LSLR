#pragma once
#include <contrib/libs/cub/cub/thread/thread_load.cuh>
#include <contrib/libs/cub/cub/thread/thread_store.cuh>

namespace NKernel {

    template <class T>
    __device__ __forceinline__ bool ExtractSignBit(T val)  {
        static_assert(sizeof(T) == sizeof(ui32), "Error: this works only for 4byte types");
        return (*reinterpret_cast<ui32*>(&val)) >> 31;
    }

    template <class T>
    __device__ __forceinline__ T OrSignBit(T val, bool flag)  {
        static_assert(sizeof(T) == sizeof(ui32), "Error: this works only for 4byte types");
        ui32 raw = (*reinterpret_cast<ui32*>(&val) | (flag << 31));
        return *reinterpret_cast<T*>(&raw);
    }

    __device__ __forceinline__  float PositiveInfty()
    {
        return __int_as_float(0x7f800000);
    }

    __device__ __forceinline__   float NegativeInfty()
    {
        return -PositiveInfty();
    }

    template <class T>
    __forceinline__ __device__ T WarpReduce(int x, volatile T* data, int reduceSize) {
        #if __CUDA_ARCH__ >= 350
        T val = data[x];
        #pragma unroll
        for (int s = reduceSize >> 1; s > 0; s >>= 1)
        {
            val +=  __shfl_down(val, s);
        }
        if (x == 0) {
            data[x] = val;
        }
        return val;
        #else
        //unsafe optimization
        #pragma unroll
        for (int s = reduceSize >> 1; s > 0; s >>= 1)
        {
            if (x < s)
            {
                data[x] += data[x + s];
            }
        }
        return data[x];
        #endif
    }

    template <typename T, int BLOCK_SIZE>
    __forceinline__ __device__ T Reduce(volatile T* data) {
        const int x = threadIdx.x;

        #pragma  unroll
        for (int s = BLOCK_SIZE >> 1; s > 0; s >>= 1) {
            if (x < s)
            {
                data[x] += data[x + s];
            }
            __syncthreads();
        }
        T result = data[0];
        __syncthreads();
        return result;
    }

    template <class T>
    __forceinline__ __device__ T FastInBlockReduce(int x, volatile T* data, int reduceSize) {
        if (reduceSize > 32) {
            #pragma  unroll
            for (int s = reduceSize >> 1; s >= 32; s >>= 1) {
                if (x < s)
                {
                    data[x] += data[x + s];
                }
                __syncthreads();
            }
        }
        if (x < 32)
        {
            return WarpReduce(x, data, min(reduceSize, 32));
        } else {
            return 0;
        }
    }

    template <typename T>
    __forceinline__ __device__ T LdgWithFallback(const T* data, ui64 offset) {
        return cub::ThreadLoad<cub::LOAD_LDG>(data + offset);
    }


    template <typename T>
    __forceinline__ __device__ T StreamLoad(const T* data) {
        return cub::ThreadLoad<cub::LOAD_CS>(data);
    }

    template <typename T>
    __forceinline__ __device__ void WriteThrough(T* data, T val) {
        cub::ThreadStore<cub::STORE_WT>(data, val);
    }

    template <class U, class V>
    struct TPair {
        U First;
        V Second;

        __host__ __device__ __forceinline__ TPair() = default;

        __host__ __device__ __forceinline__ TPair(const U& first, const V& second)
                : First(first)
                , Second(second) {

        }


    };

}