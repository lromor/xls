// Copyright 2020 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "xls/ir/function_builder.h"

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "xls/common/status/matchers.h"
#include "xls/examples/sample_packages.h"
#include "xls/ir/ir_matcher.h"
#include "xls/ir/package.h"

namespace m = ::xls::op_matchers;

namespace xls {

using status_testing::StatusIs;
using ::testing::AllOf;
using ::testing::HasSubstr;

TEST(FunctionBuilderTest, SimpleSourceLocation) {
  // Lineno/Colno are faked out here.
  const Lineno lineno(7);
  const Colno colno(11);

  Package p("p");
  SourceLocation loc = p.AddSourceLocation(__FILE__, lineno, colno);
  FunctionBuilder b("f", &p);

  Type* bits_32 = p.GetBitsType(32);
  auto x = b.Param("x", bits_32, loc);

  ASSERT_TRUE(x.loc().has_value());
  EXPECT_EQ(__FILE__ ":7", p.SourceLocationToString(*x.loc()));
}

TEST(FunctionBuilderTest, CheckFilenameToFilenoLookup) {
  const std::string filename("fake_file.cc");
  Package p("p");

  // Verify two AddSourceLocation calls to the same filename result in correct
  // filename lookups.

  SourceLocation loc0 = p.AddSourceLocation(filename, Lineno(7), Colno(11));
  EXPECT_EQ("fake_file.cc:7", p.SourceLocationToString(loc0));

  SourceLocation loc1 = p.AddSourceLocation(filename, Lineno(8), Colno(12));
  EXPECT_EQ("fake_file.cc:8", p.SourceLocationToString(loc1));
}

TEST(FunctionBuilderTest, CheckUnknownFileToString) {
  Package p("p");
  SourceLocation loc(Fileno(1), Lineno(7), Colno(11));
  EXPECT_EQ("UNKNOWN:7", p.SourceLocationToString(loc));
}

TEST(FunctionBuilderTest, EmptyFunctionTest) {
  Package p("p");
  FunctionBuilder b("empty", &p);
  EXPECT_THAT(b.Build().status(),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Function cannot be empty")));
}

TEST(FunctionBuilderTest, LessThanTest) {
  Package p("p");
  FunctionBuilder b("lt", &p);
  BitsType* type = p.GetBitsType(33);
  b.ULt(b.Param("a", type), b.Param("b", type));
  XLS_ASSERT_OK_AND_ASSIGN(Function * func, b.Build());
  Node* lt = func->return_value();
  EXPECT_EQ(lt->op(), Op::kULt);
  EXPECT_EQ(lt->GetType(), p.GetBitsType(1));
}

TEST(FunctionBuilderTest, NonRootReturnValue) {
  Package p("p");
  FunctionBuilder b("lt", &p);
  BitsType* type = p.GetBitsType(7);
  BValue and_node = b.And(b.Param("a", type), b.Param("b", type));
  b.Negate(and_node);
  XLS_ASSERT_OK_AND_ASSIGN(Function * func, b.BuildWithReturnValue(and_node));
  Node* return_value = func->return_value();
  EXPECT_EQ(return_value->op(), Op::kAnd);
}

TEST(FunctionBuilderTest, LiteralTupleTest) {
  Package p("p");
  FunctionBuilder b("literal_tuple", &p);
  BValue literal_node =
      b.Literal(Value::Tuple({Value(UBits(1, 2)), Value(UBits(3, 3))}));
  XLS_ASSERT_OK_AND_ASSIGN(Function * func,
                           b.BuildWithReturnValue(literal_node));
  EXPECT_TRUE(func->GetType()->return_type()->IsTuple());
}

TEST(FunctionBuilderTest, LiteralArrayTest) {
  Package p("p");
  FunctionBuilder b("literal_array", &p);
  BValue literal_node =
      b.Literal(Value::ArrayOrDie({Value(UBits(1, 2)), Value(UBits(3, 2))}));
  XLS_ASSERT_OK_AND_ASSIGN(Function * func,
                           b.BuildWithReturnValue(literal_node));
  EXPECT_TRUE(func->GetType()->return_type()->IsArray());
}

TEST(FunctionBuilderTest, MapTest) {
  Package p("p");
  const int kElementCount = 123;
  BitsType* element_type = p.GetBitsType(42);
  ArrayType* array_type = p.GetArrayType(kElementCount, element_type);
  Function* to_apply;
  {
    FunctionBuilder b("to_apply", &p);
    b.ULt(b.Param("element", element_type),
          b.Literal(Value(UBits(10, element_type->bit_count()))));
    XLS_ASSERT_OK_AND_ASSIGN(to_apply, b.Build());
  }
  Function* top;
  {
    FunctionBuilder b("top", &p);
    b.Map(b.Param("input", array_type), to_apply);
    XLS_ASSERT_OK_AND_ASSIGN(top, b.Build());
  }
  Node* map = top->return_value();
  EXPECT_EQ(map->op(), Op::kMap);
  EXPECT_EQ(to_apply->return_value()->GetType(), p.GetBitsType(1));
  EXPECT_EQ(map->GetType(),
            p.GetArrayType(kElementCount, to_apply->return_value()->GetType()));
}

TEST(FunctionBuilderTest, Match) {
  Package p("p");
  FunctionBuilder b("f", &p);
  BitsType* cond_type = p.GetBitsType(8);
  BitsType* value_type = p.GetBitsType(32);
  BValue cond = b.Param("cond", cond_type);
  BValue x = b.Param("x", value_type);
  BValue y = b.Param("y", value_type);
  BValue z = b.Param("z", value_type);
  BValue the_default = b.Param("default", value_type);
  b.Match(cond,
          {{b.Literal(UBits(42, 8)), x},
           {b.Literal(UBits(123, 8)), y},
           {b.Literal(UBits(8, 8)), z}},
          the_default);
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, b.Build());
  EXPECT_THAT(f->return_value(),
              m::OneHotSelect(
                  m::OneHot(m::Concat(m::Eq(m::Param("cond"), m::Literal(8)),
                                      m::Eq(m::Param("cond"), m::Literal(123)),
                                      m::Eq(m::Param("cond"), m::Literal(42)))),
                  /*cases=*/{m::Param("x"), m::Param("y"), m::Param("z"),
                             m::Param("default")}));
}

TEST(FunctionBuilderTest, MatchTrue) {
  Package p("p");
  FunctionBuilder b("f", &p);
  BitsType* pred_type = p.GetBitsType(1);
  BitsType* value_type = p.GetBitsType(32);
  BValue p0 = b.Param("p0", pred_type);
  BValue p1 = b.Param("p1", pred_type);
  BValue p2 = b.Param("p2", pred_type);
  BValue x = b.Param("x", value_type);
  BValue y = b.Param("y", value_type);
  BValue z = b.Param("z", value_type);
  BValue the_default = b.Param("default", value_type);
  b.MatchTrue({{p0, x}, {p1, y}, {p2, z}}, the_default);
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, b.Build());
  EXPECT_THAT(
      f->return_value(),
      m::OneHotSelect(
          m::OneHot(m::Concat(m::Param("p2"), m::Param("p1"), m::Param("p0"))),
          /*cases=*/{m::Param("x"), m::Param("y"), m::Param("z"),
                     m::Param("default")}));
}

TEST(FunctionBuilderTest, ConcatTuples) {
  Package p("p");
  FunctionBuilder b("f", &p);
  BitsType* value_type = p.GetBitsType(32);
  BValue x = b.Param("x", value_type);
  BValue t = b.Tuple({x});
  b.Concat({t, t});
  EXPECT_THAT(b.Build(), status_testing::StatusIs(
                             absl::StatusCode::kInvalidArgument,
                             testing::HasSubstr("it has non-bits type")));
}

TEST(FunctionBuilderTest, AfterAll) {
  Package p("p");
  FunctionBuilder fb("f", &p);
  BValue token_a = fb.AfterAll({});
  BValue token_b = fb.AfterAll({});
  fb.AfterAll({token_a, token_b});

  XLS_ASSERT_OK_AND_ASSIGN(Function * f, fb.Build());
  EXPECT_THAT(f->return_value(), m::AfterAll(m::AfterAll(), m::AfterAll()));
}

TEST(FunctionBuilderTest, AfterAllNonTokenArg) {
  Package p("p");
  FunctionBuilder fb("f", &p);
  BValue token_a = fb.AfterAll({});
  BitsType* value_type = p.GetBitsType(32);
  BValue x = fb.Param("x", value_type);
  fb.AfterAll({token_a, x});

  EXPECT_THAT(
      fb.Build(),
      status_testing::StatusIs(
          absl::StatusCode::kInvalidArgument,
          testing::HasSubstr("Dependency type bits[32] is not a token.")));
}

TEST(FunctionBuilderTest, ArrayIndexBits) {
  Package p("p");
  FunctionBuilder b("f", &p);
  BitsType* value_type = p.GetBitsType(32);
  BValue x = b.Param("x", value_type);
  b.ArrayIndex(x, x);
  EXPECT_THAT(b.Build(),
              status_testing::StatusIs(
                  absl::StatusCode::kInvalidArgument,
                  testing::HasSubstr("it has non-array type bits[32]")));
}

TEST(FunctionBuilderTest, ArrayUpdate) {
  Package p("p");
  FunctionBuilder b("f", &p);
  b.ArrayUpdate(
      b.Literal(Value::ArrayOrDie({Value(UBits(1, 32)), Value(UBits(2, 32))})),
      b.Literal(Value(UBits(0, 32))), b.Literal(Value(UBits(99, 32))));
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, b.Build());
  EXPECT_THAT(f->return_value(), m::Type("bits[32][2]"));
  EXPECT_THAT(
      f->return_value(),
      m::ArrayUpdate(m::Literal("[bits[32]: 1, bits[32]: 2]"),
                     m::Literal("bits[32]: 0"), m::Literal("bits[32]: 99")));
}

TEST(FunctionBuilderTest, ArrayUpdateOnNonArray) {
  Package p("p");
  FunctionBuilder b("f", &p);
  b.ArrayUpdate(b.Literal(Value(UBits(1, 32))), b.Literal(Value(UBits(1, 32))),
                b.Literal(Value(UBits(99, 32))));
  EXPECT_THAT(b.Build(),
              status_testing::StatusIs(
                  absl::StatusCode::kInvalidArgument,
                  testing::HasSubstr("it has non-array type bits[32]")));
}

TEST(FunctionBuilderTest, ArrayUpdateIncompatibleUpdateValue) {
  Package p("p");
  FunctionBuilder b("f", &p);
  b.ArrayUpdate(
      b.Literal(Value::ArrayOrDie({Value(UBits(1, 32)), Value(UBits(2, 32))})),
      b.Literal(Value(UBits(1, 32))), b.Literal(Value(UBits(99, 64))));
  EXPECT_THAT(b.Build(),
              status_testing::StatusIs(
                  absl::StatusCode::kInvalidArgument,
                  testing::HasSubstr("array elements have type bits[32] but "
                                     "the update value is of type bits[64]")));
}

TEST(FunctionBuilderTest, DynamicBitSlice) {
  Package p("p");
  FunctionBuilder b("f", &p);
  BitsType* value_type = p.GetBitsType(32);
  BValue x = b.Param("x", value_type);
  BValue start = b.Param("start", value_type);
  b.DynamicBitSlice(x, start, 4);
  XLS_ASSERT_OK_AND_ASSIGN(Function * func, b.Build());
  EXPECT_THAT(func->return_value(),
              AllOf(m::DynamicBitSlice(), m::Type("bits[4]")));
}

TEST(FunctionBuilderTest, FullWidthDecode) {
  Package p("p");
  FunctionBuilder b("f", &p);
  b.Decode(b.Param("x", p.GetBitsType(7)));
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, b.Build());
  EXPECT_THAT(f->return_value(),
              AllOf(m::Decode(m::Param()), m::Type("bits[128]")));
}

TEST(FunctionBuilderTest, NarrowedDecode) {
  Package p("p");
  FunctionBuilder b("f", &p);
  b.Decode(b.Param("x", p.GetBitsType(7)), /*width=*/42);
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, b.Build());
  EXPECT_THAT(f->return_value(),
              AllOf(m::Decode(m::Param()), m::Type("bits[42]")));
}

TEST(FunctionBuilderTest, OneBitDecode) {
  Package p("p");
  FunctionBuilder b("f", &p);
  b.Decode(b.Param("x", p.GetBitsType(1)));
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, b.Build());
  EXPECT_THAT(f->return_value(),
              AllOf(m::Decode(m::Param()), m::Type("bits[2]")));
}

TEST(FunctionBuilderTest, EncodePowerOfTwo) {
  Package p("p");
  FunctionBuilder b("f", &p);
  b.Encode(b.Param("x", p.GetBitsType(256)));
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, b.Build());
  EXPECT_THAT(f->return_value(),
              AllOf(m::Encode(m::Param()), m::Type("bits[8]")));
}

TEST(FunctionBuilderTest, EncodeLessThanPowerOfTwo) {
  Package p("p");
  FunctionBuilder b("f", &p);
  b.Encode(b.Param("x", p.GetBitsType(255)));
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, b.Build());
  EXPECT_THAT(f->return_value(),
              AllOf(m::Encode(m::Param()), m::Type("bits[8]")));
}

TEST(FunctionBuilderTest, EncodeGreaterThanPowerOfTwo) {
  Package p("p");
  FunctionBuilder b("f", &p);
  b.Encode(b.Param("x", p.GetBitsType(257)));
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, b.Build());
  EXPECT_THAT(f->return_value(),
              AllOf(m::Encode(m::Param()), m::Type("bits[9]")));
}

TEST(FunctionBuilderTest, OneBitEncode) {
  Package p("p");
  FunctionBuilder b("f", &p);
  b.Encode(b.Param("x", p.GetBitsType(1)));
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, b.Build());
  EXPECT_THAT(f->return_value(),
              AllOf(m::Encode(m::Param()), m::Type("bits[0]")));
}

TEST(FunctionBuilderTest, BuildTwiceFails) {
  Package p("p");
  FunctionBuilder b("lt", &p);
  BitsType* type = p.GetBitsType(33);
  b.ULt(b.Param("a", type), b.Param("b", type));

  XLS_EXPECT_OK(b.Build());
  absl::StatusOr<Function*> result = b.Build();

  EXPECT_THAT(result, StatusIs(absl::StatusCode::kFailedPrecondition,
                               HasSubstr("multiple times")));
}

TEST(FunctionBuilderTest, SendAndReceive) {
  Package p("p");
  XLS_ASSERT_OK_AND_ASSIGN(
      Channel * ch0, p.CreateChannel("ch0", ChannelKind::kSendReceive,
                                     {DataElement{"data", p.GetBitsType(32)}},
                                     ChannelMetadataProto()));
  XLS_ASSERT_OK_AND_ASSIGN(
      Channel * ch1, p.CreateChannel("ch1", ChannelKind::kSendReceive,
                                     {DataElement{"data", p.GetBitsType(32)}},
                                     ChannelMetadataProto()));
  XLS_ASSERT_OK_AND_ASSIGN(
      Channel * ch2, p.CreateChannel("ch2", ChannelKind::kSendReceive,
                                     {DataElement{"data", p.GetBitsType(32)}},
                                     ChannelMetadataProto()));
  XLS_ASSERT_OK_AND_ASSIGN(
      Channel * ch3, p.CreateChannel("ch3", ChannelKind::kSendReceive,
                                     {DataElement{"data", p.GetBitsType(32)}},
                                     ChannelMetadataProto()));

  ProcBuilder b("sending_receiving", Value(UBits(42, 32)),
                /*token_name=*/"my_token", /*state_name=*/"my_state", &p);
  BValue send = b.Send(ch0, b.GetTokenParam(), {b.GetStateParam()});
  BValue receive = b.Receive(ch1, b.GetTokenParam());
  BValue pred = b.Literal(UBits(1, 1));
  BValue send_if = b.SendIf(ch2, b.GetTokenParam(), pred, {b.GetStateParam()});
  BValue receive_if = b.ReceiveIf(ch3, b.GetTokenParam(), pred);
  BValue after_all = b.AfterAll(
      {send, b.TupleIndex(receive, 0), send_if, b.TupleIndex(receive_if, 0)});
  BValue next_state =
      b.Add(b.TupleIndex(receive, 1), b.TupleIndex(receive_if, 1));

  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, b.Build(after_all, next_state));

  EXPECT_THAT(proc->NextToken(),
              m::AfterAll(m::Send(), m::TupleIndex(m::Receive()), m::SendIf(),
                          m::TupleIndex(m::ReceiveIf())));
  EXPECT_THAT(proc->NextState(), m::Add(m::TupleIndex(m::Receive()),
                                        m::TupleIndex(m::ReceiveIf())));

  EXPECT_EQ(proc->InitValue(), Value(UBits(42, 32)));
  EXPECT_EQ(proc->StateParam()->GetName(), "my_state");
  EXPECT_EQ(proc->TokenParam()->GetName(), "my_token");
  EXPECT_EQ(proc->StateType(), p.GetBitsType(32));

  EXPECT_EQ(send.node()->GetType(), p.GetTokenType());
  EXPECT_EQ(send_if.node()->GetType(), p.GetTokenType());
  EXPECT_EQ(receive.node()->GetType(), p.GetReceiveType(ch1));
  EXPECT_EQ(receive_if.node()->GetType(), p.GetReceiveType(ch3));
}

TEST(FunctionBuilderTest, WrongAddOpMethodBinOp) {
  Package p("p");
  FunctionBuilder b("f", &p);
  b.AddBinOp(Op::kEq, b.Param("x", p.GetBitsType(32)),
             b.Param("y", p.GetBitsType(32)));
  EXPECT_THAT(b.Build().status(),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Op eq is not a operation of class BinOp")));
}

TEST(FunctionBuilderTest, WrongAddOpMethodUnaryOp) {
  Package p("p");
  FunctionBuilder b("f", &p);
  b.AddUnOp(Op::kAdd, b.Param("x", p.GetBitsType(32)));
  EXPECT_THAT(b.Build().status(),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Op add is not a operation of class UnOp")));
}

TEST(FunctionBuilderTest, WrongAddOpMethodCompareOp) {
  Package p("p");
  FunctionBuilder b("f", &p);
  b.AddCompareOp(Op::kNeg, b.Param("x", p.GetBitsType(32)),
                 b.Param("y", p.GetBitsType(32)));
  EXPECT_THAT(
      b.Build().status(),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("Op neg is not a operation of class CompareOp")));
}

TEST(FunctionBuilderTest, WrongAddOpMethodNaryOp) {
  Package p("p");
  FunctionBuilder b("f", &p);
  b.AddNaryOp(Op::kNe, {b.Param("x", p.GetBitsType(32)),
                        b.Param("y", p.GetBitsType(32))});
  EXPECT_THAT(b.Build().status(),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Op ne is not a operation of class NaryOp")));
}

TEST(FunctionBuilderTest, NamedOps) {
  Package p("p");
  FunctionBuilder b("f", &p);
  BitsType* type = p.GetBitsType(7);
  BValue and_node = b.And(b.Param("a", type), b.Param("b", type),
                          /*loc=*/absl::nullopt, /*name=*/"foo");
  b.Negate(and_node, /*loc=*/absl::nullopt, /*name=*/"bar");
  XLS_ASSERT_OK_AND_ASSIGN(Function * func, b.Build());
  Node* return_value = func->return_value();
  EXPECT_EQ(return_value->op(), Op::kNeg);
  EXPECT_TRUE(return_value->HasAssignedName());
  EXPECT_EQ(return_value->GetName(), "bar");
  EXPECT_TRUE(return_value->operand(0)->HasAssignedName());
  EXPECT_EQ(return_value->operand(0)->GetName(), "foo");
}

TEST(FunctionBuilderTest, DynamicCountedForTest) {
  Package p("p");
  BitsType* int32_type = p.GetBitsType(32);
  BitsType* int16_type = p.GetBitsType(16);
  Function* body;
  {
    FunctionBuilder b("body", &p);
    auto index = b.Param("index", int32_type);
    auto accumulator = b.Param("accumulator", int32_type);
    b.Param("invariant_1", int16_type);
    b.Param("invariant_2", int16_type);
    b.Add(index, accumulator);
    XLS_ASSERT_OK_AND_ASSIGN(body, b.Build());
  }
  Function* top;
  {
    FunctionBuilder b("top", &p);
    b.DynamicCountedFor(b.Param("init", int32_type),
                        b.Param("trip_count", int16_type),
                        b.Param("stride", int16_type), body,
                        {b.Param("invariant_1", int16_type),
                         b.Param("invariant_2", int16_type)});
    XLS_ASSERT_OK_AND_ASSIGN(top, b.Build());
  }
  Node* dynamic_for = top->return_value();
  EXPECT_EQ(dynamic_for->op(), Op::kDynamicCountedFor);
  EXPECT_EQ(body->return_value()->GetType(), p.GetBitsType(32));
  EXPECT_THAT(dynamic_for,
              m::DynamicCountedFor(
                  m::Param("init"), m::Param("trip_count"), m::Param("stride"),
                  body, {m::Param("invariant_1"), m::Param("invariant_2")}));
}

}  // namespace xls
