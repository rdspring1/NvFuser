# SPDX-FileCopyrightText: Copyright (c) 2024-present NVIDIA CORPORATION & AFFILIATES.
# All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
import enum
from typing import Any, Dict, List, Optional, Tuple

import torch

import nvfuser

from nvfuser.pytorch_utils import torch_dtype_to_nvfuser_dtype


__all__ = [
    "InstanceNorm1dNVFuser",
    "InstanceNorm2dNVFuser",
    "InstanceNorm3dNVFuser",
]


NamedAxis = enum.Enum("NamedAxis", ["BATCH", "CHANNEL"])


def partially_contig_tensor(
    fd: "nvfuser.FusionDefinition",
    x: torch.Tensor,
) -> "nvfuser.Tensor":
    return fd.define_tensor(
        shape=[1 if dim_size == 1 else -1 for dim_size in x.size()],
        contiguity=nvfuser.compute_contiguity(x.size(), x.stride()),
        dtype=torch_dtype_to_nvfuser_dtype(x.dtype),
    )


def norm_fusion_forward(
    fd: "nvfuser.FusionDefinition",
    inputs: List[torch.Tensor],
    x: "nvfuser.Tensor",
    weight: Optional["nvfuser.Tensor"],
    bias: Optional["nvfuser.Tensor"],
    running_mean: Optional["nvfuser.Tensor"],
    running_var: Optional["nvfuser.Tensor"],
    eps: "nvfuser.Scalar",
    use_input_stats: bool,
    momentum: "nvfuser.Scalar",
    channels_last: bool,
    x_datatype: "nvfuser.DataType",
    unbiased: bool = False,
    *,
    stat_axes: List[NamedAxis],
) -> Tuple["nvfuser.Tensor", "nvfuser.Tensor", "nvfuser.Tensor"]:
    """Modify FusionDefinition to add a generic normalization layer (forward).

    This can be used to construct a BatchNorm, GroupNorm, InstanceNorm, or
    LayerNorm network by indicating different sets of axes to preserve.

    BatchNorm: `stat_axes = [NamedAxis.CHANNEL]`
    LayerNorm: `stat_axes = [NamedAxis.BATCH]`
    InstanceNorm: `stat_axes = [NamedAxis.BATCH, NamedAxis.CHANNEL]`

    Args:
        fd: An initialized FusionDefinition.
        inputs: A list of :class:'torch.Tensor' inputs to the
            `FusionDefinition` `fd`.
        x: An input NVFuser tensor.
        weight: If given, multiply normed output by this `Tensor`. It should be
            one-dimensional if `NamedAxis.CHANNEL` is in `stat_axes`, and
            zero-dimensional otherwise. It will be broadcast along all other
            dimensions.
        bias: If given, add this `Tensor` to normed output. It should be
            one-dimensional if `NamedAxis.CHANNEL` is in `stat_axes`, and
            zero-dimensional otherwise. It will be broadcast along all other
            dimensions.
        running_mean: If given, a running mean estimate that will be modified
            in place.
        running_var: If given, a running variance estimate that will be
            modified in place.
        eps: Amount to regularize the square root needed to convert variance to
            standard deviation.
        use_input_stats: Whether to compute the stats of this batch or to
            _only_ use the provided running_mean and running_var.
        momentum: Momentum for exponentially weighted moving average of running
            stats.
        channels_last: Whether channels are in position -1 (`True`) or 1
            (`False`).
        x_datatype: :class:'DataType' of input :class:'Tensor' `x`
        unbiased: Whether to use unbiased variance for computing current batch
            statistics. Note that unbiased estimates are always used for
            running variance updates, regardless of this argument's value.
        stat_axes: A list of `NamedAxis` objects indicating a combination of
            axes with which to index the computed statistics. This can be used
            to implement multiple types of normalization layers, since most of
            those differ only in which axes are reduced over.
    Returns:
        The normalized output, as well as mean and 1/std. Note that
        `fd.add_output` is _not_ called by this function.
    """
    assert (running_var is None) == (
        running_mean is None
    ), "Iff running mean or var is given, the other should be"

    # dyn_shape holds Scalars describing the size of the input x
    dyn_shape = fd.ops.tensor_sizes(x)

    num_dims = len(dyn_shape)

    batch_dim = 0
    batch_size = dyn_shape[batch_dim]

    channel_dim = num_dims - 1 if channels_last else 1
    num_channels = dyn_shape[channel_dim]

    # Running stats will be kept possibly for channel but never by instance, so
    # we will reduce along batch_dim before updating running stats.
    # These are used to broadcast in spatial dims
    is_spatial_dim = [True] * num_dims
    is_spatial_or_batch_dim = [True] * num_dims

    if NamedAxis.BATCH in stat_axes:
        is_spatial_dim[batch_dim] = False
    if NamedAxis.CHANNEL in stat_axes:
        is_spatial_dim[channel_dim] = False
        is_spatial_or_batch_dim[channel_dim] = False
    x_reduction_axes = [ax for ax, flag in enumerate(is_spatial_dim) if flag]
    num_features = fd.define_scalar(1)
    for ax in x_reduction_axes:
        num_features = fd.ops.mul(num_features, dyn_shape[ax])

    if use_input_stats or running_mean is None:
        # In NVFuser Python we pass correction=1 to request unbiased variance calculation
        x_var, x_mean = fd.ops.var_mean(x, x_reduction_axes, int(unbiased))
        if running_mean is not None:
            one = fd.define_scalar(1.0)
            rev_momentum = fd.ops.sub(one, momentum)

            # do running mean with momentum
            current_mean_hat = fd.ops.mul(x_mean, momentum)
            mean_hat = fd.ops.mul(running_mean, rev_momentum)
            new_mean_hat = fd.ops.add(mean_hat, current_mean_hat)

            # If computing stats for each instance, we don't want to keep those
            # for our running mean calculation, so we sum them here
            new_mean_sum = (
                fd.ops.sum(new_mean_hat, [0])
                if NamedAxis.BATCH in stat_axes
                else new_mean_hat
            )

            rev_batch_size = fd.ops.reciprocal(batch_size)
            new_mean_channels_only = fd.ops.mul(new_mean_sum, rev_batch_size)
            if x_datatype in [nvfuser.DataType.Half, nvfuser.DataType.BFloat16]:
                new_mean_channels_only = fd.ops.cast(new_mean_channels_only, x_datatype)
            fd.add_output(new_mean_channels_only, alias_input=running_mean)

            # running var calculation
            x_var_unbiased = x_var
            if not unbiased:
                # multiply by correction to go from biased to unbiased estimate
                b2ub = fd.ops.div(
                    num_features, fd.ops.sub(num_features, fd.define_scalar(1))
                )
                x_var_unbiased = fd.ops.mul(x_var, b2ub)

            current_var_hat = fd.ops.mul(x_var_unbiased, momentum)
            var_hat = fd.ops.mul(running_var, rev_momentum)
            new_var_hat = fd.ops.add(var_hat, current_var_hat)

            # See above about reducing over batch dim for running stats
            new_var_sum = (
                fd.ops.sum(new_var_hat, [0])
                if NamedAxis.BATCH in stat_axes
                else new_var_hat
            )

            new_var_channels_only = fd.ops.mul(new_var_sum, rev_batch_size)
            if x_datatype in [nvfuser.DataType.Half, nvfuser.DataType.BFloat16]:
                new_var_channels_only = fd.ops.cast(new_var_channels_only, x_datatype)
            fd.add_output(new_var_channels_only, alias_input=running_var)

        mean = x_mean
        mean_bcast = fd.ops.broadcast(mean, is_spatial_dim)
        x_sub_mean = fd.ops.sub(x, mean_bcast)

        var_eps = fd.ops.add(x_var, eps)
        invstd = fd.ops.rsqrt(var_eps)
        invstd_bcast = fd.ops.broadcast(invstd, is_spatial_dim)

        x_normed = fd.ops.mul(x_sub_mean, invstd_bcast)

    else:  # This is inference mode with running stats
        assert running_mean is not None
        r_mean_bcast = fd.ops.broadcast(running_mean, is_spatial_or_batch_dim)
        x_sub_mean = fd.ops.sub(x, r_mean_bcast)

        var_eps = fd.ops.add(running_var, eps)
        invstd = fd.ops.rsqrt(var_eps)
        invstd_bcast = fd.ops.broadcast(invstd, is_spatial_or_batch_dim)

        mean = running_mean
        x_normed = fd.ops.mul(x_sub_mean, invstd_bcast)

    if weight is not None:
        weight_bcast = fd.ops.broadcast(weight, is_spatial_or_batch_dim)
        x_normed = fd.ops.mul(x_normed, weight_bcast)
    if bias is not None:
        bias_bcast = fd.ops.broadcast(bias, is_spatial_or_batch_dim)
        x_normed = fd.ops.add(x_normed, bias_bcast)

    return x_normed, mean, invstd


def norm_fusion_backward(
    fd: "nvfuser.FusionDefinition",
    inputs: List[torch.Tensor],
    x: "nvfuser.Tensor",
    grad_output: "nvfuser.Tensor",
    mean: Optional[torch.Tensor],
    invstd: torch.Tensor,
    weight: Optional["nvfuser.Tensor"],
    bias: Optional["nvfuser.Tensor"],
    running_mean: Optional["nvfuser.Tensor"],
    running_var: Optional["nvfuser.Tensor"],
    use_input_stats: bool,
    channels_last: bool,
    x_datatype: "nvfuser.DataType",
    *,
    stat_axes: List[NamedAxis],
) -> Tuple["nvfuser.Tensor", "nvfuser.Tensor", "nvfuser.Tensor"]:
    """
    Modify FusionDefinition to add a generic normalization layer (backward).

    Args:
        fd: An initialized FusionDefinition.
        inputs: A list of :class:'torch.Tensor' inputs to the
            `FusionDefinition` `fd`.
        x: The input NVFuser tensor.
        grad_output: NVFuser tensor representing gradient of loss with respect
            to downstream activation (typical input to backward()).
        mean: The mean used in the forward normalization.
        invstd: The reciprocal of standard deviation used in the forward normalization.
        weight: If given, multiply normed output by this `Tensor`. It should be
            one-dimensional if `NamedAxis.CHANNEL` is in `stat_axes`, and
            zero-dimensional otherwise. It will be broadcast along all other
            dimensions.
        bias: If given, add this `Tensor` to normed output. It should be
            one-dimensional if `NamedAxis.CHANNEL` is in `stat_axes`, and
            zero-dimensional otherwise. It will be broadcast along all other
            dimensions.
        running_mean: If given, a running mean estimate that will be modified
            in place.
        running_var: If given, a running variance estimate that will be
            modified in place.
        use_input_stats: Whether to compute the stats of this batch or to
            _only_ use the provided running_mean and running_var.
        channels_last: Whether channels are in position -1 (`True`) or 1
            (`False`).
        x_datatype: :class:'DataType' of input :class:'Tensor' `x`
        stat_axes: A list of `NamedAxis` objects indicating a combination of
            axes with which to index the computed statistics. This can be used
            to implement multiple types of normalization layers, since most of
            those differ only in which axes are reduced over.
    Returns:
        The normalized output, as well as mean and 1/std. Note that
        `fd.add_output` is _not_ called by this function.
    """
    assert not (
        (running_var is None) ^ (running_mean is None)
    ), "Iff running mean or var is given, the other should be"

    # dyn_shape holds Scalars describing the size of the input x
    dyn_shape = fd.ops.tensor_sizes(x)

    num_dims = len(dyn_shape)

    batch_dim = 0
    batch_size = dyn_shape[batch_dim]

    channel_dim = num_dims - 1 if channels_last else 1
    num_channels = dyn_shape[channel_dim]

    # Running stats will be kept possibly for channel but never by instance, so
    # we will reduce along batch_dim before updating running stats.
    # These are used to broadcast in spatial dims
    is_spatial_dim = [True] * num_dims
    is_spatial_or_batch_dim = [True] * num_dims

    if NamedAxis.BATCH in stat_axes:
        is_spatial_dim[batch_dim] = False
    if NamedAxis.CHANNEL in stat_axes:
        is_spatial_dim[channel_dim] = False
        is_spatial_or_batch_dim[channel_dim] = False
    x_reduction_axes = [ax for ax, flag in enumerate(is_spatial_dim) if flag]
    num_features = fd.define_scalar(1)
    for ax in x_reduction_axes:
        num_features = fd.ops.mul(num_features, dyn_shape[ax])

    mean = fd.ops.broadcast(mean, is_spatial_dim)

    norm = fd.ops.reciprocal(num_features)
    grad_output_sum = fd.ops.sum(grad_output, x_reduction_axes)
    dot_p = fd.ops.sum(
        fd.ops.mul(
            grad_output,
            fd.ops.sub(x, mean),
        ),
        x_reduction_axes,
    )
    grad_mean = fd.ops.broadcast(fd.ops.mul(grad_output_sum, norm), is_spatial_dim)
    proj_scale = fd.ops.broadcast(
        fd.ops.mul(
            fd.ops.mul(dot_p, norm),
            fd.ops.mul(invstd, invstd),
        ),
        is_spatial_dim,
    )

    invstd_bcast = fd.ops.broadcast(invstd, is_spatial_dim)
    grad_scale = (
        invstd_bcast
        if weight is None
        else fd.ops.mul(
            invstd_bcast,
            fd.ops.broadcast(weight, is_spatial_or_batch_dim),
        )
    )
    if use_input_stats:
        proj = fd.ops.mul(fd.ops.sub(x, mean), proj_scale)
        grad_input = fd.ops.mul(
            fd.ops.sub(
                fd.ops.sub(grad_output, proj),
                grad_mean,
            ),
            grad_scale,
        )
    else:
        grad_input = fd.ops.mul(grad_output, grad_scale)

    if weight is not None:
        grad_weight = fd.ops.mul(dot_p, invstd)
        grad_weight_reduced = fd.ops.sum(grad_weight, [0])
    else:
        grad_weight_reduced = None
    if bias is not None:
        grad_bias = grad_output_sum
        grad_bias_reduced = fd.ops.sum(grad_bias, [0])
    else:
        grad_bias_reduced = None

    return grad_input, grad_weight_reduced, grad_bias_reduced


class NormNVFuserFunction(torch.autograd.Function):
    @staticmethod
    def forward(
        ctx: Any,  # contexts are actually objects of the type we are currently defining
        x: torch.Tensor,
        weight: Optional[torch.Tensor],
        bias: Optional[torch.Tensor],
        running_mean: Optional[torch.Tensor],
        running_var: Optional[torch.Tensor],
        use_input_stats: bool,
        momentum: float,
        eps: float,
        unbiased: bool,
        stat_axes: List[NamedAxis],
    ) -> torch.Tensor:
        # When x.shape[1] == 1, is_contiguous will tell us the tensor is
        # channels_last, even when it is ordinary contiguous. This causes some
        # issues so we only detect channels_last when channels > 1
        channels_last = x.shape[1] > 1 and (
            x.is_contiguous(memory_format=torch.channels_last)
            or x.is_contiguous(memory_format=torch.channels_last_3d)
        )
        xorig = x
        if channels_last:
            order = [0] + list(range(2, len(x.shape))) + [1]
            x = x.permute(order)

        x_datatype = torch_dtype_to_nvfuser_dtype(x.dtype)

        with nvfuser.FusionDefinition() as fd:
            tv_x = partially_contig_tensor(fd, x)
            inputs = [x]
            if weight is not None:
                tv_weight = partially_contig_tensor(fd, weight)
                inputs.append(weight)
            else:
                tv_weight = None

            if bias is not None:
                tv_bias = partially_contig_tensor(fd, bias)
                inputs.append(bias)
            else:
                tv_bias = None

            if running_mean is None:
                tv_running_mean = None
                tv_running_var = None
            else:
                assert running_var is not None
                tv_running_mean = partially_contig_tensor(fd, running_mean)
                tv_running_var = partially_contig_tensor(fd, running_var)
                inputs.extend([running_mean, running_var])

            s_momentum = fd.define_scalar(nvfuser.DataType.Double)
            s_eps = fd.define_scalar(nvfuser.DataType.Double)
            inputs.extend([momentum, eps])

            # cast inputs if necessary
            if x_datatype in [nvfuser.DataType.Half, nvfuser.DataType.BFloat16]:
                tv_x = fd.ops.cast(tv_x, nvfuser.DataType.Float)
            if weight is not None and weight.dtype in [torch.half, torch.bfloat16]:
                tv_weight = fd.ops.cast(tv_weight, nvfuser.DataType.Float)
            if bias is not None and bias.dtype in [torch.half, torch.bfloat16]:
                tv_bias = fd.ops.cast(tv_bias, nvfuser.DataType.Float)

            out, mean, invstd = norm_fusion_forward(
                fd,
                inputs,
                tv_x,
                tv_weight,
                tv_bias,
                tv_running_mean,
                tv_running_var,
                s_eps,
                use_input_stats,
                s_momentum,
                channels_last,
                x_datatype=x_datatype,
                unbiased=unbiased,
                stat_axes=stat_axes,
            )

            if x_datatype in [nvfuser.DataType.Half, nvfuser.DataType.BFloat16]:
                out = fd.ops.cast(out, x_datatype)

            fd.add_output(out)
            fd.add_output(mean)
            fd.add_output(invstd)

        out, mean, invstd = fd.execute(inputs)

        ctx.stat_axes = stat_axes
        ctx.use_input_stats = use_input_stats
        ctx.channels_last = channels_last
        # saving for backward in "explicit channels-last format"
        ctx.save_for_backward(x, weight, bias, running_mean, running_var, mean, invstd)
        if channels_last:
            order = [0, len(x.shape) - 1] + list(range(1, len(x.shape) - 1))
            out = out.permute(order)
            if len(out.shape) == 4:
                assert out.is_contiguous(memory_format=torch.channels_last)
                assert xorig.is_contiguous(memory_format=torch.channels_last)
            elif len(out.shape) == 5:
                assert out.is_contiguous(memory_format=torch.channels_last_3d)
                assert xorig.is_contiguous(memory_format=torch.channels_last_3d)
            else:
                raise RuntimeError(
                    "unhandled channels_last format variation in forward"
                )
        return out

    @staticmethod
    def backward(
        ctx: Any, grad_output: torch.Tensor
    ) -> Tuple[
        torch.Tensor,
        torch.Tensor,
        torch.Tensor,
        None,
        None,
        None,
        None,
        None,
        None,
        None,
    ]:
        """Instance norm backward using NVFuser"""
        if ctx.channels_last:
            order = [0] + list(range(2, len(grad_output.shape))) + [1]
            grad_output = grad_output.permute(order)
        # input was saved in "explicit channels-last format"
        # assert ctx.saved_tensors[0].is_contiguous()
        # grad_output = grad_output.contiguous()
        x, weight, bias, running_mean, running_var, mean, invstd = ctx.saved_tensors

        with nvfuser.FusionDefinition() as fd:
            tv_x = partially_contig_tensor(fd, x)
            if x.dtype in [torch.half, torch.bfloat16]:
                tv_x = fd.ops.cast(tv_x, nvfuser.DataType.Float)
            inputs = [x]
            if weight is not None:
                tv_weight = partially_contig_tensor(fd, weight)
                if weight.dtype in [torch.half, torch.bfloat16]:
                    tv_weight = fd.ops.cast(tv_weight, nvfuser.DataType.Float)
                inputs.append(weight)
            else:
                tv_weight = None
            if bias is not None:
                tv_bias = partially_contig_tensor(fd, bias)
                if bias.dtype in [torch.half, torch.bfloat16]:
                    tv_bias = fd.ops.cast(tv_bias, nvfuser.DataType.Float)
                inputs.append(bias)
            else:
                tv_bias = None
            if running_mean is not None:
                tv_running_mean = partially_contig_tensor(fd, running_mean)
                if running_mean.dtype in [torch.half, torch.bfloat16]:
                    tv_running_mean = fd.ops.cast(
                        tv_running_mean, nvfuser.DataType.Float
                    )
                inputs.append(running_mean)
            else:
                tv_running_mean = None
            if running_var is not None:
                tv_running_var = partially_contig_tensor(fd, running_var)
                if running_var.dtype in [torch.half, torch.bfloat16]:
                    tv_running_var = fd.ops.cast(tv_running_var, nvfuser.DataType.Float)
                inputs.append(running_var)
            else:
                tv_running_var = None

            tv_mean = partially_contig_tensor(fd, mean)
            if mean.dtype in [torch.half, torch.bfloat16]:
                tv_mean = fd.ops.cast(tv_mean, nvfuser.DataType.Float)
            inputs.append(mean)
            tv_invstd = partially_contig_tensor(fd, invstd)
            if invstd.dtype in [torch.half, torch.bfloat16]:
                tv_invstd = fd.ops.cast(tv_invstd, nvfuser.DataType.Float)
            inputs.append(invstd)

            tv_grad_output = partially_contig_tensor(fd, grad_output)
            if grad_output.dtype in [torch.half, torch.bfloat16]:
                tv_grad_output = fd.ops.cast(tv_grad_output, nvfuser.DataType.Float)
            inputs.append(grad_output)

            x_datatype = torch_dtype_to_nvfuser_dtype(x.dtype)

            grad_input, grad_weight, grad_bias = norm_fusion_backward(
                fd,
                inputs,
                tv_x,
                tv_grad_output,
                tv_mean,
                tv_invstd,
                tv_weight,
                tv_bias,
                tv_running_mean,
                tv_running_var,
                ctx.use_input_stats,
                ctx.channels_last,
                x_datatype=x_datatype,
                stat_axes=ctx.stat_axes,
            )

            if x_datatype in [nvfuser.DataType.Half, nvfuser.DataType.BFloat16]:
                grad_input = fd.ops.cast(grad_input, x_datatype)
            fd.add_output(grad_input)

            if weight is not None:
                if x_datatype in [nvfuser.DataType.Half, nvfuser.DataType.BFloat16]:
                    grad_weight = fd.ops.cast(grad_weight, x_datatype)
                fd.add_output(grad_weight)

            if bias is not None:
                if x_datatype in [nvfuser.DataType.Half, nvfuser.DataType.BFloat16]:
                    grad_bias = fd.ops.cast(grad_bias, x_datatype)
                fd.add_output(grad_bias)

        res = fd.execute(inputs)
        grad_input = res[0]
        c = 1
        if weight is not None:
            grad_weight = res[c]
            c += 1
        else:
            grad_weight = None
        if bias is not None:
            grad_bias = res[c]
            c += 1
        else:
            grad_bias = None

        if ctx.channels_last:
            order = [0, len(grad_input.shape) - 1] + list(
                range(1, len(grad_input.shape) - 1)
            )
            grad_input = grad_input.permute(order)
            if len(grad_input.shape) == 4:
                assert grad_input.is_contiguous(memory_format=torch.channels_last)
            elif len(grad_input.shape) == 5:
                assert grad_input.is_contiguous(memory_format=torch.channels_last_3d)
            else:
                raise RuntimeError(
                    "unhandled channels_last format variation in backward"
                )
        return (
            grad_input,
            grad_weight,
            grad_bias,
            None,
            None,
            None,
            None,
            None,
            None,
            None,
        )


class _NormNVFuserBase(torch.nn.modules.batchnorm._NormBase):
    stat_axes: Optional[List[NamedAxis]] = None

    def __init__(
        self,
        num_features: int,
        eps: float = 1e-5,
        momentum: float = 0.1,
        affine: bool = False,
        track_running_stats: bool = False,
        device: torch.device = None,
        dtype: torch.dtype = None,
    ) -> None:
        factory_kwargs = {"device": device, "dtype": dtype}
        super().__init__(
            num_features, eps, momentum, affine, track_running_stats, **factory_kwargs
        )

    def _check_input_dim(self, input: torch.Tensor) -> None:
        raise NotImplementedError

    def _load_from_state_dict(
        self,
        state_dict: Dict[str, Any],
        prefix: str,
        local_metadata: Any,
        strict: bool,
        missing_keys: List[str],
        unexpected_keys: List[str],
        error_msgs: List[str],
    ) -> None:
        version = local_metadata.get("version", None)
        # at version 1: removed running_mean and running_var when
        # track_running_stats=False (default)
        if version is None and not self.track_running_stats:
            running_stats_keys = []
            for name in ("running_mean", "running_var"):
                key = prefix + name
                if key in state_dict:
                    running_stats_keys.append(key)
            if len(running_stats_keys) > 0:
                error_msgs.append(
                    "Unexpected running stats buffer(s) {names} for {klass} "
                    "with track_running_stats=False. If state_dict is a "
                    "checkpoint saved before 0.4.0, this may be expected "
                    "because {klass} does not track running stats by default "
                    "since 0.4.0. Please remove these keys from state_dict. If "
                    "the running stats are actually needed, instead set "
                    "track_running_stats=True in {klass} to enable them. See "
                    "the documentation of {klass} for details.".format(
                        names=" and ".join(
                            '"{}"'.format(k) for k in running_stats_keys
                        ),
                        klass=self.__class__.__name__,
                    )
                )
                for key in running_stats_keys:
                    state_dict.pop(key)

        super()._load_from_state_dict(
            state_dict,
            prefix,
            local_metadata,
            strict,
            missing_keys,
            unexpected_keys,
            error_msgs,
        )

    def forward(self, input: nvfuser.Tensor) -> nvfuser.Tensor:
        assert input.is_cuda, "NVFuser InstanceNorm is CUDA only"
        self._check_input_dim(input)
        out = NormNVFuserFunction.apply(
            input,
            self.weight,
            self.bias,
            self.running_mean,
            self.running_var,
            self.training or not self.track_running_stats,
            self.momentum,
            self.eps,
            False,  # unbiased=False to match PyTorch functionality
            self.stat_axes,
        )
        return out


class _InstanceNormNVFuser(_NormNVFuserBase):
    stat_axes = [NamedAxis.BATCH, NamedAxis.CHANNEL]


class _BatchNormNVFuser(_NormNVFuserBase):
    stat_axes = [NamedAxis.CHANNEL]


class _LayerNormNVFuser(_NormNVFuserBase):
    stat_axes = [NamedAxis.BATCH]


class InstanceNorm1dNVFuser(_InstanceNormNVFuser):
    def _check_input_dim(self, input: torch.Tensor) -> None:
        if input.dim() != 3:
            raise ValueError("expected 3D input (got {}D input)".format(input.dim()))


class InstanceNorm2dNVFuser(_InstanceNormNVFuser):
    def _check_input_dim(self, input: torch.Tensor) -> None:
        if input.dim() != 4:
            raise ValueError("expected 4D input (got {}D input)".format(input.dim()))


class InstanceNorm3dNVFuser(_InstanceNormNVFuser):
    def _check_input_dim(self, input: torch.Tensor) -> None:
        if input.dim() != 5:
            raise ValueError("expected 5D input (got {}D input)".format(input.dim()))
