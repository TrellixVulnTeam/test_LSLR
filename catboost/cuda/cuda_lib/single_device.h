#pragma once

#include "cuda_base.h"
#include "remote_objects.h"
#include "gpu_single_worker.h"
#include "cuda_events_provider.h"
#include "kernel.h"
#include <future>

namespace NKernelHost {
    class TWaitStreamSubmitSingleHostKernel: public TKernelBase<void, true> {
    private:
        NThreading::TPromise<ui64> Promise;

    public:
        TWaitStreamSubmitSingleHostKernel() {
            Promise = NThreading::NewPromise<ui64>();
        }

        NCudaLib::TDeviceFuture<ui64> GetResult() {
            Promise = NThreading::NewPromise<ui64>();
            return NCudaLib::TDeviceFuture<ui64>(Promise.GetFuture());
        }

        void Run(const TCudaStream& stream) const {
            Y_UNUSED(stream);
        }

        void Postprocess(const TCudaStream& stream) {
            Y_UNUSED(stream);
            Promise.SetValue(0ULL);
        }
    };
}

//TODO: (noxoomo)  In future we'll need to split this logic to two parts: 1) transfer kernel to remote host (2 impl, single-host + mpi) 2) all other common logic
namespace NCudaLib {
    class TTerminateOnErrorCallback: public IExceptionCallback {
    public:
        void Call(const TString& message) override {
            MATRIXNET_ERROR_LOG << "Application terminated with error: " << message << Endl;
            std::terminate();
        }
    };

    class TCudaSingleDevice {
    private:
        template <EPtrType PtrType>
        using TPtr = typename TMemoryProviderImplTrait<PtrType>::TRawFreeMemory;

        static const ui64 OBJECT_HANDLE_REQUEST_SIZE = 1024;
        TGpuOneDeviceWorker& Worker;

        TCudaDeviceProperties DeviceProperties;
        TVector<ui64> FreeHandles;
        TVector<ui64> UserFreeStreams;

        template <class T>
        friend class THandleBasedObject;

        ui64 GetFreeHandle() {
            if (FreeHandles.size() == 0) {
                FreeHandles = Worker.RequestHandles(OBJECT_HANDLE_REQUEST_SIZE);
            }
            Y_ASSERT(FreeHandles.size());
            auto handle = FreeHandles.back();
            FreeHandles.pop_back();
            return handle;
        }

        friend class TSingleHostDevicesProvider;
        yset<TCudaSingleDevice*> PeerDevices;

    public:
        template <class T>
        class THandleBasedObject: private TNonCopyable, public TThrRefBase {
        private:
            static const ui64 EMPTY_HANDLE = 0;
            TCudaSingleDevice* Device;
            ui64 Handle;

        public:
            THandleBasedObject(TCudaSingleDevice* device,
                               ui64 handle)
                : Device(device)
                , Handle(handle)
            {
            }

            ~THandleBasedObject() {
                if (Handle != EMPTY_HANDLE) {
                    Device->ResetPointer(GetPointer());
                    Device->FreeHandles.push_back(Handle);
                }
            }

            THandleBasedPointer<T> GetPointer() const {
                return THandleBasedPointer<T>(Handle);
            }

            friend class TCudaSingleDevice;
        };

        template <class T, EPtrType Type>
        class TSingleBuffer: public TMoveOnly {
        private:
            using TRawPtr = typename TMemoryProviderImplTrait<Type>::TRawFreeMemory;
            TIntrusivePtr<THandleBasedObject<TRawPtr>> Memory;
            ui64 AllocatedSize;
            TCudaSingleDevice* Owner;
            ui64 Offset;
            friend class TDataCopier;

        public:
            TSingleBuffer(TIntrusivePtr<THandleBasedObject<TRawPtr>> memory,
                          ui64 allocatedSize,
                          TCudaSingleDevice* device,
                          ui64 offset = 0)
                : Memory(std::move(memory))
                , AllocatedSize(allocatedSize)
                , Owner(device)
                , Offset(offset)
            {
            }

            TSingleBuffer()
                : Memory(nullptr)
                , AllocatedSize(0)
                , Owner(nullptr)
                , Offset(0)
            {
            }

            ui64 GetOffset() const {
                return Offset;
            }

            bool IsEmpty() const {
                return Size() == 0;
            }

            bool NotEmpty() const {
                return !IsEmpty();
            }

            ui64 Size() const {
                Y_ASSERT(Offset <= AllocatedSize);
                CB_ENSURE(Offset <= AllocatedSize, TStringBuilder() << "Offset " << Offset << " > size " << AllocatedSize);
                return AllocatedSize - Offset;
            }

            TSingleBuffer ShiftedBuffer(ui64 offset) const {
                return TSingleBuffer(Memory, AllocatedSize, Owner, Offset + offset);
            }

            TSingleBuffer<const T, Type> ShiftedConstBuffer(ui64 offset) const {
                return TSingleBuffer<const T, Type>(Memory, AllocatedSize, Owner, Offset + offset);
            }

            template <class U>
            TSingleBuffer<U, Type> ReinterpretCast() {
                static_assert(sizeof(U) == sizeof(T), "Error: support to reinterpret cast of equal element size only");
                return TSingleBuffer<U, Type>(Memory, AllocatedSize, Owner, Offset);
            }

            template <class U>
            TSingleBuffer<const U, Type> ReinterpretCast() const {
                static_assert(sizeof(U) == sizeof(T), "Error: support to reinterpret cast of equal element size only");
                return TSingleBuffer<const U, Type>(Memory, AllocatedSize, Owner, Offset);
            }

            ui64 MemoryHandle() const {
                return Memory != nullptr ? Memory->Handle : 0;
            }

            THandleBasedMemoryPointer<T, Type> GetPointer() {
                return THandleBasedMemoryPointer<T, Type>(MemoryHandle(),
                                                          Offset);
            }

            THandleBasedMemoryPointer<const T, Type> GetPointer() const {
                return THandleBasedMemoryPointer<const T, Type>(MemoryHandle(),
                                                                Offset);
            }
        };

        void AddTask(THolder<IGpuCommand>&& cmd) {
            Worker.AddTask(std::move(cmd));
        }

    public:
        TCudaSingleDevice(TGpuOneDeviceWorker& worker)
            : Worker(worker)
            , DeviceProperties(Worker.GetDeviceProperties())
        {
            FreeHandles = Worker.RequestHandles(OBJECT_HANDLE_REQUEST_SIZE);
            Worker.RegisterErrorCallback(new TTerminateOnErrorCallback);
        }

        ~TCudaSingleDevice() {
            for (ui32 stream : UserFreeStreams) {
                Worker.FreeStream(stream);
            }
            WaitComplete().Wait();
        }

        template <class T, EPtrType Type>
        TSingleBuffer<T, Type> CreateSingleBuffer(ui64 size) {
            using TRawPtr = typename TMemoryProviderImplTrait<Type>::TRawFreeMemory;
            auto ptr = CreateRemoteObject<TRawPtr>(size * sizeof(T));
            return TSingleBuffer<T, Type>(ptr.Release(), size, this);
        };

        template <class T, class... Args>
        THolder<THandleBasedObject<T>> CreateRemoteObject(Args&&... args) {
            auto handle = GetFreeHandle();
            THolder<IGpuCommand> cmd = TCreateObjectCommandTrait<T>::Create(handle, std::forward<Args>(args)...);
            Worker.AddTask(std::move(cmd));
            return MakeHolder<THandleBasedObject<T>>(this, handle);
        }

        template <class TKernel>
        void LaunchKernel(TKernel&& kernel,
                          ui32 stream) const {
            using TKernelTask = TGpuKernelTask<TKernel>;
            auto task = MakeHolder<TKernelTask>(std::move(kernel), stream);
            Worker.AddTask(std::move(task));
        }

        template <class T>
        void ResetPointer(THandleBasedPointer<T> ptr) {
            using TTask = TResetRemotePointerCommand<T>;
            Worker.AddTask(MakeHolder<TTask>(ptr));
        }

        bool HasPeerAccess(TCudaSingleDevice* to) const {
            return (bool)PeerDevices.count(to);
        }

        template <class TFunc>
        auto LaunchFunc(TFunc&& func) -> TDeviceFuture<decltype(func())> {
            using TTask = THostTask<TFunc>;
            auto task = MakeHolder<TTask>(std::move(func));
            auto futureResult = task->GetResult();
            Worker.AddTask(std::move(task));
            return futureResult;
        }

        void FreeStream(ui32 streamId) {
            CB_ENSURE(streamId != 0);
            UserFreeStreams.push_back(streamId);
        }

        ui64 DefaultStream() const {
            return 0;
        }

        void StreamSynchronize(ui32 streamHandle) {
            LaunchKernel(TSyncStreamKernel(), streamHandle);
            //ensure all jobs to stream we submitted to GPU (so streamSync was executed and blocked until all stream jobs are done)
            Worker.AddTask(MakeHolder<TWaitSubmitCommand>());
        }

        TDeviceFuture<ui64> WaitComplete() {
            return LaunchFunc<TBlockingSyncDevice>(TBlockingSyncDevice());
        }

        TDeviceFuture<ui64> WaitStreamSubmit(ui32 stream) {
            auto kernel = NKernelHost::TWaitStreamSubmitSingleHostKernel();
            TDeviceFuture<ui64> future = kernel.GetResult();
            LaunchKernel(std::move(kernel), stream);
            return future;
        }

        void NonBlockingSynchronize() {
            StreamSynchronize(0);
        }

        const TCudaDeviceProperties& GetDeviceProperties() const {
            return DeviceProperties;
        }

        ui32 CudaDeviceId() const {
            return Worker.GetDeviceId();
        }

        ui64 GetGpuRamSize() {
            return Worker.GetGpuRamSize();
        }

        ui64 GetFreeMemorySize() {
            return Worker.GetFreeMemory();
        }

        ui64 RequestStream() {
            if (UserFreeStreams.size() == 0) {
                UserFreeStreams.push_back(Worker.RequestStream());
            }
            ui64 id = UserFreeStreams.back();
            UserFreeStreams.pop_back();
            return id;
        }
    };

    template <EPtrType Type>
    using TRawFreeMemory = typename TMemoryProviderImplTrait<Type>::TRawFreeMemory;

    template <>
    struct TCreateObjectCommandTrait<TRawFreeMemory<CudaDevice>> {
        static THolder<IAllocateMemoryTask> Create(ui64 handle, ui64 size) {
            return new TCudaMallocTask<CudaDevice>(handle, size);
        }
    };

    template <>
    struct TCreateObjectCommandTrait<TRawFreeMemory<CudaHost>> {
        static THolder<IAllocateMemoryTask> Create(ui64 handle, ui64 size) {
            return new TCudaMallocTask<CudaHost>(handle, size);
        }
    };

    template <>
    struct TCreateObjectCommandTrait<TRawFreeMemory<Host>> {
        static THolder<IAllocateMemoryTask> Create(ui64 handle, ui64 size) {
            return new TCudaMallocTask<Host>(handle, size);
        }
    };
}
