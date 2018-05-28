#include "xchainer/routines/connection.h"

#include <cstdint>
#include <vector>

#include <nonstd/optional.hpp>

#include "xchainer/array.h"
#include "xchainer/constant.h"
#include "xchainer/device.h"
#include "xchainer/routines/math.h"
#include "xchainer/stack_vector.h"

namespace xchainer {
namespace internal {

int64_t GetConvOutDim(int64_t in_dim, int64_t kernel_size, int64_t stride, int64_t pad, bool cover_all) {
    if (cover_all) {
        return (in_dim + pad * 2 - kernel_size + stride - 1) / stride + 1;
    }
    return (in_dim + pad * 2 - kernel_size) / stride + 1;
}

int64_t GetConvTransposeOutDim(int64_t in_dim, int64_t kernel_size, int64_t stride, int64_t pad, bool cover_all) {
    if (cover_all) {
        return stride * (in_dim - 1) + kernel_size - stride + 1 - 2 * pad;
    }
    return stride * (in_dim - 1) + kernel_size - 2 * pad;
}

}  // namespace internal

namespace {

Array ConvGradW(
        Dtype w_dtype,
        const Shape& w_shape,
        const Array& x,
        const Array& gy,
        const StackVector<int64_t, kMaxNdim>& stride,
        const StackVector<int64_t, kMaxNdim>& pad,
        bool cover_all) {
    int8_t ndim = w_shape.ndim() - 2;  // Number of spacial dimensions
    assert(ndim > 0);
    assert(x.ndim() == ndim + 2);
    assert(gy.ndim() == ndim + 2);
    assert(stride.size() == static_cast<size_t>(ndim));
    assert(pad.size() == static_cast<size_t>(ndim));
    Array out = x.device().ConvGradWeight(w_dtype, w_shape, x, gy, stride, pad, cover_all);

    auto x_backward_function =
            [ x_shape = x.shape(), gy, stride, pad ](const Array& gout, const std::vector<GraphId>& graph_ids_to_stop_gradient)->Array {
        StackVector<int64_t, kMaxNdim> out_size{x_shape.begin() + 2, x_shape.end()};
        assert(out_size.size() == stride.size());
        return ConvTranspose(gy.AsConstant(graph_ids_to_stop_gradient), gout, nonstd::nullopt, stride, pad, out_size);
    };
    auto gy_backward_function = [x, stride, pad, cover_all](
                                        const Array& gout, const std::vector<GraphId>& graph_ids_to_stop_gradient) -> Array {
        return Conv(x.AsConstant(graph_ids_to_stop_gradient), gout, nonstd::nullopt, stride, pad, cover_all);
    };
    internal::SetUpOpNodes("conv-grad-weight", {x, gy}, out, {x_backward_function, gy_backward_function});

    return out;
}

}  // namespace

Array Conv(
        const Array& x,
        const Array& w,
        const nonstd::optional<Array>& b,
        const StackVector<int64_t, kMaxNdim>& stride,
        const StackVector<int64_t, kMaxNdim>& pad,
        bool cover_all) {
    Array out = x.device().Conv(x, w, b, stride, pad, cover_all);
    auto x_backward_function =
            [ x_shape = x.shape(), w, stride, pad ](const Array& gout, const std::vector<GraphId>& graph_ids_to_stop_gradient)->Array {
        StackVector<int64_t, kMaxNdim> out_size{x_shape.begin() + 2, x_shape.end()};
        return ConvTranspose(gout, w.AsConstant(graph_ids_to_stop_gradient), nonstd::nullopt, stride, pad, out_size);
    };
    auto w_backward_function = [ w_dtype = w.dtype(), w_shape = w.shape(), x, stride, pad, cover_all ](
                                       const Array& gout, const std::vector<GraphId>& graph_ids_to_stop_gradient)
                                       ->Array {
        return ConvGradW(w_dtype, w_shape, x.AsConstant(graph_ids_to_stop_gradient), gout, stride, pad, cover_all);
    };
    if (b.has_value()) {
        auto b_backward_function = [](const Array& gout, const std::vector<GraphId>&) -> Array {
            Axes axis{0};
            for (int8_t i = 2; i < gout.ndim(); ++i) {
                axis.emplace_back(int64_t{i});
            }
            return Sum(gout, axis, false);
        };
        internal::SetUpOpNodes("conv", {x, w, *b}, out, {x_backward_function, w_backward_function, b_backward_function});
    } else {
        internal::SetUpOpNodes("conv", {x, w}, out, {x_backward_function, w_backward_function});
    }
    return out;
}

Array ConvTranspose(
        const Array& x,
        const Array& w,
        const nonstd::optional<Array>& b,
        const StackVector<int64_t, kMaxNdim>& stride,
        const StackVector<int64_t, kMaxNdim>& pad,
        const nonstd::optional<StackVector<int64_t, kMaxNdim>>& out_size) {
    int8_t ndim = x.ndim() - 2;  // Number of spacial dimensions

    // Compute out_size if not specified
    StackVector<int64_t, kMaxNdim> real_out_size;
    if (out_size.has_value()) {
        real_out_size = *out_size;
    } else {
        for (int8_t i = 0; i < ndim; ++i) {
            real_out_size.emplace_back(internal::GetConvTransposeOutDim(x.shape()[i + 2], w.shape()[i + 2], stride[i], pad[i], false));
        }
    }

    // Compute transposed convolution
    Array out = x.device().ConvTranspose(x, w, b, stride, pad, real_out_size);

    // Detect cover_all
    bool cover_all = false;
    for (int8_t i = 0; i < x.ndim(); ++i) {
        int64_t xdim = x.shape()[i];
        if (xdim != internal::GetConvOutDim(xdim, real_out_size[i], w.shape()[i + 2], stride[i], pad[i])) {
            cover_all = true;
            break;
        }
    }

    auto x_backward_function =
            [ x_shape = x.shape(), w, stride, pad, cover_all ](const Array& gout, const std::vector<GraphId>& graph_ids_to_stop_gradient)
                    ->Array {
        return Conv(gout, w.AsConstant(graph_ids_to_stop_gradient), nonstd::nullopt, stride, pad, cover_all);
    };
    auto w_backward_function = [ w_dtype = w.dtype(), w_shape = w.shape(), x, stride, pad, cover_all ](
                                       const Array& gout, const std::vector<GraphId>& graph_ids_to_stop_gradient)
                                       ->Array {
        return ConvGradW(w_dtype, w_shape, gout, x.AsConstant(graph_ids_to_stop_gradient), stride, pad, cover_all);
    };
    if (b.has_value()) {
        auto b_backward_function = [](const Array& gout, const std::vector<GraphId>&) -> Array {
            Axes axis{0};
            for (int8_t i = 2; i < gout.ndim(); ++i) {
                axis.emplace_back(int64_t{i});
            }
            return Sum(gout, axis, false);
        };
        internal::SetUpOpNodes("conv_transpose", {x, w, *b}, out, {x_backward_function, w_backward_function, b_backward_function});
    } else {
        internal::SetUpOpNodes("conv_transpose", {x, w}, out, {x_backward_function, w_backward_function});
    }
    return out;
}

}  // namespace xchainer
