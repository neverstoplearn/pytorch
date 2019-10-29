#include <ATen/NativeFunctions.h>
#include <ATen/core/LegacyTypeDispatch.h>
#include <ATen/core/grad_mode.h>
#include <torch/detail/TensorDataContainer.h>

namespace torch {

namespace detail {

TensorDataContainer::TensorDataContainer() :
    sizes_({0}),
    scalar_type_(at::ScalarType::Undefined),
    type_(TensorDataContainerType::InitList) {}

#define TENSOR(T, S) \
TensorDataContainer::TensorDataContainer(T value) : \
    sizes_(), \
    scalar_type_(at::k##S), \
    type_(TensorDataContainerType::Scalar), \
    scalar_(value) {}
AT_FORALL_SCALAR_TYPES_AND3(Bool, Half, BFloat16, TENSOR)
#undef TENSOR

TensorDataContainer::TensorDataContainer(std::initializer_list<TensorDataContainer> init_list) :
    sizes_(),
    scalar_type_(init_list.begin()->scalar_type()),
    type_(TensorDataContainerType::InitList),
    init_list_(init_list) {
  const TensorDataContainer& first_elem = *(init_list.begin());
  for (const auto& elem : init_list) {
    TORCH_CHECK(elem.sizes() == first_elem.sizes(),
      "Expected all sub-lists to have sizes: ",
      first_elem.sizes(),
      " (e.g. ", first_elem, "), ",
      "but got sub-list ",
      elem,
      " with sizes: ",
      elem.sizes());
    TORCH_CHECK(elem.scalar_type() == first_elem.scalar_type(),
      "Expected all elements of the tensor to have the same scalar type: ",
      first_elem.scalar_type(),
      ", but got element of scalar type: ",
      elem.scalar_type());
  }
  sizes_.reserve(first_elem.sizes().size() + 1);
  sizes_.push_back(init_list.size());
  sizes_.insert(sizes_.end(), first_elem.sizes().begin(), first_elem.sizes().end());
}

#define TENSOR(T, S) \
TensorDataContainer::TensorDataContainer(at::ArrayRef<T> values) : \
    sizes_({(int64_t)values.size()}), \
    scalar_type_(at::k##S), \
    type_(TensorDataContainerType::Tensor) { \
  at::AutoNonVariableTypeMode non_var_type_mode(true); \
  tensor_ = at::native::tensor(values, at::TensorOptions().device(at::kCPU).is_variable(false)); \
}
AT_FORALL_SCALAR_TYPES_AND3(Bool, Half, BFloat16, TENSOR)
#undef TENSOR

#define TENSOR(T, S) \
TensorDataContainer::TensorDataContainer(const std::vector<T>& values) : \
    TensorDataContainer(at::ArrayRef<T>(values)) {}
AT_FORALL_SCALAR_TYPES_AND2(Half, BFloat16, TENSOR)
#undef TENSOR

bool TensorDataContainer::is_scalar() const {
  return type_ == TensorDataContainerType::Scalar;
}

const c10::Scalar& TensorDataContainer::scalar() const {
  TORCH_CHECK(
    is_scalar(),
    "Can only call `scalar()` on a TensorDataContainer that has `is_scalar() == true`");
  return scalar_;
}

bool TensorDataContainer::is_init_list() const {
  return type_ == TensorDataContainerType::InitList;
}

const std::initializer_list<TensorDataContainer>& TensorDataContainer::init_list() const {
  TORCH_CHECK(
    is_init_list(),
    "Can only call `init_list()` on a TensorDataContainer that has `is_init_list() == true`");
  return init_list_;
}

bool TensorDataContainer::is_tensor() const {
  return type_ == TensorDataContainerType::Tensor;
}

const at::Tensor& TensorDataContainer::tensor() const {
  TORCH_CHECK(
    is_tensor(),
    "Can only call `tensor()` on a TensorDataContainer that has `is_tensor() == true`");
  return tensor_;
}

const std::vector<int64_t>& TensorDataContainer::sizes() const {
  return sizes_;
}

const c10::ScalarType& TensorDataContainer::scalar_type() const {
  return scalar_type_;
}

at::Tensor TensorDataContainer::convert_to_tensor(const at::TensorOptions& options) const {
  if (is_scalar()) {
    at::AutoNonVariableTypeMode non_var_type_mode(true);
    return at::scalar_tensor(scalar_, options.is_variable(false));
  } else if (is_init_list()) {
    // NOTE: Here we explicitly choose to initialize the tensor on CPU first,
    // fill each element of the tensor, and then move the tensor to the desired
    // device. For CUDA device, this approach only involves 1 CUDA kernel launch,
    // and is much faster than initializing the tensor on CUDA first and then
    // filling each element of it (which involves `N` CUDA kernel launches where
    // `N` is the number of the elements in the tensor).
    at::Tensor tensor = ([&]() {
      at::AutoNonVariableTypeMode non_var_type_mode(true);
      return at::empty(sizes_, options.device(at::kCPU).is_variable(false));
    })();
    fill_tensor(tensor);
    return tensor.to(options.device());
  } else if (is_tensor()) {
    return tensor_.to(options);
  } else {
    TORCH_INTERNAL_ASSERT(false, "Invalid TensorDataContainer type");
  }
}

void TensorDataContainer::pretty_print_recursive(std::ostream& stream) const {
  if (is_scalar()) {
    // NOTE: There is no `operator<<` overload for `at::kBFloat16` type,
    // and we need to convert it to `float` type using `operator float()` function
    // defined in `c10/util/BFloat16.h`.
    if (scalar_type_ == at::ScalarType::BFloat16) {
      stream << static_cast<float>(scalar_.to<c10::BFloat16>());
    } else {
      AT_DISPATCH_ALL_TYPES_AND2(
          at::kBool,
          at::kHalf,
          scalar_type_,
          "TensorDataContainer_pretty_print_scalar", [&] {
        stream << scalar_.to<scalar_t>();
      });
    }
  } else if (is_init_list()) {
    stream << "{";
    for (const TensorDataContainer* it = init_list_.begin(); it != init_list_.end(); it++) {
      stream << *it;
      if (std::next(it) != init_list_.end()) stream << ", ";
    }
    stream << "}";
  } else if (is_tensor()) {
    stream << "{";
    for (int64_t i = 0; i < tensor_.sizes()[0]; i++) {
      // NOTE: There is no `operator<<` overload for `at::kBFloat16` type,
      // and we need to convert it to `float` type using `operator float()` function
      // defined in c10/util/BFloat16.h.
      if (scalar_type_ == at::ScalarType::BFloat16) {
        stream << static_cast<float>(tensor_[i].item<c10::BFloat16>());
      } else {
        AT_DISPATCH_ALL_TYPES_AND2(
            at::kBool,
            at::kHalf,
            scalar_type_,
            "TensorDataContainer_pretty_print_tensor_item", [&] {
          stream << tensor_[i].item<scalar_t>();
        });
      }
    }
    stream << "}";
  } else {
    TORCH_INTERNAL_ASSERT(false, "Invalid TensorDataContainer type");
  }
}

void TensorDataContainer::fill_tensor(at::Tensor tensor) const {
  if (is_scalar()) {
    TORCH_INTERNAL_ASSERT(
      tensor.dim() == 0,
      "Expected a 0-dim Tensor, but got Tensor with dimensions: ", tensor.dim());
    at::NoGradGuard guard;
    tensor.fill_(scalar_);
  } else if (is_init_list()) {
    TORCH_INTERNAL_ASSERT(
      tensor.sizes()[0] == init_list_.size(),
      "Expected a Tensor with size ",
      init_list_.size(),
      " in its first dimension, but got Tensor with size ",
      tensor.sizes()[0],
      " in its first dimension");
    size_t index = 0;
    for (const auto& elem : init_list_) {
      elem.fill_tensor(tensor[index]);
      index++;
    }
  } else if (is_tensor()) {
    TORCH_INTERNAL_ASSERT(
      false,
      "TensorDataContainer is already a Tensor type, `fill_tensor` should not be called");
  } else {
    TORCH_INTERNAL_ASSERT(false, "Invalid TensorDataContainer type");
  }
}

std::ostream& operator<<(std::ostream& stream, const TensorDataContainer& tensor_data_container) {
  tensor_data_container.pretty_print_recursive(stream);
  return stream;
}

} // namespace detail

} // namespace torch