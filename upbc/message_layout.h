/*
 * Copyright (c) 2009-2021, Google LLC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of Google LLC nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL Google LLC BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef UPBC_MESSAGE_LAYOUT_H
#define UPBC_MESSAGE_LAYOUT_H

#include "absl/base/macros.h"
#include "absl/container/flat_hash_map.h"
#include "google/protobuf/descriptor.h"

namespace upbc {

class MessageLayout {
 public:
  struct Size {
    void Add(const Size& other) {
      size32 += other.size32;
      size64 += other.size64;
    }

    void MaxFrom(const Size& other) {
      size32 = std::max(size32, other.size32);
      size64 = std::max(size64, other.size64);
    }

    void AlignUp(const Size& align) {
      size32 = Align(size32, align.size32);
      size64 = Align(size64, align.size64);
    }

    int64_t size32;
    int64_t size64;
  };

  struct SizeAndAlign {
    Size size;
    Size align;

    void MaxFrom(const SizeAndAlign& other) {
      size.MaxFrom(other.size);
      align.MaxFrom(other.align);
    }
  };

  MessageLayout(const google::protobuf::Descriptor* descriptor) {
    ComputeLayout(descriptor);
  }

  Size GetFieldOffset(const google::protobuf::FieldDescriptor* field) const {
    return GetMapValue(field_offsets_, field);
  }

  Size GetOneofCaseOffset(
      const google::protobuf::OneofDescriptor* oneof) const {
    return GetMapValue(oneof_case_offsets_, oneof);
  }

  int GetHasbitIndex(const google::protobuf::FieldDescriptor* field) const {
    return GetMapValue(hasbit_indexes_, field);
  }

  Size message_size() const { return size_; }

  int hasbit_count() const { return hasbit_count_; }
  int hasbit_bytes() const { return hasbit_bytes_; }

  // Required fields always have the lowest hasbits.
  int required_count() const { return required_count_; }

  static bool HasHasbit(const google::protobuf::FieldDescriptor* field);
  static SizeAndAlign SizeOfUnwrapped(
      const google::protobuf::FieldDescriptor* field);

 private:
  void ComputeLayout(const google::protobuf::Descriptor* descriptor);
  void PlaceNonOneofFields(const google::protobuf::Descriptor* descriptor);
  void PlaceOneofFields(const google::protobuf::Descriptor* descriptor);
  Size Place(SizeAndAlign size_and_align);

  template <class K, class V>
  static V GetMapValue(const absl::flat_hash_map<K, V>& map, K key) {
    auto iter = map.find(key);
    if (iter == map.end()) {
      fprintf(stderr, "No value for field.\n");
      abort();
    }
    return iter->second;
  }

  static bool IsPowerOfTwo(size_t val) {
    return (val & (val - 1)) == 0;
  }

  static size_t Align(size_t val, size_t align) {
    ABSL_ASSERT(IsPowerOfTwo(align));
    return (val + align - 1) & ~(align - 1);
  }

  static SizeAndAlign SizeOf(const google::protobuf::FieldDescriptor* field);
  static int64_t FieldLayoutRank(
      const google::protobuf::FieldDescriptor* field);

  absl::flat_hash_map<const google::protobuf::FieldDescriptor*, Size>
      field_offsets_;
  absl::flat_hash_map<const google::protobuf::FieldDescriptor*, int>
      hasbit_indexes_;
  absl::flat_hash_map<const google::protobuf::OneofDescriptor*, Size>
      oneof_case_offsets_;
  Size maxalign_;
  Size size_;
  int hasbit_count_;
  int hasbit_bytes_;
  int required_count_;
};

// Returns fields in order of "hotness", eg. how frequently they appear in
// serialized payloads. Ideally this will use a profile. When we don't have
// that, we assume that fields with smaller numbers are used more frequently.
inline std::vector<const google::protobuf::FieldDescriptor*> FieldHotnessOrder(
    const google::protobuf::Descriptor* message) {
  std::vector<const google::protobuf::FieldDescriptor*> fields;
  for (int i = 0; i < message->field_count(); i++) {
    fields.push_back(message->field(i));
  }
  std::sort(fields.begin(), fields.end(),
            [](const google::protobuf::FieldDescriptor* a,
               const google::protobuf::FieldDescriptor* b) {
              return std::make_pair(!a->is_required(), a->number()) <
                     std::make_pair(!b->is_required(), b->number());
            });
  return fields;
}

}  // namespace upbc

#endif  // UPBC_MESSAGE_LAYOUT_H
