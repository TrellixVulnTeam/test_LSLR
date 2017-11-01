#include <catboost/cuda/cuda_lib/cuda_buffer.h>
#include <catboost/cuda/cuda_lib/cuda_profiler.h>
#include <catboost/cuda/cuda_util/segmented_scan.h>
#include <catboost/cuda/cuda_util/cpu_random.h>
#include <library/unittest/registar.h>
#include <iostream>
#include <catboost/cuda/cuda_util/scan.h>
#include <catboost/cuda/cuda_util/helpers.h>

using namespace NCudaLib;

SIMPLE_UNIT_TEST_SUITE(TSegmentedScanTest) {
    SIMPLE_UNIT_TEST(TestSegmentedScan) {
        StartCudaManager();
        {
            ui64 tries = 10;
            TRandom rand(0);
            for (ui32 k = 0; k < tries; ++k) {
                ui64 size = 1001 + (rand.NextUniformL() % 1000000);

                auto mapping = TSingleMapping(0, size);

                auto input = TSingleBuffer<ui32>::Create(mapping);
                auto output = TSingleBuffer<ui32>::CopyMapping(input);
                auto flags = TSingleBuffer<ui32>::CopyMapping(input);

                yvector<ui32> data(size);
                yvector<ui32> flagsCpu(size);

                std::generate(data.begin(), data.end(), [&]() {
                    return rand.NextUniformL() % 10000;
                });

                const double p = 1000.0 / size;
                std::generate(flagsCpu.begin(), flagsCpu.end(), [&]() {
                    return rand.NextUniform() < p ? 1 : 0;
                });
                flagsCpu[0] = 1;

                input.Write(data);
                flags.Write(flagsCpu);

                SegmentedScanVector(input, flags, output);
                yvector<ui32> result;
                output.Read(result);

                ui32 prefixSum = 0;
                for (ui32 i = 0; i < result.size(); ++i) {
                    if (flagsCpu[i]) {
                        prefixSum = 0;
                    }

                    if (result[i] != prefixSum && i > 0) {
                        MATRIXNET_INFO_LOG << flagsCpu[i - 1] << " " << i - 1 << " " << result[i - 1] << " " << data[i - 1] << Endl;
                        MATRIXNET_INFO_LOG << flagsCpu[i] << " " << i << " " << result[i] << " " << prefixSum << Endl;
                    }
                    UNIT_ASSERT_EQUAL(result[i], prefixSum);
                    prefixSum += data[i];
                }

                SegmentedScanVector(input, flags, output, true);
                output.Read(result);

                prefixSum = data[0];

                for (ui32 i = 0; i < result.size(); ++i) {
                    if (flagsCpu[i]) {
                        prefixSum = 0;
                    }
                    prefixSum += data[i];
                    UNIT_ASSERT_EQUAL(result[i], prefixSum);
                }
            }
        }
        StopCudaManager();
    }

    SIMPLE_UNIT_TEST(TestSegmentedScanWithMask) {
        StartCudaManager();
        {
            ui64 tries = 10;
            TRandom rand(0);
            for (ui32 k = 0; k < tries; ++k) {
                ui64 size = 1001 + (rand.NextUniformL() % 1000000);

                auto mapping = TSingleMapping(0, size);

                auto input = TSingleBuffer<ui32>::Create(mapping);
                auto output = TSingleBuffer<ui32>::CopyMapping(input);
                auto flags = TSingleBuffer<ui32>::CopyMapping(input);

                yvector<ui32> data(size);
                yvector<ui32> flagsCpu(size);

                std::generate(data.begin(), data.end(), [&]() {
                    return rand.NextUniformL() % 10000;
                });

                const double p = 1000.0 / size;
                std::generate(flagsCpu.begin(), flagsCpu.end(), [&]() {
                    return (rand.NextUniform() < p ? 1 : 0) << 31;
                });
                flagsCpu[0] = 1 << 31;

                input.Write(data);
                flags.Write(flagsCpu);

                SegmentedScanVector(input, flags, output, false, 1 << 31);
                yvector<ui32> result;
                output.Read(result);

                ui32 prefixSum = 0;
                for (ui32 i = 0; i < result.size(); ++i) {
                    if (flagsCpu[i] >> 31) {
                        prefixSum = 0;
                    }

                    if (result[i] != prefixSum) {
                        MATRIXNET_INFO_LOG << flagsCpu[i - 1] << " " << i - 1 << " " << result[i - 1] << " " << data[i - 1] << Endl;
                        MATRIXNET_INFO_LOG << flagsCpu[i] << " " << i << " " << result[i] << " " << prefixSum << Endl;
                    }
                    UNIT_ASSERT_EQUAL(result[i], prefixSum);
                    prefixSum += data[i];
                }

                SegmentedScanVector(input, flags, output, true, 1 << 31);
                output.Read(result);

                prefixSum = 0;

                for (ui32 i = 0; i < result.size(); ++i) {
                    if (flagsCpu[i] >> 31) {
                        prefixSum = 0;
                    }
                    prefixSum += data[i];
                    UNIT_ASSERT_EQUAL(result[i], prefixSum);
                }
            }
        }
        StopCudaManager();
    }

    SIMPLE_UNIT_TEST(TestNonNegativeSegmentedScan) {
        StartCudaManager();
        {
            ui64 tries = 10;
            TRandom rand(0);
            for (ui32 k = 0; k < tries; ++k) {
                ui64 size = 1001 + (rand.NextUniformL() % 1000000);

                auto mapping = TSingleMapping(0, size);

                auto input = TSingleBuffer<float>::Create(mapping);
                auto output = TSingleBuffer<float>::CopyMapping(input);

                yvector<float> data(size);
                yvector<ui32> flagsCpu(size);

                std::generate(data.begin(), data.end(), [&]() {
                    return 1.0 / (1 << (rand.NextUniformL() % 10));
                });

                const double p = 1000.0 / size;
                std::generate(flagsCpu.begin(), flagsCpu.end(), [&]() {
                    return (rand.NextUniform() < p ? 1 : 0);
                });
                flagsCpu[0] = 1;

                for (ui32 i = 0; i < flagsCpu.size(); ++i) {
                    if (flagsCpu[i]) {
                        data[i] = -data[i];
                    }
                }

                input.Write(data);
                yvector<float> result;
                //
                float prefixSum = 0;

                InclusiveSegmentedScanNonNegativeVector(input, output);
                output.Read(result);

                prefixSum = 0;

                for (ui32 i = 0; i < result.size(); ++i) {
                    if (flagsCpu[i]) {
                        prefixSum = 0;
                    }
                    prefixSum += std::abs(data[i]);
                    UNIT_ASSERT_DOUBLES_EQUAL(std::abs(result[i]), prefixSum, 1e-9);
                }
            }
        }
        StopCudaManager();
    }

    SIMPLE_UNIT_TEST(TestNonNegativeSegmentedScanAndScatter) {
        StartCudaManager();
        {
            ui64 tries = 10;
            TRandom rand(0);

            for (ui32 k = 0; k < tries; ++k) {
                ui64 size = 1001 + (rand.NextUniformL() % 1000000);

                auto mapping = TSingleMapping(0, size);

                auto input = TSingleBuffer<float>::Create(mapping);
                auto output = TSingleBuffer<float>::CopyMapping(input);

                yvector<float> data(size);
                yvector<ui32> indicesCpu(size);

                std::generate(data.begin(), data.end(), [&]() {
                    return 1.0 / (1 << (rand.NextUniformL() % 10));
                });

                const double p = 1000.0 / size;
                for (ui32 i = 0; i < indicesCpu.size(); ++i) {
                    indicesCpu[i] = i;
                };
                std::random_shuffle(indicesCpu.begin(), indicesCpu.end(), rand);
                for (ui32 i = 0; i < indicesCpu.size(); ++i) {
                    indicesCpu[i] |= ((rand.NextUniform() < p ? 1 : 0) << 31);
                }
                indicesCpu[0] |= 1 << 31;

                for (ui32 i = 0; i < indicesCpu.size(); ++i) {
                    if (indicesCpu[i] >> 31) {
                        data[i] = -data[i];
                    }
                }
                auto indices = TSingleBuffer<ui32>::Create(mapping);
                indices.Write(indicesCpu);
                input.Write(data);

                yvector<float> result;

                SegmentedScanAndScatterNonNegativeVector(input, indices, output, false);
                output.Read(result);
                //
                float prefixSum = 0;
                const ui32 mask = 0x3FFFFFFF;
                for (ui32 i = 0; i < result.size(); ++i) {
                    if (indicesCpu[i] >> 31) {
                        prefixSum = 0;
                    }

                    const ui32 scatterIndex = indicesCpu[i] & mask;
                    UNIT_ASSERT_EQUAL(result[scatterIndex], prefixSum);
                    prefixSum += std::abs(data[i]);
                }

                prefixSum = 0;

                SegmentedScanAndScatterNonNegativeVector(input, indices, output, true);
                output.Read(result);

                for (ui32 i = 0; i < result.size(); ++i) {
                    if (indicesCpu[i] >> 31) {
                        prefixSum = 0;
                    }
                    prefixSum += std::abs(data[i]);
                    const ui32 scatterIndex = indicesCpu[i] & mask;
                    const float val = result[scatterIndex];
                    if (std::abs(val - prefixSum) > 1e-9) {
                        MATRIXNET_INFO_LOG << scatterIndex << " " << std::abs(val) << " " << prefixSum << Endl;
                        MATRIXNET_INFO_LOG << indicesCpu[i - 1] << " " << i - 1 << " " << result[i - 1] << " " << data[i - 1] << Endl;
                        MATRIXNET_INFO_LOG << indicesCpu[i] << " " << i << " " << result[i] << " " << prefixSum << Endl;
                    }
                    UNIT_ASSERT_EQUAL(val, prefixSum);
                }
            }
        }
        StopCudaManager();
    }

    inline void RunSegmentedScanNonNegativePerformanceTest() {
        {
            auto& profiler = NCudaLib::GetCudaManager().GetProfiler();
            SetDefaultProfileMode(EProfileMode::ImplicitLabelSync);

            ui64 tries = 20;
            TRandom rand(0);
            for (ui32 i = 10000; i < 10000001; i *= 10) {
                const ui32 size = i;
                auto mapping = TSingleMapping(0, size);

                auto input = TSingleBuffer<float>::Create(mapping);
                auto output = TSingleBuffer<float>::CopyMapping(input);

                yvector<float> data(size);
                yvector<ui32> flagsCpu(size);

                std::generate(data.begin(), data.end(), [&]() {
                    return rand.NextUniformL() % 10000;
                });

                const double p = 5000.0 / size;
                std::generate(flagsCpu.begin(), flagsCpu.end(), [&]() {
                    return rand.NextUniform() < p ? 1 : 0;
                });
                flagsCpu[0] = 1;

                for (ui32 j = 0; j < flagsCpu.size(); ++j) {
                    if (flagsCpu[j]) {
                        data[j] = -data[j];
                    }
                }
                input.Write(data);

                for (ui32 k = 0; k < tries; ++k) {
                    {
                        auto guard = profiler.Profile(TStringBuilder() << "Inclusive segmented scan of #" << i << " elements");
                        InclusiveSegmentedScanNonNegativeVector(input, output);
                    }
                }
            }
        }
    }

    //
    template <class T>
    inline void TestSegmentedScanPerformance() {
        {
            auto& profiler = NCudaLib::GetCudaManager().GetProfiler();
            SetDefaultProfileMode(EProfileMode::ImplicitLabelSync);

            ui64 tries = 10;
            TRandom rand(0);
            for (ui32 i = 10000; i < 10000001; i *= 10) {
                const ui32 size = i;
                auto mapping = TSingleMapping(0, size);

                auto input = TSingleBuffer<ui32>::Create(mapping);
                auto output = TSingleBuffer<ui32>::CopyMapping(input);
                auto flags = TSingleBuffer<ui32>::CopyMapping(input);

                yvector<ui32> data(size);
                yvector<ui32> flagsCpu(size);

                std::generate(data.begin(), data.end(), [&]() {
                    return rand.NextUniformL() % 10000;
                });

                const double p = 5000.0 / size;
                std::generate(flagsCpu.begin(), flagsCpu.end(), [&]() {
                    return rand.NextUniform() < p ? 1 : 0;
                });
                flagsCpu[0] = 1;

                input.Write(data);
                flags.Write(flagsCpu);

                for (ui32 k = 0; k < tries; ++k) {
                    {
                        auto guard = profiler.Profile(TStringBuilder() << "Scan of #" << i << " elements");
                        SegmentedScanVector(input, flags, output);
                    }
                }
            }
        }
    }

    SIMPLE_UNIT_TEST(TestSegmentedScanPerformanceFloat) {
        StartCudaManager();
        {
            TestSegmentedScanPerformance<float>();
        }
        StopCudaManager();
    }

    SIMPLE_UNIT_TEST(TestSegmentedScanPerformanceInt) {
        StartCudaManager();
        {
            TestSegmentedScanPerformance<int>();
        }
        StopCudaManager();
    }

    SIMPLE_UNIT_TEST(TestSegmentedScanPerformanceUnsignedInt) {
        StartCudaManager();
        {
            TestSegmentedScanPerformance<ui32>();
        }
        StopCudaManager();
    }

    SIMPLE_UNIT_TEST(TestSegmentedScanNonNegativePerformance) {
        StartCudaManager();
        {
            RunSegmentedScanNonNegativePerformanceTest();
        }
        StopCudaManager();
    }
}
