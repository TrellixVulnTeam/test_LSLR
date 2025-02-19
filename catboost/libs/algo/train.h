#pragma once

#include "learn_context.h"
#include "target_classifier.h"
#include "full_features.h"
#include "online_predictor.h"
#include "bin_tracker.h"
#include "rand_score.h"
#include "fold.h"
#include "projection.h"
#include "online_ctr.h"
#include "score_calcer.h"
#include "approx_calcer.h"
#include "index_hash_calcer.h"
#include "greedy_tensor_search.h"
#include "logger.h"
#include "error_functions.h"

#include <catboost/libs/params/params.h>
#include <catboost/libs/metrics/metric.h>
#include <catboost/libs/helpers/interrupt.h>
#include <catboost/libs/model/hash.h>
#include <catboost/libs/model/model.h>

#include <catboost/libs/overfitting_detector/error_tracker.h>
#include <catboost/libs/logging/profile_info.h>

#include <library/fast_exp/fast_exp.h>
#include <library/fast_log/fast_log.h>

#include <util/string/vector.h>
#include <util/string/iterator.h>
#include <util/stream/file.h>
#include <util/generic/ymath.h>
#include <util/random/shuffle.h>
#include <util/random/normal.h>

void ShrinkModel(int itCount, TLearnProgress* progress);

TVector<int> CountSplits(const TVector<TFloatFeature>& floatFeatures);

TErrorTracker BuildErrorTracker(bool isMaxOptimal, bool hasTest, TLearnContext* ctx);

void NormalizeLeafValues(const TVector<TIndexType>& indices, int learnSampleCount, TVector<TVector<double>>* treeValues);

template <typename TError>
TError BuildError(const TFitParams& params) {
    return TError(params.StoreExpApprox);
}

template <>
inline TQuantileError BuildError<TQuantileError>(const TFitParams& params) {
    auto lossParams = GetLossParams(params.Objective);
    if (lossParams.empty()) {
        return TQuantileError(params.StoreExpApprox);
    } else {
        CB_ENSURE(lossParams.begin()->first == "alpha", "Invalid loss description" << params.Objective);
        return TQuantileError(lossParams["alpha"], params.StoreExpApprox);
    }
}

template <>
inline TLogLinQuantileError BuildError<TLogLinQuantileError>(const TFitParams& params) {
    auto lossParams = GetLossParams(params.Objective);
    if (lossParams.empty()) {
        return TLogLinQuantileError(params.StoreExpApprox);
    } else {
        CB_ENSURE(lossParams.begin()->first == "alpha", "Invalid loss description" << params.Objective);
        return TLogLinQuantileError(lossParams["alpha"], params.StoreExpApprox);
    }
}

template <>
inline TCustomError BuildError<TCustomError>(const TFitParams& params) {
    Y_ASSERT(params.ObjectiveDescriptor.Defined());
    return TCustomError(params);
}

using TTrainFunc = std::function<void(const TTrainData& data,
                                      TLearnContext* ctx,
                                      TVector<TVector<double>>* testMultiApprox)>;

using TTrainOneIterationFunc = std::function<void(const TTrainData& data,
                                                  TLearnContext* ctx)>;

template <typename TError>
inline void CalcWeightedDerivatives(const TVector<TVector<double>>& approx,
                                    const TVector<float>& target,
                                    const TVector<float>& weight,
                                    const TVector<ui32>& queriesId,
                                    const yhash<ui32, ui32>& queriesSize,
                                    const TVector<TVector<TCompetitor>>& competitors,
                                    const TError& error,
                                    int tailFinish,
                                    TLearnContext* ctx,
                                    TVector<TVector<double>>* derivatives) {
    if (error.GetErrorType() == EErrorType::QuerywiseError) {
        TVector<TDer1Der2> ders((*derivatives)[0].ysize());
        error.CalcDersForQueries(0, tailFinish, approx[0], target, weight, queriesId, queriesSize, &ders);
        for (int docId = 0; docId < ders.ysize(); ++docId) {
            (*derivatives)[0][docId] = ders[docId].Der1;
        }
    } else if (error.GetErrorType() == EErrorType::PairwiseError) {
        error.CalcDersPairs(approx[0], competitors, 0, tailFinish, &(*derivatives)[0]);
    } else {
        int approxDimension = approx.ysize();
        NPar::TLocalExecutor::TBlockParams blockParams(0, tailFinish);
        blockParams.SetBlockSize(1000);

        Y_ASSERT(error.GetErrorType() == EErrorType::PerObjectError);
        if (approxDimension == 1) {
            ctx->LocalExecutor.ExecRange([&](int blockId) {
                const int blockOffset = blockId * blockParams.GetBlockSize();
                error.CalcFirstDerRange(blockOffset, Min<int>(blockParams.GetBlockSize(), tailFinish - blockOffset),
                    approx[0].data(),
                    nullptr, // no approx deltas
                    target.data(),
                    weight.data(),
                    (*derivatives)[0].data());
            }, 0, blockParams.GetBlockCount(), NPar::TLocalExecutor::WAIT_COMPLETE);
        } else {
            ctx->LocalExecutor.ExecRange([&](int blockId) {
                TVector<double> curApprox(approxDimension);
                TVector<double> curDelta(approxDimension);
                NPar::TLocalExecutor::BlockedLoopBody(blockParams, [&](int z) {
                    for (int dim = 0; dim < approxDimension; ++dim) {
                        curApprox[dim] = approx[dim][z];
                    }
                    error.CalcDersMulti(curApprox, target[z], weight.empty() ? 1 : weight[z], &curDelta, nullptr);
                    for (int dim = 0; dim < approxDimension; ++dim) {
                        (*derivatives)[dim][z] = curDelta[dim];
                    }
                })(blockId);
            }, 0, blockParams.GetBlockCount(), NPar::TLocalExecutor::WAIT_COMPLETE);
        }
    }
}

inline void CalcAndLogLearnErrors(const TVector<TVector<double>>& avrgApprox,
                                  const TVector<float>& target,
                                  const TVector<float>& weight,
                                  const TVector<ui32>& queryId,
                                  const yhash<ui32, ui32>& queriesSize,
                                  const TVector<TPair>& pairs,
                                  const TVector<THolder<IMetric>>& errors,
                                  int learnSampleCount,
                                  int iteration,
                                  TVector<TVector<double>>* learnErrorsHistory,
                                  NPar::TLocalExecutor* localExecutor,
                                  TLogger* logger) {
    learnErrorsHistory->emplace_back();
    for (int i = 0; i < errors.ysize(); ++i) {
        double learnErr = errors[i]->GetFinalError(
            errors[i]->GetErrorType() == EErrorType::PerObjectError ?
                errors[i]->Eval(avrgApprox, target, weight, 0, learnSampleCount, *localExecutor) :
                errors[i]->GetErrorType() == EErrorType::PairwiseError ?
                    errors[i]->EvalPairwise(avrgApprox, pairs, 0, learnSampleCount):
                    errors[i]->EvalQuerywise(avrgApprox, target, weight, queryId, queriesSize, 0, learnSampleCount));
        if (i == 0) {
            MATRIXNET_NOTICE_LOG << "learn: " << Prec(learnErr, 7);
        }
        learnErrorsHistory->back().push_back(learnErr);
    }

    if (logger != nullptr) {
        Log(iteration, learnErrorsHistory->back(), errors, logger, EPhase::Learn);
    }
}

inline void CalcAndLogTestErrors(const TVector<TVector<double>>& avrgApprox,
                                 const TVector<float>& target,
                                 const TVector<float>& weight,
                                 const TVector<ui32>& queryId,
                                 const yhash<ui32, ui32>& queriesSize,
                                 const TVector<TPair>& pairs,
                                 const TVector<THolder<IMetric>>& errors,
                                 int learnSampleCount,
                                 int sampleCount,
                                 int iteration,
                                 TErrorTracker& errorTracker,
                                 TVector<TVector<double>>* testErrorsHistory,
                                 NPar::TLocalExecutor* localExecutor,
                                 TLogger* logger) {
    TVector<double> valuesToLog;

    testErrorsHistory->emplace_back();
    for (int i = 0; i < errors.ysize(); ++i) {
        double testErr = errors[i]->GetFinalError(
            errors[i]->GetErrorType() == EErrorType::PerObjectError ?
                errors[i]->Eval(avrgApprox, target, weight, learnSampleCount, sampleCount, *localExecutor) :
                errors[i]->GetErrorType() == EErrorType::PairwiseError ?
                    errors[i]->EvalPairwise(avrgApprox, pairs, learnSampleCount, sampleCount):
                    errors[i]->EvalQuerywise(avrgApprox, target, weight, queryId, queriesSize, learnSampleCount, sampleCount));

        if (i == 0) {
            errorTracker.AddError(testErr, iteration, &valuesToLog);
            double bestErr = errorTracker.GetBestError();

            MATRIXNET_NOTICE_LOG << "\ttest: " << Prec(testErr, 7) << "\tbestTest: " << Prec(bestErr, 7)
                                 << " (" << errorTracker.GetBestIteration() << ")";
        }
        testErrorsHistory->back().push_back(testErr);
    }

    if (logger != nullptr) {
        Log(iteration, testErrorsHistory->back(), errors, logger, EPhase::Test);
    }
}

template <typename TError>
void TrainOneIter(const TTrainData& data,
                  TLearnContext* ctx) {
    TError error = BuildError<TError>(ctx->Params);
    TProfileInfo& profile = ctx->Profile;

    const auto sampleCount = data.GetSampleCount();
    const int approxDimension = ctx->LearnProgress.AvrgApprox.ysize();

    TVector<THolder<IMetric>> errors = CreateMetrics(ctx->Params.EvalMetric, ctx->Params.EvalMetricDescriptor, ctx->Params.CustomLoss, approxDimension);

    auto splitCounts = CountSplits(ctx->LearnProgress.FloatFeatures);

    const int foldCount = ctx->LearnProgress.Folds.ysize();
    const int currentIteration = ctx->LearnProgress.TreeStruct.ysize();

    MATRIXNET_NOTICE_LOG << currentIteration << ": ";
    profile.StartNextIteration();

    CheckInterrupted(); // check after long-lasting operation

    double modelLength = currentIteration * ctx->Params.LearningRate;

    TSplitTree bestSplitTree;
    {
        TFold* takenFold = &ctx->LearnProgress.Folds[ctx->Rand.GenRand() % foldCount];

        ctx->LocalExecutor.ExecRange([&](int bodyTailId) {
            TFold::TBodyTail& bt = takenFold->BodyTailArr[bodyTailId];
            CalcWeightedDerivatives<TError>(bt.Approx,
                                            takenFold->LearnTarget,
                                            takenFold->LearnWeights,
                                            takenFold->LearnQueryId,
                                            takenFold->LearnQuerySize,
                                            bt.Competitors,
                                            error,
                                            bt.TailFinish,
                                            ctx,
                                            &bt.Derivatives);
        }, 0, takenFold->BodyTailArr.ysize(), NPar::TLocalExecutor::WAIT_COMPLETE);
        profile.AddOperation("Calc derivatives");

        GreedyTensorSearch(
            data,
            splitCounts,
            modelLength,
            profile,
            takenFold,
            ctx,
            &bestSplitTree);
    }
    CheckInterrupted(); // check after long-lasting operation
    {
        TVector<TFold*> trainFolds;
        for (int foldId = 0; foldId < foldCount; ++foldId) {
            trainFolds.push_back(&ctx->LearnProgress.Folds[foldId]);
        }

        TrimOnlineCTRcache(trainFolds);
        TrimOnlineCTRcache({&ctx->LearnProgress.AveragingFold});
        {
            TVector<TFold*> allFolds = trainFolds;
            allFolds.push_back(&ctx->LearnProgress.AveragingFold);

            TVector<TCalcOnlineCTRsBatchTask> parallelJobsData;
            for (const auto& split : bestSplitTree.Splits) {
                if (split.Type != ESplitType::OnlineCtr) {
                    continue;
                }

                auto& proj = split.Ctr.Projection;
                for (auto* foldPtr : allFolds) {
                    if (!foldPtr->GetCtrs(proj).has(proj)) {
                        parallelJobsData.emplace_back(TCalcOnlineCTRsBatchTask{proj, foldPtr, &foldPtr->GetCtrRef(proj)});
                    }
                }
            }
            CalcOnlineCTRsBatch(parallelJobsData, data, ctx);
        }
        profile.AddOperation("ComputeOnlineCTRs for tree struct (train folds and test fold)");
        CheckInterrupted(); // check after long-lasting operation

        TVector<TVector<TVector<TVector<double>>>> approxDelta;
        CalcApproxForLeafStruct(
            data,
            error,
            trainFolds,
            bestSplitTree,
            ctx,
            &approxDelta);
        profile.AddOperation("CalcApprox tree struct");
        CheckInterrupted(); // check after long-lasting operation

        const double learningRate = ctx->Params.LearningRate;

        for (int foldId = 0; foldId < foldCount; ++foldId) {
            TFold& ff = ctx->LearnProgress.Folds[foldId];

            for (int bodyTailId = 0; bodyTailId < ff.BodyTailArr.ysize(); ++bodyTailId) {
                TFold::TBodyTail& bt = ff.BodyTailArr[bodyTailId];
                for (int dim = 0; dim < approxDimension; ++dim) {
                    double* approxDeltaData = approxDelta[foldId][bodyTailId][dim].data();
                    double* approxData = bt.Approx[dim].data();
                    ctx->LocalExecutor.ExecRange([=](int z) {
                        approxData[z] = UpdateApprox<TError::StoreExpApprox>(approxData[z], ApplyLearningRate<TError::StoreExpApprox>(approxDeltaData[z], learningRate));
                    }, NPar::TLocalExecutor::TBlockParams(0, bt.TailFinish).SetBlockSize(10000)
                     , NPar::TLocalExecutor::WAIT_COMPLETE);
                }
            }
        }
        profile.AddOperation("Update tree structure approx");
        CheckInterrupted(); // check after long-lasting operation

        TVector<TIndexType> indices;
        TVector<TVector<double>> treeValues;
        CalcLeafValues(data,
                       bestSplitTree,
                       error,
                       ctx,
                       &treeValues,
                       &indices);

        // TODO(nikitxskv): if this will be a bottleneck, we can use precalculated counts.
        if (IsPairwiseError(ctx->Params.LossFunction)) {
            NormalizeLeafValues(indices, data.LearnSampleCount, &treeValues);
        }

        TVector<TVector<double>> expTreeValues;
        expTreeValues.yresize(approxDimension);
        for (int dim = 0; dim < approxDimension; ++dim) {
            for (auto& leafVal : treeValues[dim]) {
                leafVal *= learningRate;
            }
            expTreeValues[dim] = treeValues[dim];
            ExpApproxIf(TError::StoreExpApprox, &expTreeValues[dim]);
        }

        profile.AddOperation("CalcApprox result leafs");
        CheckInterrupted(); // check after long-lasting operation

        Y_ASSERT(ctx->LearnProgress.AveragingFold.BodyTailArr.ysize() == 1);
        TFold::TBodyTail& bt = ctx->LearnProgress.AveragingFold.BodyTailArr[0];

        const int tailFinish = bt.TailFinish;
        const int learnSampleCount = data.LearnSampleCount;
        const int* learnPermutationData = ctx->LearnProgress.AveragingFold.LearnPermutation.data();
        const TIndexType* indicesData = indices.data();
        for (int dim = 0; dim < approxDimension; ++dim) {
            const double* expTreeValuesData = expTreeValues[dim].data();
            const double* treeValuesData = treeValues[dim].data();
            double* approxData = bt.Approx[dim].data();
            double* avrgApproxData = ctx->LearnProgress.AvrgApprox[dim].data();
            ctx->LocalExecutor.ExecRange([=](int docIdx) {
                const int permutedDocIdx = docIdx < learnSampleCount ? learnPermutationData[docIdx] : docIdx;
                if (docIdx < tailFinish) {
                    approxData[docIdx] = UpdateApprox<TError::StoreExpApprox>(approxData[docIdx], expTreeValuesData[indicesData[docIdx]]);
                }
                avrgApproxData[permutedDocIdx] += treeValuesData[indicesData[docIdx]];
            }, NPar::TLocalExecutor::TBlockParams(0, sampleCount).SetBlockSize(10000)
             , NPar::TLocalExecutor::WAIT_COMPLETE);
        }

        ctx->LearnProgress.LeafValues.push_back(treeValues);
        ctx->LearnProgress.TreeStruct.push_back(bestSplitTree);

        profile.AddOperation("Update final approxes");
        CheckInterrupted(); // check after long-lasting operation
    }
}

template <typename TError>
void Train(const TTrainData& data, TLearnContext* ctx, TVector<TVector<double>>* testMultiApprox) {
    TProfileInfo& profile = ctx->Profile;

    const int sampleCount = data.GetSampleCount();
    const int approxDimension = testMultiApprox->ysize();
    const bool hasTest = sampleCount > data.LearnSampleCount;

    TVector<THolder<IMetric>> metrics = CreateMetrics(ctx->Params.EvalMetric, ctx->Params.EvalMetricDescriptor, ctx->Params.CustomLoss, approxDimension);

    // TODO(asaitgalin): Should we have multiple error trackers?
    TErrorTracker errorTracker = BuildErrorTracker(metrics.front()->IsMaxOptimal(), hasTest, ctx);

    CB_ENSURE(hasTest || !ctx->Params.UseBestModel, "cannot select best model, no test provided");

    THolder<TLogger> logger;

    if (ctx->TryLoadProgress()) {
        for (int dim = 0; dim < approxDimension; ++dim) {
            (*testMultiApprox)[dim].assign(
                    ctx->LearnProgress.AvrgApprox[dim].begin() + data.LearnSampleCount, ctx->LearnProgress.AvrgApprox[dim].end());
        }
    }
    if (ctx->Params.AllowWritingFiles) {
        logger = CreateLogger(metrics, *ctx, hasTest);
    }
    TVector<TVector<double>> errorsHistory = ctx->LearnProgress.TestErrorsHistory;
    TVector<double> valuesToLog;
    for (int i = 0; i < errorsHistory.ysize(); ++i) {
        errorTracker.AddError(errorsHistory[i][0], i, &valuesToLog);
    }

    TVector<TFold*> folds;
    for (auto& fold : ctx->LearnProgress.Folds) {
        folds.push_back(&fold);
    }

    if (AreStatsFromPrevTreeUsed(ctx->Params)) {
        ctx->ParamsUsedWithStatsFromPrevTree.Create(*folds[0]); // assume that all folds have the same shape
        const int approxDimension = folds[0]->GetApproxDimension();
        const int bodyTailCount = folds[0]->BodyTailArr.ysize();
        ctx->StatsFromPrevTree.Create(CountNonCtrBuckets(CountSplits(ctx->LearnProgress.FloatFeatures), data.AllFeatures.OneHotValues), ctx->Params.Depth, approxDimension, bodyTailCount);
    }

    for (int iter = ctx->LearnProgress.TreeStruct.ysize(); iter < ctx->Params.Iterations; ++iter) {
        TrainOneIter<TError>(data, ctx);

        CalcAndLogLearnErrors(
            ctx->LearnProgress.AvrgApprox,
            data.Target,
            data.Weights,
            data.QueryId,
            data.QuerySize,
            data.Pairs,
            metrics,
            data.LearnSampleCount,
            iter,
            &ctx->LearnProgress.LearnErrorsHistory,
            &ctx->LocalExecutor,
            logger.Get());

        profile.AddOperation("Calc learn errors");

        if (hasTest) {
            CalcAndLogTestErrors(
                ctx->LearnProgress.AvrgApprox,
                data.Target,
                data.Weights,
                data.QueryId,
                data.QuerySize,
                data.Pairs,
                metrics,
                data.LearnSampleCount,
                sampleCount,
                iter,
                errorTracker,
                &ctx->LearnProgress.TestErrorsHistory,
                &ctx->LocalExecutor,
                logger.Get());

            profile.AddOperation("Calc test errors");

            if (ctx->Params.UseBestModel && iter == errorTracker.GetBestIteration() || !ctx->Params.UseBestModel) {
                for (int dim = 0; dim < approxDimension; ++dim) {
                    (*testMultiApprox)[dim].assign(
                        ctx->LearnProgress.AvrgApprox[dim].begin() + data.LearnSampleCount, ctx->LearnProgress.AvrgApprox[dim].end());
                }
            }
        }

        profile.FinishIteration();
        ctx->SaveProgress();

        if (IsNan(ctx->LearnProgress.LearnErrorsHistory.back()[0])) {
            ctx->LearnProgress.LeafValues.pop_back();
            ctx->LearnProgress.TreeStruct.pop_back();
            MATRIXNET_WARNING_LOG << "Training has stopped (degenerate solution on iteration "
                                  << iter << ", probably too small l2-regularization, try to increase it)" << Endl;
            break;
        }

        if (errorTracker.GetIsNeedStop()) {
            MATRIXNET_NOTICE_LOG << "Stopped by overfitting detector "
                               << " (" << errorTracker.GetOverfittingDetectorIterationsWait() << " iterations wait)" << Endl;
            break;
        }
    }

    ctx->LearnProgress.Folds.clear();

    if (hasTest) {
        MATRIXNET_NOTICE_LOG << "\n";
        MATRIXNET_NOTICE_LOG << "bestTest = " << errorTracker.GetBestError() << "\n";
        MATRIXNET_NOTICE_LOG << "bestIteration = " << errorTracker.GetBestIteration() << "\n";
        MATRIXNET_NOTICE_LOG << "\n";
    }

    if (ctx->Params.DetailedProfile || ctx->Params.DeveloperMode) {
        profile.PrintAverages();
    }

    if (ctx->Params.UseBestModel && ctx->Params.Iterations > 0) {
        const int itCount = errorTracker.GetBestIteration() + 1;
        MATRIXNET_NOTICE_LOG << "Shrink model to first " << itCount << " iterations." << Endl;
        ShrinkModel(itCount, &ctx->LearnProgress);
    }
}
