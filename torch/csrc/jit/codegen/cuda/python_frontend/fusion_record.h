#pragma once
#include <c10/util/complex.h>
#include <torch/csrc/jit/codegen/cuda/arith.h>
#include <torch/csrc/jit/codegen/cuda/ops/normalization.h>
#include <torch/csrc/jit/codegen/cuda/python_frontend/fusion_definition.h>

namespace nvfuser {

//! RecordFunctor is the base class record for operations recorded by
//! the FusionDefinition.  It is, in essence, a node in the graph with
//! input edges, args, and outputs edges outputs that where the stored
//! values are indices into the recorded state.
//!
//! The virual functor is the operators that is replayed on a cache
//! to build the appropriate part of the nvFuser Fusion IR for a given
//! record.

struct RecordFunctor {
  RecordFunctor(std::vector<size_t> _args, std::vector<size_t> _outputs)
      : args(std::move(_args)), outputs(std::move(_outputs)) {}
  virtual ~RecordFunctor() = default;

  //! The base virtual equality operator is defined so all child
  //! classes can utilize the check for the same args and outputs.
  virtual bool operator==(const RecordFunctor& other) const {
    auto result = (args.size() == other.args.size()) &&
        (outputs.size() == other.outputs.size());
    if (result) {
      for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] != other.args[i]) {
          result = false;
          break;
        }
      }
    }
    if (result) {
      for (size_t i = 0; i < outputs.size(); ++i) {
        if (outputs[i] != other.outputs[i]) {
          result = false;
          break;
        }
      }
    }
    return result;
  }

  //! Abstraction for an operation to build this record's nvFuser Fusion IR
  //! piece if the recording has a cache miss.
  virtual void operator()(FusionDefinition& fd) = 0;

  //! Inputs that are indices into the FusionDefinition's Recorded State.
  std::vector<size_t> args;
  //! Outputs that are indices into the FusionDefinition's Recorded State.
  std::vector<size_t> outputs;
};

//! The OpRecord RecordFunctor is the most widely used child class because
//! it utilizes varidiac template arguments to represent unary, binary,
//! ternary, and other similar flavors of operations in nvFuser that have
//! a mix of Tensor and Scalar arguments only.
//!
//! The additional data memeber of this child class records the function
//! signature of the nvFuser Arith Operation to be replayed upon a cache
//! miss by the functor operator() call.

template <class OutType, class... ArgTypes>
struct OpRecord : RecordFunctor {
  OpRecord(
      std::vector<size_t> _args,
      std::vector<size_t> _outputs,
      std::function<OutType(ArgTypes...)> fusion_op)
      : RecordFunctor(std::move(_args), std::move(_outputs)),
        fusion_op_(fusion_op) {}
  virtual ~OpRecord() = default;

  virtual bool operator==(const RecordFunctor& other) const final {
    auto result = false;
    // A succesfull cast indicates a RecordFunctor of the same child class
    if (auto child_ptr = dynamic_cast<const OpRecord*>(&other)) {
      result = RecordFunctor::operator==(other);
      if (result) {
        // Match the nvFuser arith function types
        result = result &&
            (fusion_op_.target_type() == child_ptr->fusion_op_.target_type());
        // Match the nvFuser arith function pointers
        result = result &&
            (fusion_op_.template target<OutType (*)(ArgTypes...)>() ==
             child_ptr->fusion_op_.template target<OutType (*)(ArgTypes...)>());
      }
    }
    return result;
  }

  //! The variadic set of indices for the number of args for this op are
  //! deduced by providing the index_sequence as a parameter.  Similarly,
  //! the tuple type is also deduced.
  //!
  //! The tuple type is used to decide whether to cast the input argument
  //! to a Fusion IR TensorView or leave it as a Fusion IR Val (Scalar).
  //!
  //! A deduced binary op could look like:
  //!   OutType opFunc<std::tuple<NvfTensor*, NvfTensor*>, 0, 1>
  //! A deduced ternary op could look like:
  //!   OutTupe opFunc<std::tuple<NvfTensor*, NvfVal*, NvfVal*>, 0, 1, 2>
  template <class TupleType, std::size_t... Is>
  OutType opFunc(
      FusionDefinition& fd,
      TupleType& tp,
      std::index_sequence<Is...>) {
    return fusion_op_(
        dynamic_cast<typename std::tuple_element<Is, TupleType>::type>(
            fd.getFusionState(args.at(Is)))...);
  }

  void operator()(FusionDefinition& fd) final {
    using arg_tuple_t = std::tuple<ArgTypes...>;
    auto indices =
        std::make_index_sequence<std::tuple_size<arg_tuple_t>::value>();
    // The tuple variable is never populated, it is passed for its type.
    arg_tuple_t inputs;
    auto output = opFunc(fd, inputs, indices);
    fd.setFusionState(outputs.at(0), output);
  }

 private:
  //! An nvFuser Arith Operation function signature
  std::function<OutType(ArgTypes...)> fusion_op_;
};

//! Specialized Record Functor for the FusionDefinition's broadcast_in_dim op.

struct BroadcastOpRecord : RecordFunctor {
  BroadcastOpRecord(
      std::vector<size_t> _args,
      std::vector<size_t> _outputs,
      std::vector<int64_t>& output_shape,
      std::vector<int64_t>& broadcast_dims)
      : RecordFunctor(std::move(_args), std::move(_outputs)),
        output_shape_(std::move(output_shape)),
        broadcast_dims_(std::move(broadcast_dims)) {}
  virtual ~BroadcastOpRecord() = default;

  virtual bool operator==(const RecordFunctor& other) const final {
    auto result = false;
    if (auto child_ptr = dynamic_cast<const BroadcastOpRecord*>(&other)) {
      result = RecordFunctor::operator==(other);
      if (result) {
        result =
            ((output_shape_.size() == child_ptr->output_shape_.size()) &&
             (broadcast_dims_.size() == child_ptr->broadcast_dims_.size()));
        if (result) {
          for (size_t i = 0; i < output_shape_.size(); ++i) {
            if (output_shape_[i] != child_ptr->output_shape_[i]) {
              result = false;
              break;
            }
          }
        }
        if (result) {
          for (size_t i = 0; i < broadcast_dims_.size(); ++i) {
            if (broadcast_dims_[i] != child_ptr->broadcast_dims_[i]) {
              result = false;
              break;
            }
          }
        }
      }
    }
    return result;
  }

  void operator()(FusionDefinition& fd) final {
    auto arg = fd.getFusionState(args.at(0))->template as<TensorView>();

    const auto arg_ndims = arg->domain()->noReductions().size();
    TORCH_CHECK(
        output_shape_.size() >= arg_ndims,
        "The new shape is expected to be greater-then-or-equal to the input",
        output_shape_.size(),
        arg_ndims);
    TORCH_CHECK(
        arg_ndims == broadcast_dims_.size(),
        "The broadcast dimensions should match the input dimensions.",
        arg_ndims,
        broadcast_dims_.size());

    std::vector<bool> is_broadcast_dim(output_shape_.size(), true);
    for (const auto idx : c10::irange(broadcast_dims_.size())) {
      if (idx > 0) {
        TORCH_CHECK(
            broadcast_dims_[idx - 1] < broadcast_dims_[idx],
            "Broadcast dimension is not greater than the previous value.");
      }
      TORCH_CHECK(
          broadcast_dims_[idx] < static_cast<int>(output_shape_.size()),
          "Invalid broadcast_dims value.");
      is_broadcast_dim.at(broadcast_dims_[idx]) = false;
    }

    auto output = torch::jit::fuser::cuda::broadcast(arg, is_broadcast_dim);
    fd.setFusionState(outputs.at(0), output);
  }

 private:
  //! Represents the tensor dimensions of the output tensor.
  std::vector<int64_t> output_shape_;
  //! Communicates which dimensions of the output the input tensor maps.
  //! For instance, for output [2, 3, 4] and input [3]. This vector would
  //! contain [1].
  std::vector<int64_t> broadcast_dims_;
};

template <class OutType, class ArgType>
struct CastOpRecord : RecordFunctor {
  CastOpRecord(
      std::vector<size_t> _args,
      std::vector<size_t> _outputs,
      std::function<OutType(NvfDataType, ArgType)> fusion_op,
      NvfDataType dtype)
      : RecordFunctor(std::move(_args), std::move(_outputs)),
        fusion_op_(fusion_op),
        dtype_(dtype) {}
  virtual ~CastOpRecord() = default;

  virtual bool operator==(const RecordFunctor& other) const final {
    auto result = false;
    if (auto child_ptr = dynamic_cast<const CastOpRecord*>(&other)) {
      result = RecordFunctor::operator==(other);
      if (result) {
        result = result &&
            (fusion_op_.target_type() == child_ptr->fusion_op_.target_type());
        result = result &&
            (fusion_op_.template target<OutType (*)(NvfDataType, ArgType)>() ==
             child_ptr->fusion_op_
                 .template target<OutType (*)(NvfDataType, ArgType)>());
        result = result && (dtype_ == child_ptr->dtype_);
      }
    }
    return result;
  }

  void operator()(FusionDefinition& fd) final {
    auto arg = dynamic_cast<ArgType>(fd.getFusionState(args.at(0)));
    auto output = fusion_op_(dtype_, arg);
    fd.setFusionState(outputs.at(0), output);
  }

 private:
  //! nvFuser arith function signature
  std::function<OutType(NvfDataType, ArgType)> fusion_op_;
  //! Type to cast to.
  NvfDataType dtype_;
};

//! Specialized Record Functor for recording FusionDefinition constant state.

template <typename ExprType, typename ValueType>
struct ConstantRecord : RecordFunctor {
  ConstantRecord(std::vector<size_t> _outputs, ValueType val)
      : RecordFunctor({}, std::move(_outputs)), value_(val) {}
  virtual ~ConstantRecord() = default;

  virtual bool operator==(const RecordFunctor& other) const final {
    auto result = false;
    if (auto child_ptr = dynamic_cast<const ConstantRecord*>(&other)) {
      result = RecordFunctor::operator==(other);
      result = result && (value_ == child_ptr->value_);
    }
    return result;
  }

  void operator()(FusionDefinition& fd) final {
    NvfVal* output = IrBuilder::create<ExprType>(value_);
    fd.setFusionState(outputs.at(0), output);
  }

 private:
  //! The constants literal value.
  ValueType value_;
};

//! Specialized Record Functor for recording FusionDefinition input tensors.

struct InputTensorRecord : RecordFunctor {
  InputTensorRecord(
      std::vector<size_t> _outputs,
      std::vector<int64_t> _symbolic_sizes,
      std::vector<bool> _contiguous_info,
      NvfDataType _dtype)
      : RecordFunctor({}, std::move(_outputs)),
        symbolic_sizes_(std::move(_symbolic_sizes)),
        contiguous_info_(std::move(_contiguous_info)),
        dtype_(_dtype) {}
  virtual ~InputTensorRecord() = default;

  virtual bool operator==(const RecordFunctor& other) const final {
    auto result = false;
    if (auto child_ptr = dynamic_cast<const InputTensorRecord*>(&other)) {
      result = RecordFunctor::operator==(other);
      result = result && (dtype_ == child_ptr->dtype_);
      if (result) {
        result =
            ((symbolic_sizes_.size() == child_ptr->symbolic_sizes_.size()) &&
             (contiguous_info_.size() == child_ptr->contiguous_info_.size()));
        if (result) {
          for (size_t i = 0; i < symbolic_sizes_.size(); ++i) {
            if (symbolic_sizes_[i] != child_ptr->symbolic_sizes_[i]) {
              result = false;
              break;
            }
          }
        }
        if (result) {
          for (size_t i = 0; i < contiguous_info_.size(); ++i) {
            if (contiguous_info_[i] != child_ptr->contiguous_info_[i]) {
              result = false;
              break;
            }
          }
        }
      }
    }
    return result;
  }

  void operator()(FusionDefinition& fd) final {
    auto tv = TensorViewBuilder()
                  .ndims(symbolic_sizes_.size())
                  .contiguity(contiguous_info_)
                  .shape(symbolic_sizes_)
                  .dtype(dtype_)
                  .build();

    fd.setFusionState(outputs.at(0), tv);
    fd.addInput(tv);
  }

 private:
  //! A vector of tensor dimension sizes.
  //! This vector only captures sizes of -1 or 1 to indicate a symbolic
  //! dimension (-1) or a broadcast dimension (1).
  std::vector<int64_t> symbolic_sizes_;
  //! A vector to indicate whether the a tensor dimension is contiguous
  //! with the dimension just to its right.
  std::vector<bool> contiguous_info_;
  //! Tensor data type.
  NvfDataType dtype_;
};

//! Specialized Record Functor for recording FusionDefinition outputs.

template <class OutputType>
struct OutputRecord : RecordFunctor {
  OutputRecord(std::vector<size_t> _args)
      : RecordFunctor(std::move(_args), {}) {}
  virtual ~OutputRecord() = default;

  virtual bool operator==(const RecordFunctor& other) const final {
    auto result = false;
    if (auto child_ptr = dynamic_cast<const OutputRecord*>(&other)) {
      result = RecordFunctor::operator==(other);
    }
    return result;
  }

  void operator()(FusionDefinition& fd) final {
    auto input = fd.getFusionState(args.at(0));

    // With C++17, this statement should be "if constexpr"
    if (std::is_same<OutputType, NvfTensorView>::value) {
      fd.addOutput(input->template as<NvfTensorView>());
    } else {
      fd.addOutput(input);
    }
  }
};

//! Specialized Record Functor for the FusionDefinition's sum/min/max ops.

struct ReductionOpRecord : RecordFunctor {
  ReductionOpRecord(
      std::vector<size_t> _args,
      std::vector<size_t> _outputs,
      std::function<
          NvfTensorView*(NvfTensorView*, std::vector<int>&, bool, NvfDataType)>
          fusion_op,
      std::vector<int> axes,
      bool keep_dim,
      NvfDataType dtype)
      : RecordFunctor(std::move(_args), std::move(_outputs)),
        fusion_op_(fusion_op),
        axes_(std::move(axes)),
        keep_dim_(keep_dim),
        dtype_(dtype) {}
  virtual ~ReductionOpRecord() = default;

  virtual bool operator==(const RecordFunctor& other) const final {
    auto result = false;
    if (auto child_ptr = dynamic_cast<const ReductionOpRecord*>(&other)) {
      result = RecordFunctor::operator==(other);
      if (result) {
        result = result &&
            (fusion_op_.target_type() == child_ptr->fusion_op_.target_type());
        result = result &&
            (fusion_op_.template target<
                 NvfTensorView* (*)(NvfTensorView*, std::vector<int>&, bool, NvfDataType)>() ==
             child_ptr->fusion_op_.template target<
                 NvfTensorView* (*)(NvfTensorView*, std::vector<int>&, bool, NvfDataType)>());
        result = result && (keep_dim_ == child_ptr->keep_dim_);
        result = result && (dtype_ == child_ptr->dtype_);
        if (result) {
          result = (axes_.size() == child_ptr->axes_.size());
          if (result) {
            for (size_t i = 0; i < axes_.size(); ++i) {
              if (axes_[i] != child_ptr->axes_[i]) {
                result = false;
                break;
              }
            }
          }
        }
      }
    }
    return result;
  }

  void operator()(FusionDefinition& fd) final {
    auto arg = fd.getFusionState(args.at(0))->template as<NvfTensorView>();
    auto output = fusion_op_(arg, axes_, keep_dim_, dtype_);
    fd.setFusionState(outputs.at(0), output);
  }

 private:
  //! nvFuser arith function signature for a given reduction operation
  std::function<
      NvfTensorView*(NvfTensorView*, std::vector<int>&, bool, NvfDataType)>
      fusion_op_;
  //! The tensor dimensions to reduce
  std::vector<int> axes_;
  //! Indicates whether to keep the reduced dimension(s).
  bool keep_dim_;
  //! The output data type.
  NvfDataType dtype_;
};

//! Specialized Record Functor for recording FusionDefinition input scalars.

struct ScalarRecord : RecordFunctor {
  ScalarRecord(std::vector<size_t> _outputs, NvfDataType dtype)
      : RecordFunctor({}, std::move(_outputs)), dtype_(dtype) {}
  virtual ~ScalarRecord() = default;

  virtual bool operator==(const RecordFunctor& other) const final {
    auto result = false;
    if (auto child_ptr = dynamic_cast<const ScalarRecord*>(&other)) {
      result = RecordFunctor::operator==(other);
      result = result && (dtype_ == child_ptr->dtype_);
    }
    return result;
  }

  void operator()(FusionDefinition& fd) final {
    NvfVal* output = nullptr;
    if (dtype_ == NvfDataType::Double) {
      output = IrBuilder::create<torch::jit::fuser::cuda::Double>();
    } else if (dtype_ == NvfDataType::ComplexDouble) {
      output = IrBuilder::create<torch::jit::fuser::cuda::ComplexDouble>();
    } else if (dtype_ == NvfDataType::Bool) {
      output = IrBuilder::create<torch::jit::fuser::cuda::Bool>();
    } else if (dtype_ == NvfDataType::Int) {
      output = IrBuilder::create<torch::jit::fuser::cuda::Int>();
    } else {
      TORCH_CHECK(false, "Dtype is not supported:", dtype_);
    }
    fd.addInput(output);
    fd.setFusionState(outputs.at(0), output);
  }

 private:
  //! Scalar data type.
  NvfDataType dtype_;
};

//! Specialized Record Functor for the FusionDefinition's var op.

struct VarianceOpRecord : RecordFunctor {
  VarianceOpRecord(
      std::vector<size_t> _args,
      std::vector<size_t> _outputs,
      std::vector<int>& axes,
      int64_t correction,
      bool keep_dim)
      : RecordFunctor(std::move(_args), std::move(_outputs)),
        axes_(axes),
        correction_(correction),
        keep_dim_(keep_dim) {}
  virtual ~VarianceOpRecord() = default;

  virtual bool operator==(const RecordFunctor& other) const final {
    auto result = false;
    if (auto child_ptr = dynamic_cast<const VarianceOpRecord*>(&other)) {
      result = RecordFunctor::operator==(other);
      result = result && (correction_ == child_ptr->correction_);
      result = result && (keep_dim_ == child_ptr->keep_dim_);
      if (result) {
        result = (axes_.size() == child_ptr->axes_.size());
        if (result) {
          for (size_t i = 0; i < axes_.size(); ++i) {
            if (axes_[i] != child_ptr->axes_[i]) {
              result = false;
              break;
            }
          }
        }
      }
    }
    return result;
  }

  void operator()(FusionDefinition& fd) final {
    auto arg = fd.getFusionState(args.at(0))->as<NvfTensorView>();
    auto output =
        torch::jit::fuser::cuda::variance(arg, axes_, correction_, keep_dim_);
    fd.setFusionState(outputs.at(0), output);
  }

 private:
  //! Dimensions of tensor to reduce for variance calculation
  std::vector<int> axes_;
  //! Bessel's correction value
  int64_t correction_;
  //! Indicates whether to keep the reduced dimension(s).
  bool keep_dim_;
};

} // namespace nvfuser
