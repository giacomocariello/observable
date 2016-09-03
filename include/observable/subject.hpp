#pragma once
#include <memory>
#include <mutex>
#include <unordered_map>
#include "detail/function_collection.hpp"
#include "detail/function_traits.hpp"
#include "detail/handle.hpp"

namespace observable {

//! Handle that manages a subscription.
using subscription = detail::handle;
using auto_unsubscribe = detail::auto_handle;

//! This class stores observers and provides a way to notify them with
//! heterogeneous parameters.
//!
//! An observer is a generic function. Once you call subscribe(), the observer
//! is said to be subscribed to notifications from the subject.
//!
//! A call to notify() or notify_tagged() calls all observers that have matching
//! parameter types and have subscribed with the same tag value (in case you
//! called notify_tagged).
//!
//! \tparam Tag The type of the tag parameter for tagged subscriptions.
//! \note All methods defined in this class are safe to be called in parallel.
template <typename Tag=std::string>
class subject
{
public:
    //! Subscribe to all untagged notifications.
    template <typename Function>
    auto subscribe(Function && function) -> subscription;

    //! Subscribe to all notifications tagged with the provided tag value.
    template <typename T, typename Function>
    auto subscribe(T && tag, Function && fun) -> subscription;

    //! Notify all untagged subscriptions.
    template <typename ... Arguments>
    auto notify_untagged(Arguments && ... arguments) const -> void;

    //! Notify all tagged subscriptions that have used the provided tag value
    //! when subscribing.
    template <typename T, typename ... Arguments>
    auto notify_tagged(T && tag, Arguments && ... arguments) const -> void;

public:
    //! Create an empty subject.
    subject() = default;

    //! Subject is not copy-constructible.
    subject(subject const & other) =delete;

    //! Subject is not copy-assignable.
    auto operator=(subject const & other) -> subject & =delete;

    //! Subject is move-constructible.
    subject(subject && other) noexcept =default;

    //! Subject is move-assignable.
    auto operator=(subject && other) -> subject & =default;

private:
    using collection_map = std::unordered_map<Tag, detail::function_collection>;

    std::shared_ptr<collection_map> functions_ { std::make_shared<collection_map>() };
    mutable std::shared_ptr<std::mutex> mutex_ { std::make_shared<std::mutex>() };
};

namespace detail {
    //! Check if the provided function can be used as a subscription callback
    //! function.
    template <typename Function>
    constexpr auto check_compatibility()
    {
        using traits = function_traits<Function>;

        static_assert(std::is_same<typename traits::return_type, void>::value,
                      "Subscription function cannot return a value.");

        static_assert(std::is_convertible<
                            std::function<typename traits::signature>,
                            std::function<typename traits::normalized>>::value,
                      "Subscription function arguments must not be non-const "
                      "references.");
    }

    //! Call the provided collection with the provided arguments.
    template <typename Collection, typename ... Arguments>
    auto call(Collection && collection, Arguments && ... arguments)
    {
        using signature = typename function_traits<void(Arguments ...)>::normalized;
        check_compatibility<signature>(); // This should never fail.

        collection.template call_all<signature>(std::forward<Arguments>(arguments) ...);
    }

    template <typename Mutex, typename CollectionMap>
    auto snapshot(std::shared_ptr<Mutex> const & mutex,
                  std::shared_ptr<CollectionMap> & functions) -> void
    {
        std::lock_guard<Mutex> lock { *mutex };
        functions = std::make_shared<CollectionMap>(*functions);
    }

    template <typename Mutex, typename CollectionMap, typename Tag, typename Id>
    auto unsubscribe(std::weak_ptr<Mutex> const & mutex,
                     std::shared_ptr<CollectionMap> & functions,
                     Tag const & tag,
                     Id const & id) -> void
    {
        auto const mutex_ptr = mutex.lock();
        if(!mutex_ptr)
            return;

        snapshot(mutex_ptr, functions);

        auto const it = functions->find(tag);
        if(it == end(*functions))
            return;

        it->second.remove(id);
    }
}

template <typename Tag>
template <typename Function>
inline auto subject<Tag>::subscribe(Function && function) -> subscription
{
    static Tag const no_tag { };
    return subscribe(no_tag, std::forward<Function>(function));
}

template <typename Tag>
template <typename T, typename Function>
inline auto subject<Tag>::subscribe(T && tag, Function && function) -> subscription
{
    using traits = detail::function_traits<Function>;
    using signature = typename traits::normalized;
    detail::check_compatibility<Function>();

    snapshot(mutex_, functions_);

    auto & collection = (*functions_)[tag];
    auto id = collection.template insert<signature>(std::forward<Function>(function));

    return subscription {
                [this, tag { tag }, id, mutex = std::weak_ptr<std::mutex> { mutex_ }]() mutable {
                    detail::unsubscribe(mutex, functions_, tag, id);
                }
            };
}

template <typename Tag>
template<typename ... Arguments>
inline void subject<Tag>::notify_untagged(Arguments && ... arguments) const
{
    static Tag const no_tag { };
    return notify_tagged(no_tag, std::forward<Arguments>(arguments) ...);
}

template <typename Tag>
template<typename T, typename ... Arguments>
inline void subject<Tag>::notify_tagged(T && tag, Arguments && ... arguments) const
{
    std::shared_ptr<collection_map const> const functions = functions_;
    auto const it = functions->find(tag);
    if(it == end(*functions))
        return;

    detail::call(it->second, std::forward<Arguments>(arguments) ...);
}

}
