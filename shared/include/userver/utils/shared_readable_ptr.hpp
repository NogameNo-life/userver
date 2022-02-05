#pragma once

/// @file userver/utils/shared_readable_ptr.hpp
/// @brief @copybrief utils::SharedReadablePtr

#include <memory>
#include <type_traits>

USERVER_NAMESPACE_BEGIN

namespace utils {

/// @ingroup userver_containers
///
/// @brief std::shared_ptr<const T> wrapper that makes sure that the pointer
/// is stored before dereferencing. Protects from dangling references:
/// @code
///   // BAD! Result of `config_.Get()` may be destroyed after the invocation.
///   const auto& config = config_.Get()->Get<taxi_config::TaxiConfig>();
/// @endcode
template <typename T>
class SharedReadablePtr final {
  static_assert(!std::is_const_v<T>,
                "SharedReadablePtr already adds `const` to `T`");
  static_assert(!std::is_reference_v<T>,
                "SharedReadablePtr does not work with references");

 public:
  using Base = std::shared_ptr<const T>;

  SharedReadablePtr(const SharedReadablePtr& ptr) = default;
  SharedReadablePtr(SharedReadablePtr&& ptr) noexcept = default;

  SharedReadablePtr& operator=(const SharedReadablePtr& ptr) = default;
  SharedReadablePtr& operator=(SharedReadablePtr&& ptr) noexcept = default;

  SharedReadablePtr(const Base& ptr) noexcept : base_(ptr) {}

  SharedReadablePtr(Base&& ptr) noexcept : base_(std::move(ptr)) {}

  SharedReadablePtr& operator=(const Base& ptr) {
    base_ = ptr;
    return *this;
  }

  SharedReadablePtr& operator=(Base&& ptr) noexcept {
    base_ = std::move(ptr);
    return *this;
  }

  const T& operator*() const& noexcept { return *base_; }

  const T& operator*() && { ReportMisuse(); }

  const T* operator->() const& noexcept { return base_.get(); }

  const T* operator->() && { ReportMisuse(); }

  operator const Base&() const& noexcept { return base_; }

  operator const Base&() && { ReportMisuse(); }

  explicit operator bool() const noexcept { return !!base_; }

  bool operator==(const SharedReadablePtr<T>& other) const {
    return base_ == other.base_;
  }

  bool operator!=(const SharedReadablePtr<T>& other) const {
    return !(*this == other);
  }

  void Reset() noexcept { base_.reset(); }

 private:
  [[noreturn]] static void ReportMisuse() {
    static_assert(!sizeof(T), "keep the pointer before using, please");
  }

  Base base_;
};

template <typename T>
bool operator==(const SharedReadablePtr<T>& ptr, std::nullptr_t) {
  return !ptr;
}

template <typename T>
bool operator==(std::nullptr_t, const SharedReadablePtr<T>& ptr) {
  return !ptr;
}

template <typename T>
bool operator!=(const SharedReadablePtr<T>& ptr, std::nullptr_t) {
  return !(ptr == nullptr);
}

template <typename T>
bool operator!=(std::nullptr_t, const SharedReadablePtr<T>& ptr) {
  return !(nullptr == ptr);
}

template <typename T, typename... Args>
SharedReadablePtr<T> MakeSharedReadable(Args&&... args) {
  return SharedReadablePtr<T>{std::make_shared<T>(std::forward<Args>(args)...)};
}

}  // namespace utils

USERVER_NAMESPACE_END
