#pragma once

#include "compat_runtime/objc_runtime.hpp"
#include "compat_runtime/shim_registry.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace radek::compat_runtime::objc {

/**
 * Bounded guest-facing Objective-C core adapters.
 *
 * Class and object pointers are materialized in the guest address space, while
 * selectors and method dispatch are backed by the host-testable Runtime. This
 * is not a complete Apple Objective-C ABI or framework implementation.
 */
class ShimAdapter {
    struct PropertyCopyContinuation {
        GuestAddress receiver = 0;
        std::int32_t offset = 0;
        GuestAddress continuationStackPointer = 0;
        GuestAddress originalStackPointer = 0;
        std::uint32_t originalStackWord = 0;
        GuestAddress originalReturnAddress = 0;
        bool atomic = false;
    };

    struct GuestState {
        std::map<const Class *, GuestAddress> classAddresses;
        std::map<GuestAddress, const Class *> classesByAddress;
        std::map<Object *, GuestAddress> objectAddresses;
        std::map<GuestAddress, Object *> objectsByAddress;
        std::map<Selector, GuestAddress> selectorAddresses;
        std::map<GuestAddress, std::string> protocolNames;
        std::map<std::string, GuestAddress> protocolAddresses;
        std::map<std::string, std::vector<std::string>> protocolParents;
        std::map<std::uint32_t, PropertyCopyContinuation> pendingPropertyCopies;
    };

    struct AutoreleasePoolState {
        Object *poolObject = nullptr;
        std::vector<Object *> objects;
    };

    struct GuestImplementation {
        GuestAddress address = 0;
        std::string typeEncoding;
    };

    Runtime runtime_;
    Class *rootClass_ = nullptr;
    Class *autoreleasePoolClass_ = nullptr;
    std::map<std::string, Class *> classes_;
    std::map<const GuestAddressSpace *, std::unique_ptr<GuestState>> guestStates_;
    std::map<Selector, std::string> selectorNames_;
    std::map<std::string, Selector> selectorIds_;
    std::map<std::pair<const Class *, Selector>, GuestImplementation> guestImplementations_;
    std::map<GuestAddress, AutoreleasePoolState> autoreleasePools_;
    std::vector<GuestAddress> activeAutoreleasePools_;
    std::map<const GuestAddressSpace *, GuestAddress> emptyDataAddresses_;
    std::mutex mutex_;
    std::mutex propertyMutex_;
    std::uint32_t nextCallout_ = 0xf0004000;
    std::uint32_t nextPropertyCopyToken_ = 1;
    GuestAddress propertyCopyContinuationAddress_ = 0;
    bool registered_ = false;

    GuestState &guestState(GuestAddressSpace &memory);
    GuestAddress ensureClassAddress(GuestAddressSpace &memory, const Class *klass);
    GuestAddress ensureObjectAddress(GuestAddressSpace &memory, Object *object);
    std::string readGuestString(const GuestAddressSpace &memory, GuestAddress address) const;
    Selector selectorForGuest(GuestAddressSpace &memory, GuestAddress address);
    GuestAddress emptyDataAddress(GuestAddressSpace &memory);
    GuestAddress selectorStringAddress(GuestAddressSpace &memory, Selector selector);
    bool autoreleasePoolPush(CpuRegisterState &registers, GuestAddressSpace &memory,
                             std::string &reason);
    bool autoreleasePoolPop(CpuRegisterState &registers, GuestAddressSpace &memory,
                            std::string &reason);
    bool initializeImage(GuestAddressSpace &memory,
                         const std::vector<GuestImageSection> &sections,
                         std::string &reason);
    bool dispatch(CpuRegisterState &registers, GuestAddressSpace &memory,
                  std::string &reason, bool superDispatch = false,
                  Value *rawReturn = nullptr, GuestAddress *guestTarget = nullptr);
    bool dispatchStret(CpuRegisterState &registers, GuestAddressSpace &memory,
                       std::string &reason, GuestAddress *guestTarget = nullptr);
    bool setProperty(CpuRegisterState &registers, GuestAddressSpace &memory,
                     std::string &reason);
    bool setPropertyOrCopy(CpuRegisterState &registers, GuestAddressSpace &memory,
                           GuestAddress &guestTarget, std::string &reason);
    bool continuePropertyCopy(CpuRegisterState &registers, GuestAddressSpace &memory,
                              std::string &reason);
    bool storePropertyValue(CpuRegisterState &registers, GuestAddressSpace &memory,
                            GuestAddress receiver, std::int32_t offset, GuestAddress value,
                            bool atomic, GuestAddress originalStackPointer,
                            GuestAddress originalReturnAddress, std::string &reason);
    bool searchPaths(CpuRegisterState &registers, GuestAddressSpace &memory,
                     std::string &reason);
    GuestAddress createGuestString(GuestAddressSpace &memory, const std::string &value,
                                   bool autorelease);
    GuestAddress createGuestStringArray(GuestAddressSpace &memory,
                                       const std::vector<std::string> &values,
                                       bool autorelease);
    void autoreleaseGuestObject(Object *object);
    bool getClass(CpuRegisterState &registers, GuestAddressSpace &memory,
                  std::string &reason, bool metaclass = false);
    bool registerFunction(ShimRegistry &registry, const std::string &symbol,
                          const std::string &adapterName,
                          std::function<bool(CpuRegisterState &, GuestAddressSpace &,
                                             std::string &)> invoke);
    bool registerExceptionFunction(ShimRegistry &registry, const std::string &symbol,
                                   const std::string &adapterName,
                                   std::function<bool(CpuRegisterState &, GuestAddressSpace &,
                                                      std::string &)> invoke);
    bool registerTransferFunction(ShimRegistry &registry, const std::string &symbol,
                                  const std::string &adapterName,
                                  std::function<bool(CpuRegisterState &, GuestAddressSpace &,
                                                     GuestAddress &, std::string &)> invoke);
    void registerClassSymbols(ShimRegistry &registry);
    void registerEmptyDataSymbol(ShimRegistry &registry, const std::string &symbol);
    Object *objectForGuest(GuestAddressSpace &memory, GuestAddress address);
    const Class *classForGuest(GuestAddressSpace &memory, GuestAddress address);
    void synchronizeObject(GuestAddressSpace &memory, Object *object, GuestAddress address);
    void releaseObject(GuestAddressSpace &memory, Object *object);
    void drainPool(GuestAddressSpace &memory, Object *poolObject);

  public:
    ShimAdapter();
    ~ShimAdapter();
    ShimAdapter(const ShimAdapter &) = delete;
    ShimAdapter &operator=(const ShimAdapter &) = delete;

    /** Add only the named Objective-C symbols with an explicit native adapter. */
    void registerBindings(ShimRegistry &registry);
    Runtime &runtime() noexcept { return runtime_; }
    const Runtime &runtime() const noexcept { return runtime_; }
};

} // namespace radek::compat_runtime::objc
