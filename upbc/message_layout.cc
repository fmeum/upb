// Copyright (c) 2009-2021, Google LLC
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//     * Neither the name of Google LLC nor the
//       names of its contributors may be used to endorse or promote products
//       derived from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL Google LLC BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "upbc/message_layout.h"
#include "google/protobuf/descriptor.pb.h"

namespace upbc {

namespace protobuf = ::google::protobuf;

static int64_t DivRoundUp(int64_t a, int64_t b) {
  ABSL_ASSERT(a >= 0);
  ABSL_ASSERT(b > 0);
  return (a + b - 1) / b;
}

MessageLayout::Size MessageLayout::Place(
    MessageLayout::SizeAndAlign size_and_align) {
  Size offset = size_;
  offset.AlignUp(size_and_align.align);
  size_ = offset;
  size_.Add(size_and_align.size);
  //maxalign_.MaxFrom(size_and_align.align);
  maxalign_.MaxFrom(size_and_align.size);
  return offset;
}

bool MessageLayout::HasHasbit(const protobuf::FieldDescriptor* field) {
  return field->has_presence() && !field->real_containing_oneof() &&
         !field->containing_type()->options().map_entry();
}

MessageLayout::SizeAndAlign MessageLayout::SizeOf(
    const protobuf::FieldDescriptor* field) {
  if (field->is_repeated()) {
    return {{4, 8}, {4, 8}};  // Pointer to array object.
  } else {
    return SizeOfUnwrapped(field);
  }
}

MessageLayout::SizeAndAlign MessageLayout::SizeOfUnwrapped(
    const protobuf::FieldDescriptor* field) {
  switch (field->cpp_type()) {
    case protobuf::FieldDescriptor::CPPTYPE_MESSAGE:
      return {{4, 8}, {4, 8}};  // Pointer to message.
    case protobuf::FieldDescriptor::CPPTYPE_STRING:
      return {{8, 16}, {4, 8}};  // upb_strview
    case protobuf::FieldDescriptor::CPPTYPE_BOOL:
      return {{1, 1}, {1, 1}};
    case protobuf::FieldDescriptor::CPPTYPE_FLOAT:
    case protobuf::FieldDescriptor::CPPTYPE_INT32:
    case protobuf::FieldDescriptor::CPPTYPE_UINT32:
    case protobuf::FieldDescriptor::CPPTYPE_ENUM:
      return {{4, 4}, {4, 4}};
    case protobuf::FieldDescriptor::CPPTYPE_INT64:
    case protobuf::FieldDescriptor::CPPTYPE_UINT64:
    case protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
      return {{8, 8}, {8, 8}};
  }
  assert(false);
  return {{-1, -1}, {-1, -1}};
}

int64_t MessageLayout::FieldLayoutRank(const protobuf::FieldDescriptor* field) {
  // Order:
  //   1, 2, 3. primitive fields (8, 4, 1 byte)
  //   4. string fields
  //   5. submessage fields
  //   6. repeated fields
  //
  // This has the following nice properties:
  //
  //  1. padding alignment is (nearly) minimized.
  //  2. fields that might have defaults (1-4) are segregated
  //     from fields that are always zero-initialized (5-7).
  //
  // We skip oneof fields, because they are emitted in a separate pass.
  int64_t rank;
  if (field->containing_oneof()) {
    fprintf(stderr, "shouldn't have oneofs here.\n");
    abort();
  } else if (field->label() == protobuf::FieldDescriptor::LABEL_REPEATED) {
    rank = 6;
  } else {
    switch (field->cpp_type()) {
      case protobuf::FieldDescriptor::CPPTYPE_MESSAGE:
        rank = 5;
        break;
      case protobuf::FieldDescriptor::CPPTYPE_STRING:
        rank = 4;
        break;
      case protobuf::FieldDescriptor::CPPTYPE_BOOL:
        rank = 3;
        break;
      case protobuf::FieldDescriptor::CPPTYPE_FLOAT:
      case protobuf::FieldDescriptor::CPPTYPE_INT32:
      case protobuf::FieldDescriptor::CPPTYPE_UINT32:
        rank = 2;
        break;
      default:
        rank = 1;
        break;
    }
  }

  // Break ties with field number.
  return (rank << 29) | field->number();
}

void MessageLayout::ComputeLayout(const protobuf::Descriptor* descriptor) {
  size_ = Size{0, 0};
  maxalign_ = Size{8, 8};

  if (descriptor->options().map_entry()) {
    // Map entries aren't actually stored, they are only used during parsing.
    // For parsing, it helps a lot if all map entry messages have the same
    // layout.
    SizeAndAlign size{{8, 16}, {4, 8}};  // upb_strview
    field_offsets_[descriptor->FindFieldByNumber(1)] = Place(size);
    field_offsets_[descriptor->FindFieldByNumber(2)] = Place(size);
  } else {
    PlaceNonOneofFields(descriptor);
    PlaceOneofFields(descriptor);
  }

  // Align overall size up to max size.
  size_.AlignUp(maxalign_);
}

void MessageLayout::PlaceNonOneofFields(
    const protobuf::Descriptor* descriptor) {
  std::vector<const protobuf::FieldDescriptor*> field_order;
  for (int i = 0; i < descriptor->field_count(); i++) {
    const protobuf::FieldDescriptor* field = descriptor->field(i);
    if (!field->containing_oneof()) {
      field_order.push_back(descriptor->field(i));
    }
  }
  std::sort(field_order.begin(), field_order.end(),
            [](const protobuf::FieldDescriptor* a,
               const protobuf::FieldDescriptor* b) {
              return FieldLayoutRank(a) < FieldLayoutRank(b);
            });

  // Place/count hasbits.
  hasbit_count_ = 0;
  required_count_ = 0;
  for (auto field : FieldHotnessOrder(descriptor)) {
    if (HasHasbit(field)) {
      // We don't use hasbit 0, so that 0 can indicate "no presence" in the
      // table. This wastes one hasbit, but we don't worry about it for now.
      int index = ++hasbit_count_;
      hasbit_indexes_[field] = index;
      if (field->is_required()) {
        if (index > 63) {
          // This could be fixed in the decoder without too much trouble.  But
          // we expect this to be so rare that we don't worry about it for now.
          std::cerr << "upb does not support messages with more than 63 "
                       "required fields: "
                    << field->full_name() << "\n";
          exit(1);
        }
        required_count_++;
      }
    }
  }

  // Place hasbits at the beginning.
  hasbit_bytes_ = DivRoundUp(hasbit_count_, 8);
  Place(SizeAndAlign{{hasbit_bytes_, hasbit_bytes_}, {1, 1}});

  // Place non-oneof fields.
  for (auto field : field_order) {
    field_offsets_[field] = Place(SizeOf(field));
  }
}

void MessageLayout::PlaceOneofFields(const protobuf::Descriptor* descriptor) {
  std::vector<const protobuf::OneofDescriptor*> oneof_order;
  for (int i = 0; i < descriptor->oneof_decl_count(); i++) {
    oneof_order.push_back(descriptor->oneof_decl(i));
  }
  std::sort(oneof_order.begin(), oneof_order.end(),
            [](const protobuf::OneofDescriptor* a,
               const protobuf::OneofDescriptor* b) {
              return a->full_name() < b->full_name();
            });

  for (auto oneof : oneof_order) {
    SizeAndAlign oneof_maxsize{{0, 0}, {0, 0}};
    // Calculate max size.
    for (int i = 0; i < oneof->field_count(); i++) {
      oneof_maxsize.MaxFrom(SizeOf(oneof->field(i)));
    }

    // Place discriminator enum and data.
    Size data = Place(oneof_maxsize);
    Size discriminator = Place(SizeAndAlign{{4, 4}, {4, 4}});

    oneof_case_offsets_[oneof] = discriminator;

    for (int i = 0; i < oneof->field_count(); i++) {
      field_offsets_[oneof->field(i)] = data;
    }
  }
}

}  // namespace upbc
