// clang-format off
/*
 * SPDX-FileCopyrightText: Copyright (c) 2023-present NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
// clang-format on
#include <device_lower/utils.h>
#include <expr_simplifier.h>
#include <fusion.h>
#include <ir/builder.h>
#include <ir/iostream.h>
#include <ir/utils.h>
#include <iter_visitor.h>
#include <ops/arith.h>
#include <scheduler/mma_utils.h>

#include <limits>
#include <set>

namespace nvfuser::ir_utils {

std::vector<int64_t> normalizeNew2Old(
    const std::vector<int64_t>& new2old_in,
    int64_t ndims) {
  NVF_CHECK(
      (int64_t)new2old_in.size() == ndims,
      "There must be a transpose mapping for each dimension in domain");

  // Canonicalize dimensions by wrapping each dim for the given ndims
  std::vector<int64_t> new2old;
  std::transform(
      new2old_in.begin(),
      new2old_in.end(),
      std::inserter(new2old, new2old.begin()),
      [ndims](int64_t entry) { return entry < 0 ? entry + ndims : entry; });

  // Check if any adjusted values are < 0, or >= nDims, which are invalid
  NVF_CHECK(
      std::none_of(
          new2old.begin(),
          new2old.end(),
          [ndims](int64_t entry) { return entry < 0 || entry >= ndims; }),
      "New2Old axes are not within the number of dimensions of the provided domain.\t",
      new2old);

  // Going to use sets, to see if any duplicate values are in the map.
  std::set<int64_t> old_pos_set;
  std::transform(
      new2old.begin(),
      new2old.end(),
      std::inserter(old_pos_set, old_pos_set.begin()),
      [](int64_t entry) { return entry; });

  // Error out if duplicate values are found.
  NVF_CHECK(
      (int64_t)new2old.size() == ndims && old_pos_set.size() == new2old.size(),
      "Duplicate entries in transformation map.");

  // END VALIDATION CHECKS
  return new2old;
}

std::vector<int64_t> normalizeOld2New(
    const std::unordered_map<int64_t, int64_t>& old2new_in,
    int64_t ndims) {
  // adjust based on negative values (any negative values gets nDims added to
  // it)
  std::unordered_map<int64_t, int64_t> old2new;
  std::transform(
      old2new_in.begin(),
      old2new_in.end(),
      std::inserter(old2new, old2new.begin()),
      [ndims](std::unordered_map<int64_t, int64_t>::value_type entry) {
        return std::unordered_map<int64_t, int64_t>::value_type({
            entry.first < 0 ? entry.first + ndims : entry.first,
            entry.second < 0 ? entry.second + ndims : entry.second,
        });
      });

  // Check if any adjusted values are < 0, or >= nDims, which are invalid

  NVF_CHECK(
      std::none_of(
          old2new.begin(),
          old2new.end(),
          [ndims](std::unordered_map<int64_t, int64_t>::value_type entry) {
            return entry.first < 0 || entry.first >= ndims ||
                entry.second < 0 || entry.second >= ndims;
          }),
      "Reorder axes are not within the number of dimensions of the provided domain.");

  // Going to use sets, to see if any duplicate values are in the map.

  std::set<int64_t> old_pos_set;
  std::transform(
      old2new.begin(),
      old2new.end(),
      std::inserter(old_pos_set, old_pos_set.begin()),
      [](std::unordered_map<int64_t, int64_t>::value_type entry) {
        return entry.first;
      });

  std::set<int64_t> new_pos_set;
  std::transform(
      old2new.begin(),
      old2new.end(),
      std::inserter(new_pos_set, new_pos_set.begin()),
      [](std::unordered_map<int64_t, int64_t>::value_type entry) {
        return entry.second;
      });

  // Error out if duplicate values are found.
  NVF_CHECK(
      old_pos_set.size() == old2new.size() &&
          new_pos_set.size() == old2new.size(),
      "Duplicate entries in transformation map sent to TensorView reorder.");

  // END VALIDATION CHECKS

  std::vector<int64_t> new2old(ndims, -1);

  // Go through each old and new position, make sure they're within [0, ndims)
  for (std::pair<int64_t, int64_t> elem : old2new) {
    int64_t old_pos = elem.first;
    int64_t new_pos = elem.second;
    new2old[new_pos] = old_pos;
  }

  // old_positions that already have a new position
  std::set<int64_t> old_positions(new2old.begin(), new2old.end());
  old_positions.erase(-1);

  // All available new positions
  std::set<int64_t> all_positions;
  for (auto i : c10::irange(ndims)) {
    all_positions.insert((int64_t)i);
  }

  // Check what positions haven't been specified.
  std::set<int64_t> positions_left;
  std::set_difference(
      all_positions.begin(),
      all_positions.end(),
      old_positions.begin(),
      old_positions.end(),
      std::inserter(positions_left, positions_left.end()));

  // Fill in positions that weren't specified, in relative order,
  // in empty spots in the set of new positions.
  // new2old[new_position] = old_position
  auto it = positions_left.begin(); // old positions left
  std::transform(
      new2old.begin(),
      new2old.end(),
      new2old.begin(),
      [&it](int64_t i) -> int64_t { return i == -1 ? *it++ : i; });

  return new2old;
}

namespace ValReplacement {
// Create New Expr given producer - [an input for the expression]
// Creates a new Expr substituting current with producer
struct SubstituteInExpr : public OptOutMutator {
 public:
  static Expr* subsitute(Expr* expr, Val* reference, Val* substitute) {
    NVF_ERROR(
        expr != nullptr && reference != nullptr && substitute != nullptr,
        "Nullptr arg found.");
    SubstituteInExpr sie(reference, substitute);
    sie.mutate(expr);
    // if nothing substituted, then return the original expr
    return sie.expr_ == nullptr ? expr : sie.expr_;
  }

 protected:
  void removeExpr(IrContainer*, Expr*) const override {}

  void registerNewExpr(Expr* expr) override {
    expr_ = expr;
  }

 private:
  explicit SubstituteInExpr(Val* reference, Val* substitute) {
    mutations_[reference] = substitute;
  }

 private:
  Expr* expr_ = nullptr;
};

} // namespace ValReplacement

Expr* replaceValInExprInputs(Expr* expr, Val* reference, Val* substitute) {
  FusionGuard fg(expr->fusion());
  return ValReplacement::SubstituteInExpr::subsitute(
      expr, reference, substitute);
}

void replaceValInAllExprInputsAndFusionOutputs(Val* old_val, Val* new_val) {
  auto uses = old_val->uses();
  for (auto use_of_old_val : uses) {
    ir_utils::replaceValInExprInputs(use_of_old_val, old_val, new_val);
  }
  if (old_val->isFusionOutput()) {
    old_val->fusion()->replaceOutput(old_val, new_val);
  }
}

Expr* transferDefinitionToNewOutputs(
    Expr* expr,
    const std::vector<Val*>& new_outputs) {
  NVF_ERROR(
      new_outputs.size() == expr->outputs().size(),
      "Number of new outputs must match old outputs");
  OptOutMutator mutator;
  for (const auto i : c10::irange(new_outputs.size())) {
    auto old_output = expr->outputs().at(i);
    auto new_output = new_outputs.at(i);
    if (new_output == old_output) {
      continue;
    }
    NVF_ERROR(
        !new_output->isConst(),
        "Cannot transfer a definition Expr onto a const Val. Found new output ",
        new_output->toString(),
        " with constant value ",
        new_output->value());
    NVF_ERROR(
        new_output->vtype() == old_output->vtype(),
        "transforDefinitionToNewOutputs cannot change val type. Found ",
        new_output->vtype(),
        " and ",
        old_output->vtype());
    NVF_ERROR(
        new_output->dtype() == old_output->dtype(),
        "transforDefinitionToNewOutputs cannot change data type. Found ",
        new_output->dtype(),
        " and ",
        old_output->dtype());
    NVF_ERROR(
        new_output->definition() == nullptr,
        "New output ",
        new_output->toString(),
        " must not already have a definition.");
    mutator.registerMutation(old_output, new_output);
  }
  return mutator.mutateExprOutputsOnly(expr);
}

TensorView* rFactorHelper(
    TensorView* reduction_tv,
    const std::vector<int64_t>& axes) {
  NVF_ERROR(reduction_tv->definition() != nullptr);
  const bool has_multiple_tvs = reduction_tv->definition()->inputs().size() > 1;
  if (!has_multiple_tvs) {
    return reduction_tv->rFactor(axes);
  }

  std::vector<TensorView*> out_tvs;
  std::transform(
      reduction_tv->definition()->outputs().begin(),
      reduction_tv->definition()->outputs().end(),
      std::back_inserter(out_tvs),
      [](Val* val) { return val->as<TensorView>(); });

  auto rf_tvs = reduction_tv->rFactor(axes, out_tvs);

  return rf_tvs.at(std::distance(
      out_tvs.begin(),
      std::find(out_tvs.begin(), out_tvs.end(), reduction_tv)));
}

namespace {

template <typename T>
std::vector<T*> uniqueEntries(const std::vector<T*>& tv_vector) {
  VectorOfUniqueEntries<T*> unique_vector(tv_vector.begin(), tv_vector.end());
  return unique_vector.vector();
}

} // namespace

// Return immediate producers of val
std::vector<Val*> producerValsOf(const Val* val) {
  if (val->definition() == nullptr) {
    return {};
  }
  auto producer_vals = val->definition()->inputs();
  return uniqueEntries<Val>({producer_vals.begin(), producer_vals.end()});
}

// Return immediate consumers of val
std::vector<Val*> consumerValsOf(const Val* val) {
  std::vector<Val*> consumer_vals;
  for (auto use_expr : val->uses()) {
    auto outputs = use_expr->outputs();
    consumer_vals.insert(consumer_vals.end(), outputs.begin(), outputs.end());
  }
  return uniqueEntries<Val>(consumer_vals);
}

// Return immediate siblings of val
std::vector<Val*> siblingValsOf(const Val* val) {
  std::vector<Val*> sibling_vals;
  auto def = val->definition();
  if (def != nullptr) {
    auto outs = def->outputs();
    for (auto sibling_val : outs) {
      if (sibling_val == val) {
        continue;
      }
      sibling_vals.emplace_back(sibling_val);
    }
  }
  return sibling_vals;
}

// Return immediate producers of val
std::vector<Val*> producerValsOf(const std::vector<Val*>& vals) {
  std::vector<Val*> all_producer_vals;
  for (auto val : vals) {
    auto producer_vals = producerValsOf(val);
    all_producer_vals.insert(
        all_producer_vals.end(), producer_vals.begin(), producer_vals.end());
  }

  return uniqueEntries<Val>(all_producer_vals);
}

// Return immediate consumers of val
std::vector<Val*> consumerValsOf(const std::vector<Val*>& vals) {
  std::vector<Val*> all_consumer_vals;
  for (auto val : vals) {
    auto consumer_vals = consumerValsOf(val);
    all_consumer_vals.insert(
        all_consumer_vals.end(), consumer_vals.begin(), consumer_vals.end());
  }

  return uniqueEntries<Val>(all_consumer_vals);
}

std::vector<TensorView*> producerTvsOf(const TensorView* tv) {
  auto producer_vals = producerValsOf(tv);
  auto producer_tvs = ir_utils::filterByType<TensorView>(producer_vals);
  return {producer_tvs.begin(), producer_tvs.end()};
}

std::vector<TensorView*> consumerTvsOf(const TensorView* tv) {
  auto consumer_vals = consumerValsOf(tv);
  auto consumer_tvs = ir_utils::filterByType<TensorView>(consumer_vals);
  return {consumer_tvs.begin(), consumer_tvs.end()};
}

std::vector<TensorView*> siblingTvsOf(const TensorView* tv) {
  auto sibling_vals = siblingValsOf(tv);
  auto sibling_tvs = ir_utils::filterByType<TensorView>(sibling_vals);
  return {sibling_tvs.begin(), sibling_tvs.end()};
}

std::vector<TensorView*> producerTvsOf(const std::vector<TensorView*>& tvs) {
  std::vector<TensorView*> all_producer_tvs;
  for (auto tv : tvs) {
    auto producer_tvs = producerTvsOf(tv);
    all_producer_tvs.insert(
        all_producer_tvs.end(), producer_tvs.begin(), producer_tvs.end());
  }

  return uniqueEntries<TensorView>(all_producer_tvs);
}

std::vector<TensorView*> consumerTvsOf(const std::vector<TensorView*>& tvs) {
  std::vector<TensorView*> all_consumer_tvs;
  for (auto tv : tvs) {
    auto consumer_tvs = consumerTvsOf(tv);
    all_consumer_tvs.insert(
        all_consumer_tvs.end(), consumer_tvs.begin(), consumer_tvs.end());
  }

  return uniqueEntries<TensorView>(all_consumer_tvs);
}

std::vector<TensorView*> inputTvsOf(TensorView* tv) {
  return inputTvsOf(std::vector<TensorView*>{tv});
}

std::vector<TensorView*> outputTvsOf(TensorView* tv) {
  return outputTvsOf(std::vector<TensorView*>{tv});
}

std::vector<TensorView*> inputTvsOf(std::vector<TensorView*> tvs) {
  auto inp_vals = IterVisitor::getInputsTo({tvs.begin(), tvs.end()});
  auto filtered = ir_utils::filterByType<TensorView>(inp_vals);
  std::vector<TensorView*> inp_tvs(filtered.begin(), filtered.end());
  return uniqueEntries<TensorView>(inp_tvs);
}

std::vector<TensorView*> outputTvsOf(std::vector<TensorView*> tvs) {
  auto out_vals = DependencyCheck::getAllOutputsOf({tvs.begin(), tvs.end()});
  auto filtered = ir_utils::filterByType<TensorView>(out_vals);
  std::vector<TensorView*> out_tvs(filtered.begin(), filtered.end());
  return uniqueEntries<TensorView>(out_tvs);
}

VectorOfUniqueEntries<TensorView*> allTvsOfExprs(
    const std::vector<Expr*>& exprs) {
  VectorOfUniqueEntries<TensorView*> all_tvs;
  for (auto expr : exprs) {
    auto input_tvs = ir_utils::filterByType<TensorView>(expr->inputs());
    auto output_tvs = ir_utils::filterByType<TensorView>(expr->outputs());
    for (const auto& tvs : {input_tvs, output_tvs}) {
      all_tvs.pushBack(tvs.begin(), tvs.end());
    }
  }
  return all_tvs;
}

std::vector<TensorView*> allTvsExcept(
    Fusion* fusion,
    const std::unordered_set<TensorView*>& except) {
  auto all_tvs = fusion->allTvs();
  std::vector<TensorView*> result;
  for (auto tv : all_tvs) {
    if (except.count(tv) == 0) {
      result.emplace_back(tv);
    }
  }
  return result;
}

std::vector<Expr*> getAllTypesOfReductionOps(Fusion* fusion) {
  return getOpsOfType<ReductionOp, GroupedReductionOp, WelfordOp>(fusion);
}

bool hasAnyReductionOps(Fusion* fusion) {
  return hasOpsOfType<ReductionOp, GroupedReductionOp, WelfordOp>(fusion);
}

namespace {

class ValReplacementMutator : private OptOutMutator {
 public:
  ValReplacementMutator(
      Fusion* fusion,
      const std::unordered_map<Val*, Val*>& replacement_map)
      : replacement_map_(replacement_map) {
    FusionGuard fg(fusion);

    // Welford makes this a little annoying since it holds a count which is
    // typically not used by anything else. If we don't grab that count, then it
    // would be a tensorview that doesn't get updated extents. Therefore, first
    // grab all leaves towards outputs and grab stmts from there.
    auto stmts = StmtSort::getStmtsTo(allLeafOuts(fusion), true, true);

    // Some fusions, such as standalone rand_like, can have disconnected DAG, so
    // we need some mechanism to make sure our replacement set is as complete as
    // possible
    // TODO: I think we need a more general mechanism to support disconnected
    // DAG
    std::vector<Val*> more;
    for (auto v : fusion->inputs()) {
      if (std::find(stmts.begin(), stmts.end(), v) == stmts.end()) {
        more.emplace_back(v);
      }
    }
    for (auto v : fusion->axioms()) {
      if (std::find(stmts.begin(), stmts.end(), v) == stmts.end()) {
        more.emplace_back(v);
      }
    }
    auto more_stmts = StmtSort::getStmtsTo(more, true, true);
    more_stmts.insert(more_stmts.end(), stmts.begin(), stmts.end());

    for (auto stmt : more_stmts) {
      dispatchMutate(stmt);
    }

    for (const auto& [old_v, new_v] : replacement_map_) {
      if (old_v->isFusionOutput()) {
        fusion->replaceOutput(old_v, new_v);
      }
    }
  }

 private:
  using OptOutMutator::dispatchMutate;
  using OptOutMutator::mutate;

  void dispatchMutate(Val* val) final {
    if (replacement_map_.find(val) == replacement_map_.end()) {
      return OptOutMutator::dispatchMutate(val);
    }
    auto replaced_val = replacement_map_.at(val);
    registerMutation(val, replaced_val);
  }

  std::vector<Val*> allLeafOuts(Fusion* fusion) {
    auto exprs = StmtSort::getExprs(fusion, true);
    std::unordered_set<Val*> inputs;
    std::unordered_set<Val*> outputs;
    std::vector<Val*> ordered_outputs;
    for (auto expr : exprs) {
      inputs.insert(expr->inputs().begin(), expr->inputs().end());
      outputs.insert(expr->outputs().begin(), expr->outputs().end());
      ordered_outputs.insert(
          ordered_outputs.end(),
          expr->outputs().begin(),
          expr->outputs().end());
    }
    for (auto input : inputs) {
      outputs.erase(input);
    }

    std::vector<Val*> ordered_leaf_outs;
    for (auto out : ordered_outputs) {
      if (outputs.find(out) != outputs.end()) {
        ordered_leaf_outs.push_back(out);
      }
    }
    return ordered_leaf_outs;
  }

  const std::unordered_map<Val*, Val*>& replacement_map_;
};

} // namespace

void replaceValue(
    Fusion* fusion,
    const std::unordered_map<Val*, Val*>& replacement_map) {
  // NOLINTNEXTLINE(bugprone-unused-raii)
  ValReplacementMutator(fusion, replacement_map);
}

Val* getReductionInitValOf(TensorView* tv) {
  auto def = tv->definition();
  if (def == nullptr) {
    return nullptr;
  }

  Val* init = nullptr;
  if (auto rop = dynamic_cast<ReductionOp*>(def)) {
    init = rop->init();
  } else if (auto grop = dynamic_cast<GroupedReductionOp*>(def)) {
    int output_idx = grop->getExprIndexOfOutput(tv);
    init = grop->initVal(output_idx);
  } else if (auto wop = dynamic_cast<WelfordOp*>(def)) {
    return wop->getInitValOfOutput(tv);
  } else if (auto gwop = dynamic_cast<GroupedWelfordOp*>(def)) {
    init = gwop->getInitValOfOutput(tv);
  } else if (auto mma = dynamic_cast<MmaOp*>(def)) {
    init = mma->init();
  }

  return init;
}

// TODO: Should mma be in here? Should we return true if it's a trivial
// reduction?
bool isReductionOp(const Expr* expr) {
  // Note that GridReduction inherits ReductionOp
  return expr->isOneOf<
      ReductionOp,
      GroupedReductionOp,
      WelfordOp,
      GroupedWelfordOp,
      kir::GridWelford,
      kir::GroupedGridWelford>();
}

bool isReductionTvOp(const Expr* expr) {
  return ir_utils::isTvOp(expr) && isReductionOp(expr);
}

bool isPointwiseTvOp(const Expr* expr) {
  // LoadStoreOp with producer projection means transpose, which is not
  // considered pointwise
  return isTvOp(expr) &&
      (expr->isOneOf<UnaryOp, BinaryOp, TernaryOp>() ||
       (expr->isA<LoadStoreOp>() && !ir_utils::getTvOutput(expr)->hasRoot()));
}

bool isSegmentSet(const Expr* e) {
  if (const auto* ldst = dynamic_cast<const LoadStoreOp*>(e)) {
    if (ldst->opType() == LoadStoreOpType::SegmenterSet) {
      return true;
    }
  }
  return false;
}

std::vector<ViewOp*> getViewOps(Fusion* fusion) {
  auto all_exprs = fusion->exprs();

  auto all_view_ops = ir_utils::filterByType<ViewOp>(all_exprs);

  std::vector<ViewOp*> view_ops;

  std::copy_if(
      all_view_ops.begin(),
      all_view_ops.end(),
      std::back_inserter(view_ops),
      [](ViewOp* view) {
        return std::any_of(
            view->outputs().begin(), view->outputs().end(), [](Val* v) {
              if (!v->isA<TensorView>()) {
                return false;
              }
              return v->as<TensorView>()->hasRoot();
            });
      });

  return view_ops;
}

Val* replaceValRecursively(
    Val* val,
    const std::unordered_map<Val*, Val*>& replacement_map) {
  if (replacement_map.find(val) != replacement_map.end()) {
    return replacement_map.at(val);
  }

  auto def = val->definition();
  if (def == nullptr) {
    return val;
  }

  NVF_ERROR(def->outputs().size() == 1);

  bool mutated = false;

  std::vector<Val*> mutated_inputs;
  mutated_inputs.reserve(def->inputs().size());
  for (auto input : def->inputs()) {
    auto new_input = replaceValRecursively(input, replacement_map);
    if (new_input != input) {
      mutated = true;
    }
    mutated_inputs.emplace_back(new_input);
  }

  std::vector<Statement*> mutated_attrs;
  mutated_attrs.reserve(def->attributes().size());
  for (auto attr : def->attributes()) {
    if (auto attr_val = dynamic_cast<Val*>(attr)) {
      auto new_attr_val = replaceValRecursively(attr_val, replacement_map);
      if (new_attr_val != attr_val) {
        mutated = true;
      }
      mutated_attrs.emplace_back(new_attr_val);
    } else {
      mutated_attrs.emplace_back(attr);
    }
  }

  if (!mutated) {
    return val;
  }

  auto out = IrBuilder::create<Val>(val->dtype());
  auto newObjectFunc = def->newObjectFunc();
  newObjectFunc(def->container(), mutated_inputs, {out}, mutated_attrs);

  return out;
}

bool isSqueezeInput(const TensorView* tv) {
  for (auto expr : tv->uses()) {
    if (expr->isA<SqueezeOp>()) {
      return true;
    }
  }
  return false;
}

bool isSqueezedID(const TensorView* tv, const IterDomain* id) {
  auto logical_dom = TensorDomain::noReductions(tv->getLogicalDomain());
  auto squeezes = ir_utils::filterByType<SqueezeOp>(tv->uses());
  for (auto i : c10::irange(logical_dom.size())) {
    if (logical_dom[i] != id) {
      continue;
    }
    for (auto squeeze : squeezes) {
      if (squeeze->isSqueezeDim(i)) {
        return true;
      }
    }
  }
  return false;
}

bool isIndexedID(const TensorView* tv, const IterDomain* id) {
  return isIndexedProducerID(tv, id) || isIndexedConsumerID(tv, id);
}

bool isIndexedProducerID(const TensorView* tv, const IterDomain* id) {
  return std::any_of(tv->uses().begin(), tv->uses().end(), [&](Expr* expr) {
    return getIndexedProducerID(expr) == id;
  });
}

IterDomain* getIndexedProducerID(const Expr* expr) {
  if (auto select = dynamic_cast<const SelectOp*>(expr)) {
    return select->getIndexedID();
  } else if (auto index_select = dynamic_cast<const IndexSelectOp*>(expr)) {
    return index_select->getIndexedID();
  } else if (auto gather = dynamic_cast<const TorchGatherOp*>(expr)) {
    return gather->getIndexedID();
  } else {
    return nullptr;
  }
}

IterDomain* getConsumerOfIndexedProducerID(const Expr* expr) {
  if (auto index_select = dynamic_cast<const IndexSelectOp*>(expr)) {
    return index_select->getConsumerOfIndexedID();
  } else if (auto gather = dynamic_cast<const TorchGatherOp*>(expr)) {
    return gather->getConsumerOfIndexedID();
  } else {
    return nullptr;
  }
}

bool isIndexedConsumerID(const TensorView* tv, const IterDomain* id) {
  return tv->definition()->isA<ScatterOp>() &&
      tv->definition()->as<ScatterOp>()->getIndexedID() == id;
}

bool isIndexSelectLookupTv(const TensorView* tv) {
  for (auto expr : tv->uses()) {
    if (expr->isA<IndexSelectOp>()) {
      auto idx_sel = expr->as<IndexSelectOp>();
      if (idx_sel->input(0) == tv) {
        return true;
      }
    }
  }
  return false;
}

bool isIndexSelectIndicesTv(const TensorView* tv) {
  for (auto expr : tv->uses()) {
    if (expr->isA<IndexSelectOp>()) {
      auto idx_sel = expr->as<IndexSelectOp>();
      if (idx_sel->input(1) == tv) {
        return true;
      }
    }
  }
  return false;
}

bool isTorchGatherLookupTv(const Val* tv) {
  for (auto expr : tv->uses()) {
    if (expr->isA<TorchGatherOp>()) {
      auto idx_sel = expr->as<TorchGatherOp>();
      if (idx_sel->lookupTv() == tv) {
        return true;
      }
    }
  }
  return false;
}

std::string varName(const Val* val) {
  if (val->isA<kir::TensorIndex>()) {
    return varName(val->as<kir::TensorIndex>()->view());
  }
  std::stringstream name;
  if (val->isA<TensorView>()) {
    name << "T";
  } else {
    name << typePrefix(val->dtype());
  }
  name << val->name();
  return name.str();
}

bool hasResizedRfactor(const TensorView* tv) {
  if (!tv->hasRoot()) {
    return false;
  }
  auto root_to_rf_exprs = StmtSort::getExprsBetween(
      {tv->getRootDomain().begin(), tv->getRootDomain().end()},
      {tv->getLogicalDomain().begin(), tv->getLogicalDomain().end()});
  return std::any_of(
      root_to_rf_exprs.begin(), root_to_rf_exprs.end(), [](Expr* expr) {
        return expr->isA<Resize>();
      });
}

std::vector<TensorView*> getTVsWithDynamicTransform(Fusion* fusion) {
  const auto all_tvs = fusion->allTvs();
  std::vector<TensorView*> dynamic_tvs;
  std::copy_if(
      all_tvs.begin(),
      all_tvs.end(),
      std::back_inserter(dynamic_tvs),
      [](auto tv) { return tv->domain()->hasSymbolicAxis(); });
  return dynamic_tvs;
}

CompareDomainResult compareDomains(
    std::vector<IterDomain*> dom0,
    const std::vector<IterDomain*>& dom1,
    const std::vector<IterDomain*>& additional_ids,
    bool ignore_broadcast) {
  std::unordered_set<Val*> dom0_set(dom0.begin(), dom0.end());
  std::unordered_set<Val*> dom1_set(dom1.begin(), dom1.end());
  std::unordered_set<Val*> additional_ids_set(
      additional_ids.begin(), additional_ids.end());

  // empty domain are equivalent.
  if (dom0.empty() && dom1.empty()) {
    return {};
  }
  // Make sure there's no duplicate in the parameter vectors
  NVF_ERROR(
      dom0.size() == dom0_set.size(),
      "Duplicated entry is detected in dom0: ",
      toDelimitedString(dom0));
  NVF_ERROR(
      dom1.size() == dom1_set.size(),
      "Duplicated entry is detected in dom1: ",
      toDelimitedString(dom1));

  dom0.insert(dom0.end(), additional_ids.begin(), additional_ids.end());
  auto exprs = IRBFS::getExprsBetween(
      {dom0.begin(), dom0.end()}, {dom1.begin(), dom1.end()}, false);

  std::unordered_set<Val*> frontier(dom0.begin(), dom0.end());

  for (auto [expr, direction] : exprs) {
    NVF_ERROR(
        std::all_of(expr->inputs().begin(), expr->inputs().end(), [](Val* v) {
          return v->isA<IterDomain>();
        }));
    NVF_ERROR(
        std::all_of(expr->outputs().begin(), expr->outputs().end(), [](Val* v) {
          return v->isA<IterDomain>();
        }));
    std::vector<Val*> from;
    std::vector<Val*> to;
    if (direction == Direction::Forward) {
      from = expr->inputs();
      to = expr->outputs();
    } else {
      from = expr->outputs();
      to = expr->inputs();
    }
    if (std::all_of(from.begin(), from.end(), [&](Val* v) {
          return additional_ids_set.count(v);
        })) {
      additional_ids_set.insert(to.begin(), to.end());
      continue;
    }
    for (auto v : to) {
      if (additional_ids_set.count(v)) {
        continue;
      }
      NVF_ERROR(
          frontier.insert(v).second,
          "Invalid derived domain due to dependent expr: ",
          expr->toString(),
          ". Output should just show up once: ",
          v->toString());
    }
    for (auto v : from) {
      bool ignorable =
          (ignore_broadcast && v->as<IterDomain>()->isBroadcast()) ||
          additional_ids_set.count(v);
      NVF_ERROR(
          frontier.erase(v) == 1 || ignorable,
          "Invalid derived domain due to dependent expr: ",
          expr->toString(),
          ". Input not seen before: ",
          v->toString());
    }
  }

  // Remove symbolic IDs that appear both in frontier and in dom1_set. These IDs
  // are carried over without any transformation.
  auto is_symb = [](Val* v) {
    return v->as<IterDomain>()->getIterType() == IterType::Symbolic;
  };
  std::vector<Val*> ids_to_remove;
  for (Val* id : frontier) {
    if (is_symb(id) && dom1_set.count(id)) {
      ids_to_remove.push_back(id);
    }
  }
  for (Val* id : ids_to_remove) {
    frontier.erase(id);
    dom1_set.erase(id);
  }
  // At this point, the frontier set and dom1 should be equal, except when
  // there's a symbolic ID in frontier or dom1, where the transformations are
  // incomplete.
  bool frontier_has_symbolic =
      std::any_of(frontier.begin(), frontier.end(), is_symb);
  bool dom1_has_symbolic =
      std::any_of(dom1_set.begin(), dom1_set.end(), is_symb);

  CompareDomainResult result;

  // Check if iter domains can be reachable from
  // target_set. Returns true if any of iter domains is
  // unreachable. Additionaly, make sure none of iter domains has any
  // overlap with the other iter domains.
  auto check_ids = [ignore_broadcast](
                       const auto& ids_to_check,
                       const auto& target_set) -> bool {
    bool unreachable = false;
    for (auto id : ids_to_check) {
      // Symbolic and broadcast IDs are ignored
      if (id->template as<IterDomain>()->getIterType() == IterType::Symbolic ||
          (ignore_broadcast && id->template as<IterDomain>()->isBroadcast())) {
        continue;
      }
      if (!target_set.count(id)) {
        // not found in target, which means either:
        //
        // 1. id is unreachable from target_set, or
        // 2. id is reachable from target_set but was erased from
        // target_set as it was used as an input in the traversal.
        //
        // The second case means id is redundant
        NVF_ERROR(
            IRBFS::getReachableValsFrom(
                {target_set.begin(), target_set.end()}, {id})
                .empty(),
            id->toString(),
            " is redundant in ",
            toDelimitedString(target_set));

        unreachable = true;
        // Do not break here. The return value is now determined to be
        // true, but the remaining IDs need also to be checked if they
        // are redundant.
      }
    }
    return unreachable;
  };

  if (!frontier_has_symbolic) {
    result.dom1_has_unreachable_ids = check_ids(dom1, frontier);
  }

  if (!dom1_has_symbolic) {
    result.dom0_has_unreachable_ids = check_ids(frontier, dom1_set);
  }

  return result;
}

void validateDomainEquivalence(
    std::vector<IterDomain*> dom0,
    const std::vector<IterDomain*>& dom1,
    const std::vector<IterDomain*>& additional_ids) {
  const auto compare_result = compareDomains(dom0, dom1, additional_ids);

  NVF_ERROR(
      !compare_result.dom0_has_unreachable_ids,
      "dom0 has unreachable IDs. dom0: ",
      toDelimitedString(dom0),
      ". dom1: ",
      toDelimitedString(dom1));

  NVF_ERROR(
      !compare_result.dom1_has_unreachable_ids,
      "dom1 has unreachable IDs. dom0: ",
      toDelimitedString(dom0),
      ". dom1: ",
      toDelimitedString(dom1));
}

namespace {

std::vector<Statement*> next(Statement* stmt) {
  if (stmt->isVal()) {
    if (auto val = stmt->as<Val>()->definition()) {
      return {val};
    } else {
      return {};
    }
  } else {
    auto expr = stmt->as<Expr>();
    std::vector<Statement*> inputs{
        expr->inputs().begin(), expr->inputs().end()};
    return inputs;
  }
}

} // namespace

std::vector<Statement*> checkCycle(
    Fusion* fusion,
    const std::unordered_set<Statement*>& from,
    const std::vector<Val*>& to) {
  std::unordered_set<Statement*> path;
  std::unordered_set<Statement*> visited;
  std::deque<Statement*> queue;
  queue.insert(queue.end(), to.begin(), to.end());

  while (!queue.empty()) {
    auto val = queue.front();

    // early termination if we have already reached boundary or hit a previously
    // visited node
    if (from.count(val) != 0 || visited.count(val) != 0) {
      queue.pop_front();
      continue;
    }

    auto next_stmts = next(val);

    // if val is a leaf node.
    if (next_stmts.empty()) {
      queue.pop_front();
      visited.insert(val);
      continue;
    }

    // if val is already in path, we are just cleaning up the stack here.
    auto iter = path.find(val);
    if (iter != path.end()) {
      queue.pop_front();
      path.erase(iter);
      visited.insert(val);
      continue;
    }

    // putting self on path
    path.insert(val);

    // check for cycles
    for (auto stmt : next_stmts) {
      if (path.count(stmt) != 0) {
        // find a cycle, return current path;
        std::vector<Statement*> ret;
        std::copy(path.begin(), path.end(), std::back_inserter(ret));
        return ret;
      }
      // adding statement to a queue;
      queue.push_front(stmt);
    }
  }

  // no cycle detected, return empty
  return {};
}

bool isAlignedScopeExpr(const Expr* expr) {
  NVF_ERROR(expr != nullptr);
  if (auto ite = dynamic_cast<const kir::IfThenElse*>(expr)) {
    if (ite->predicate()->hasValue() &&
        getRegisterType(ite->predicate()->value()) ==
            RegisterType::GeneralPurpose) {
      return false;
    }

  } else if (auto fl = dynamic_cast<const ForLoop*>(expr)) {
    // If the start, stop, step are not thread dependent
    //  then this for loop should be thread independent.
    if (getRegisterType(fl->start()) == RegisterType::GeneralPurpose ||
        getRegisterType(fl->stop()) == RegisterType::GeneralPurpose ||
        getRegisterType(fl->step()) == RegisterType::GeneralPurpose) {
      return false;
    }
  } else {
    NVF_THROW("Invalid scope expr: ", expr->toString());
  }

  return true;
}

std::vector<Statement*> checkCycle(Fusion* fusion) {
  return checkCycle(fusion, {}, fusion->outputs());
}

namespace {

inline bool isTensorAttr(const Val* val, const std::string& attr_name) {
  NVF_ERROR(val != nullptr);
  auto getitem = dynamic_cast<GetItem*>(val->definition());
  if (getitem == nullptr) {
    return false;
  }
  auto getattr = dynamic_cast<GetAttr*>(getitem->array()->definition());
  if (getattr == nullptr) {
    return false;
  }
  if (getattr->attr() != attr_name) {
    return false;
  }
  auto metadata = dynamic_cast<GetMetaData*>(getattr->struct_()->definition());
  if (metadata == nullptr) {
    return false;
  }
  return metadata->in()->isA<TensorView>();
}

} // namespace

bool isTensorSize(const Val* val) {
  return isTensorAttr(val, "logical_size") || isTensorAttr(val, "alloc_size");
}

bool isTensorStride(const Val* val) {
  return isTensorAttr(val, "logical_stride") ||
      isTensorAttr(val, "alloc_stride");
}

int64_t getVectorizeSize(const TensorView* tv) {
  for (auto id : tv->getLoopDomain()) {
    if (!isParallelTypeVectorize(id->getParallelType())) {
      continue;
    }

    NVF_ERROR(
        id->extent()->isConstInt(),
        "Could not evaluate constant value bound to vectorized dim.");

    return id->extent()->evaluate().as<int64_t>();
  }
  return 1;
}

bool hasTrivialAllocationDomain(const TensorView* tv) {
  if (!tv->hasAllocation()) {
    return true;
  }
  const std::vector<IterDomain*>& alloc = tv->getMaybeAllocationDomain();
  const std::vector<IterDomain*>& logical = tv->getLogicalDomain();
  return TensorDomain::noBroadcasts(TensorDomain::noReductions(logical)) ==
      TensorDomain::noBroadcasts(TensorDomain::noReductions(alloc));
}
bool hasUniformSiblings(Expr* expr) {
  return !expr->isOneOf<SdpaFwdOp, SdpaBwdOp>();
}

bool hasRootToLoopLinearTransformations(const TensorView* tv) {
  auto root = tv->getMaybeRootDomain();
  auto loop = tv->getLoopDomain();
  std::vector<Val*> loop_val(loop.begin(), loop.end());
  auto all_ids_vec =
      DependencyCheck::getAllValsBetween({root.begin(), root.end()}, loop_val);
  std::unordered_set<Val*> all_ids_set(all_ids_vec.begin(), all_ids_vec.end());
  auto alloc = tv->getMaybeAllocationDomain();
  auto logical = tv->getLogicalDomain();
  bool all_alloc_id_on_path = std::all_of(
      alloc.begin(), alloc.end(), [&](Val* v) { return all_ids_set.count(v); });
  bool all_logical_id_on_path =
      std::all_of(logical.begin(), logical.end(), [&](Val* v) {
        return all_ids_set.count(v);
      });
  return all_alloc_id_on_path && all_logical_id_on_path;
}

bool isLoopDomainFullyDerivedFromLogicalDomain(TensorView* tv) {
  return ir_utils::hasRootToLoopLinearTransformations(tv) &&
      !ir_utils::compareDomains(
           tv->getLoopDomain(),
           tv->getLogicalDomain(),
           /*additional_ids=*/{},
           /*ignore_broadcast=*/false)
           .dom0_has_unreachable_ids;
}

AsyncOpType getAsyncOpType(const Expr* expr) {
  if (auto mma = dynamic_cast<const MmaOp*>(expr)) {
    if (mma->isHopper()) {
      return AsyncOpType::WgMma;
    }
  } else if (ir_utils::isCpAsyncBulkStore(expr)) {
    return AsyncOpType::CpAsyncBulk;
  } else if (ir_utils::isCpAsyncOp(expr)) {
    return AsyncOpType::CpAsync;
  }
  return AsyncOpType::NotAsync;
}

std::string nullOrToString(const Statement* val) {
  return val ? val->toString() : "nullptr";
}

std::string nullOrToInlineString(const Statement* id) {
  return id ? id->toInlineString() : "nullptr";
}

bool isFunctional(const Val* v) {
  auto def = v->definition();
  if (def == nullptr) {
    return true;
  }
  if (auto uop = dynamic_cast<UnaryOp*>(def)) {
    // ElectSync is not functional, it does not return the same value
    // every time it is called, so we do not want to reuse it.
    if (uop->getUnaryOpType() == UnaryOpType::ElectSync) {
      return false;
    }
  }
  return std::all_of(def->inputs().begin(), def->inputs().end(), isFunctional);
}

bool isRecursivelyDefined(Val* val) {
  NVF_ERROR(val != nullptr);

  std::deque<Val*> vals_to_visit;
  vals_to_visit.push_back(val);

  std::unordered_set<Val*> visited_vals;

  while (!vals_to_visit.empty()) {
    auto v = vals_to_visit.front();
    vals_to_visit.pop_front();

    visited_vals.insert(v);

    auto v_def = v->definition();
    if (v_def == nullptr) {
      continue;
    }

    for (const auto inp : v_def->inputs()) {
      if (inp == val) {
        // Recursive dependency detected
        return true;
      }
      // Don't visit the same multiple times
      if (!visited_vals.count(inp)) {
        vals_to_visit.push_back(inp);
      }
    }
  }

  return false;
}

int64_t getOperationCount(Val* val) {
  int64_t num_ops = 0;

  // Start with the given val and recursively count the number of ops
  // by traversing inputs
  std::deque<Val*> vals;
  vals.push_back(val);

  while (!vals.empty()) {
    auto v = vals.front();
    vals.pop_front();

    auto def = v->definition();
    if (def == nullptr) {
      continue;
    }
    ++num_ops;

    for (auto inp : def->inputs()) {
      vals.push_back(inp);
    }
  }

  return num_ops;
}

} // namespace nvfuser::ir_utils
