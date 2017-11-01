#pragma once

#include <catboost/libs/overfitting_detector/overfitting_detector.h>
#include <catboost/cuda/targets/target_base.h>
#include <catboost/cuda/targets/mse.h>
#include <catboost/cuda/cuda_lib/cuda_profiler.h>
#include <catboost/cuda/models/additive_model.h>
#include <catboost/cuda/gpu_data/fold_based_dataset.h>
#include <catboost/cuda/gpu_data/fold_based_dataset_builder.h>
#include <catboost/cuda/targets/target_options.h>
#include <util/stream/format.h>

namespace NCatboostCuda
{
    template<class TTarget,
            class TWeakModel>
    class IBoostingListener
    {
    public:
        using TConstVec = typename TTarget::TConstVec;

        virtual ~IBoostingListener()
        = default;

        virtual void Init(const TAdditiveModel<TWeakModel>& newEnsemble,
                          const TTarget& target,
                          const TConstVec& point)
        {
            Y_UNUSED(newEnsemble);
            Y_UNUSED(target);
            Y_UNUSED(point);
        }

        virtual void UpdateEnsemble(const TAdditiveModel<TWeakModel>& newEnsemble,
                                    const TTarget& target,
                                    const TConstVec& point) = 0;

    };

    template<class TTarget, class TWeakModel>
    class TMetricLogger: public IBoostingListener<TTarget, TWeakModel>
    {
    public:
        using TConstVec = typename TTarget::TConstVec;
        using TTargetStat = typename TMetricHelper<TTarget>::TTargetStat;

        explicit TMetricLogger(const TString& messagePrefix,
                               TString outputPath = "",
                               TString noticeLogSuffix = "\t",
                               TString bestPrefix = "")
                : MessagePrefix(messagePrefix)
                  , OutputPath(outputPath)
                  , BestPrefix(bestPrefix)
                  , NoticeLogSuffix(std::move(noticeLogSuffix))
        {
            if (OutputPath) {
                Out.Reset(new TOFStream(outputPath));
                (*Out) << "iter\t" << TTarget::TargetName() << Endl;
            }
        }

        static TStringBuf GetMetricName() {
            return TTarget::TargetName();
        }

        static bool IsMinOptimal()
        {
            return TTarget::IsMinOptimal();
        }

        ui32 GetBestIteration() const
        {
            return BestEnsembleSize;
        }

        double GetBestScore() const {
            return TTarget::Score(BestStat);
        }

        void RegisterOdDetector(IOverfittingDetector* odDetector)
        {
            OdDetector = odDetector;
        }

        void UpdateEnsemble(const TAdditiveModel<TWeakModel>& newEnsemble,
                            const TTarget& target,
                            const TConstVec& point) override
        {
            Y_UNUSED(newEnsemble);
            TMetricHelper<TTarget> metricHelper(target);
            metricHelper.SetPoint(point);
            if (BestEnsembleSize == 0 || metricHelper.IsBetter(BestStat))
            {
                BestStat = metricHelper.GetStat();
                BestEnsembleSize = static_cast<ui32>(newEnsemble.Size());
            }

            MATRIXNET_NOTICE_LOG << MessagePrefix << metricHelper.Score();
            if (BestPrefix.Size()) {
                MATRIXNET_NOTICE_LOG << BestPrefix <<  metricHelper.Score(BestStat) << " (" << BestEnsembleSize << ")";
            }
            MATRIXNET_NOTICE_LOG << NoticeLogSuffix;

            if (Out)
            {
                (*Out) << newEnsemble.Size() << "\t" << metricHelper.Score() << Endl;
            }
            if (OdDetector)
            {
                OdDetector->AddError(metricHelper.Score());
            }
        }
    private:
        ui32 BestEnsembleSize = 0;
        TTargetStat BestStat;
        TString MessagePrefix;
        TString OutputPath;
        TString BestPrefix;
        TString NoticeLogSuffix;
        THolder<TOFStream> Out;
        IOverfittingDetector* OdDetector = nullptr;
    };

    template<class TTarget,
            class TWeakModel>
    class TIterationLogger: public IBoostingListener<TTarget, TWeakModel>
    {
    public:
       using TConstVec = typename TTarget::TConstVec;

        explicit TIterationLogger(TString suffix = ":\t")
                : Suffix(std::move(suffix)) {

        }

        void UpdateEnsemble(const TAdditiveModel<TWeakModel>& newEnsemble,
                            const TTarget& target,
                            const TConstVec& point) override
        {
            Y_UNUSED(newEnsemble);
            Y_UNUSED(target);
            Y_UNUSED(point);
            MATRIXNET_NOTICE_LOG << newEnsemble.Size() - 1 << Suffix;
        }
    private:
        TString Suffix;
    };

    template<class TTarget,
            class TWeakModel>
    class TTimeWriter: public IBoostingListener<TTarget, TWeakModel>
    {
    public:
        using TConstVec = typename TTarget::TConstVec;

        TTimeWriter(const ui32 totalIterations,
                    const TString& outputFile,
                    TString noticeLogSuffix)
                : TotalIterations(totalIterations)
                  , Output(outputFile)
                  , StartTime(Now())
                  , NoticeLogSuffix(std::move(noticeLogSuffix))
        {
        }

        void Init(const TAdditiveModel<TWeakModel>& model,
                  const TTarget& target,
                  const TConstVec& point) override
        {
            Y_UNUSED(target);
            Y_UNUSED(point);
            StartTime = Now();
            FirstIteration = model.Size();
        }

        void UpdateEnsemble(const TAdditiveModel<TWeakModel>& newEnsemble,
                            const TTarget& target,
                            const TConstVec& point) override
        {
            Y_UNUSED(target);
            Y_UNUSED(point);
            const ui32 passedIterations = newEnsemble.Size();

            auto passedTime = (Now() - StartTime).GetValue();
            auto remainingTime = passedTime * (TotalIterations - passedIterations) / (passedIterations - FirstIteration);

            Output << newEnsemble.Size() - 1 << "\t" << TDuration(remainingTime).MilliSeconds() << "\t"
                   << TDuration(passedTime).MilliSeconds() << Endl;

            MATRIXNET_NOTICE_LOG << "total: " << HumanReadable(TDuration(passedTime));
            MATRIXNET_NOTICE_LOG << "\tremaining: " << HumanReadable(TDuration(remainingTime));
            MATRIXNET_NOTICE_LOG << NoticeLogSuffix;
        }

    private:
        ui32 TotalIterations = 0;
        TOFStream Output;
        ui32 Iteration = 0;
        ui32 FirstIteration = 0;
        TInstant StartTime;
        TString NoticeLogSuffix;
    };
}
