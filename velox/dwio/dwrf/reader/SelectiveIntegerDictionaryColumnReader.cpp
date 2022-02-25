/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "velox/dwio/dwrf/reader/SelectiveIntegerDictionaryColumnReader.h"

namespace facebook::velox::dwrf {
using namespace dwio::common;

SelectiveIntegerDictionaryColumnReader::SelectiveIntegerDictionaryColumnReader(
    std::shared_ptr<const TypeWithId> requestedType,
    const std::shared_ptr<const TypeWithId>& dataType,
    StripeStreams& stripe,
    common::ScanSpec* scanSpec,
    uint32_t numBytes)
    : SelectiveColumnReader(
          std::move(requestedType),
          stripe,
          scanSpec,
          dataType->type) {
  EncodingKey encodingKey{nodeType_->id, flatMapContext_.sequence};
  auto encoding = stripe.getEncoding(encodingKey);
  dictionarySize_ = encoding.dictionarysize();
  rleVersion_ = convertRleVersion(encoding.kind());
  auto data = encodingKey.forKind(proto::Stream_Kind_DATA);
  bool dataVInts = stripe.getUseVInts(data);
  dataReader_ = IntDecoder</* isSigned = */ false>::createRle(
      stripe.getStream(data, true),
      rleVersion_,
      memoryPool_,
      dataVInts,
      numBytes);

  // make a lazy dictionary initializer
  dictInit_ = stripe.getIntDictionaryInitializerForNode(
      encodingKey, numBytes, numBytes);

  auto inDictStream = stripe.getStream(
      encodingKey.forKind(proto::Stream_Kind_IN_DICTIONARY), false);
  if (inDictStream) {
    inDictionaryReader_ =
        createBooleanRleDecoder(std::move(inDictStream), encodingKey);
  }
}

uint64_t SelectiveIntegerDictionaryColumnReader::skip(uint64_t numValues) {
  numValues = ColumnReader::skip(numValues);
  dataReader_->skip(numValues);
  if (inDictionaryReader_) {
    inDictionaryReader_->skip(numValues);
  }
  return numValues;
}

void SelectiveIntegerDictionaryColumnReader::read(
    vector_size_t offset,
    RowSet rows,
    const uint64_t* incomingNulls) {
  VELOX_WIDTH_DISPATCH(
      sizeOfIntKind(type_->kind()), prepareRead, offset, rows, incomingNulls);
  auto end = rows.back() + 1;
  const auto* rawNulls =
      nullsInReadRange_ ? nullsInReadRange_->as<uint64_t>() : nullptr;

  // read the stream of booleans indicating whether a given data entry
  // is an offset or a literal value.
  if (inDictionaryReader_) {
    bool isBulk = useBulkPath();
    int32_t numFlags = (isBulk && nullsInReadRange_)
        ? bits::countNonNulls(nullsInReadRange_->as<uint64_t>(), 0, end)
        : end;
    ensureCapacity<uint64_t>(
        inDictionary_, bits::nwords(numFlags), &memoryPool_);
    inDictionaryReader_->next(
        inDictionary_->asMutable<char>(),
        numFlags,
        isBulk ? nullptr : rawNulls);
  }

  // lazy load dictionary only when it's needed
  ensureInitialized();

  bool isDense = rows.back() == rows.size() - 1;
  common::Filter* filter = scanSpec_->filter();
  if (scanSpec_->keepValues()) {
    if (scanSpec_->valueHook()) {
      if (isDense) {
        processValueHook<true>(rows, scanSpec_->valueHook());
      } else {
        processValueHook<false>(rows, scanSpec_->valueHook());
      }
      return;
    }
    if (isDense) {
      processFilter<true>(filter, ExtractToReader(this), rows);
    } else {
      processFilter<false>(filter, ExtractToReader(this), rows);
    }
  } else {
    if (isDense) {
      processFilter<true>(filter, DropValues(), rows);
    } else {
      processFilter<false>(filter, DropValues(), rows);
    }
  }
}

void SelectiveIntegerDictionaryColumnReader::ensureInitialized() {
  if (LIKELY(initialized_)) {
    return;
  }

  Timer timer;
  dictionary_ = dictInit_();
  // Make sure there is a cache even for an empty dictionary because
  // of asan failure when preparing a gather with all lanes masked
  // out.
  filterCache_.resize(std::max<int32_t>(1, dictionarySize_));
  simd::memset(filterCache_.data(), FilterResult::kUnknown, dictionarySize_);
  initialized_ = true;
  initTimeClocks_ = timer.elapsedClocks();
}

} // namespace facebook::velox::dwrf
