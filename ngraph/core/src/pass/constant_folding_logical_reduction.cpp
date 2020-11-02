//*****************************************************************************
// Copyright 2017-2020 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//*****************************************************************************

#include "constant_folding.hpp"
#include "ngraph/log.hpp"
#include "ngraph/op/reduce_logical_and.hpp"
#include "ngraph/op/reduce_logical_or.hpp"
#include "ngraph/runtime/reference/logical_reduction.hpp"

NGRAPH_SUPPRESS_DEPRECATED_START

using namespace std;
using namespace ngraph;

static shared_ptr<op::Constant> fold_constant_logical_reduction(shared_ptr<op::Constant> constant,
                                                                shared_ptr<Node> reduction_node)
{
    runtime::AlignedBuffer buffer(shape_size(reduction_node->get_shape()) * sizeof(char));
    char* data_ptr = buffer.get_ptr<char>();

    if (auto reduce_and = as_type_ptr<::ngraph::op::v1::ReduceLogicalAnd>(reduction_node))
    {
        const auto reduction_axes = reduce_and->get_reduction_axes();
        const auto input_shape = reduce_and->get_input_shape(0);
        const char* arg = constant->get_data_ptr<char>();

        runtime::reference::reduce_logical_and(
            arg, data_ptr, input_shape, reduction_axes, reduce_and->get_keep_dims());
    }
    else if (auto reduce_or = as_type_ptr<::ngraph::op::v1::ReduceLogicalOr>(reduction_node))
    {
        const auto reduction_axes = reduce_or->get_reduction_axes();
        const auto input_shape = reduce_or->get_input_shape(0);
        const char* arg = constant->get_data_ptr<char>();

        runtime::reference::reduce_logical_or(
            arg, data_ptr, input_shape, reduction_axes, reduce_or->get_keep_dims());
    }
    else
    {
        NGRAPH_CHECK(false,
                     "Internal nGraph error: Ops handled in "
                     "fold_constant_logical_reduction must be consistent with those "
                     "matched in construct_constant_logical_reduction");
    }

    return make_shared<op::Constant>(
        reduction_node->get_output_element_type(0), reduction_node->get_shape(), data_ptr);
}

void pass::ConstantFolding::construct_constant_logical_reduction()
{
    auto constant_data_label = make_shared<pattern::op::Label>(
        element::boolean, Shape{2, 3, 4}, pattern::has_class<op::Constant>());
    auto constant_axes_label =
        make_shared<pattern::op::Label>(element::i64, Shape{2}, pattern::has_class<op::Constant>());
    auto is_supported_reduction = [](std::shared_ptr<Node> n) {
        return pattern::has_class<::ngraph::op::v1::ReduceLogicalAnd>()(n) ||
               pattern::has_class<::ngraph::op::v1::ReduceLogicalOr>()(n);
    };
    auto reduction =
        std::make_shared<pattern::op::Any>(element::i32,
                                           Shape{2},
                                           is_supported_reduction,
                                           NodeVector{constant_data_label, constant_axes_label});

    auto constant_logical_reduction_callback = [this, constant_data_label](pattern::Matcher& m) {
        NGRAPH_DEBUG << "In callback for constant_logical_reduction_callback against node = "
                     << m.get_match_root()->get_name();

        auto pattern_map = m.get_pattern_map();

        auto constant_match = static_pointer_cast<op::Constant>(pattern_map[constant_data_label]);
        auto reduction_match = m.get_match_root();

        if (cf_is_disabled(reduction_match))
            return false;

        NGRAPH_CHECK(revalidate_and_ensure_static(reduction_match));
        auto const_node = fold_constant_logical_reduction(constant_match, reduction_match);
        const_node->set_friendly_name(reduction_match->get_friendly_name());
        replace_node(reduction_match, const_node);
        copy_runtime_info_to_target_inputs(reduction_match, const_node);
        return true;
    };

    auto logical_reduction_matcher =
        make_shared<pattern::Matcher>(reduction, "ConstantFolding.ConstantLogicalReduction");
    NGRAPH_SUPPRESS_DEPRECATED_START
    this->add_matcher(logical_reduction_matcher,
                      constant_logical_reduction_callback,
                      PassProperty::CHANGE_DYNAMIC_STATE);
    NGRAPH_SUPPRESS_DEPRECATED_END
}