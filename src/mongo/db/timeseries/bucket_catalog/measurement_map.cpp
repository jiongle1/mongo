/**
 *    Copyright (C) 2024-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/timeseries/bucket_catalog/measurement_map.h"
#include "mongo/bson/util/bsoncolumn.h"
#include "mongo/logv2/log.h"
#include "mongo/util/base64.h"
#include "mongo/util/testing_proctor.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo::timeseries::bucket_catalog {

MeasurementMap::MeasurementMap(TrackingContext& trackingContext)
    : _trackingContext(trackingContext),
      _builders(makeTrackedStringMap<TrackedBSONColumnBuilder>(_trackingContext)) {}

void MeasurementMap::initBuilders(BSONObj bucketDataDocWithCompressedBuilders,
                                  size_t numMeasurements) {
    for (auto&& [key, columnValue] : bucketDataDocWithCompressedBuilders) {
        int binLength = 0;
        const char* binData = columnValue.binData(binLength);

        _compressedSize += binLength;
        _builders.emplace(make_tracked_string(_trackingContext, key.data(), key.size()),
                          TrackedBSONColumnBuilder(
                              binData, binLength, _trackingContext.get().makeAllocator<void>()));
    }
    _measurementCount = numMeasurements;
    if (TestingProctor::instance().isEnabled()) {
        for (auto&& [key, columnValue] : bucketDataDocWithCompressedBuilders) {
            int binLength = 0;
            const char* binData = columnValue.binData(binLength);
            TrackingContext trackingContext;
            TrackedBSONColumnBuilder builderToCompareTo{trackingContext.makeAllocator<void>()};
            BSONColumn c(binData, binLength);
            for (auto&& elem : c) {
                builderToCompareTo.append(elem);
            }
            [[maybe_unused]] auto diff = builderToCompareTo.intermediate();
            auto it = _builders.find(key);
            bool isInternalStateCorrect = it->second.isInternalStateIdentical(builderToCompareTo);
            if (!isInternalStateCorrect) {
                LOGV2(10402,
                      "Detected incorrect internal state when reopening from following binary: ",
                      "binary"_attr = base64::encode(StringData(binData, binLength)));
            }
            invariant(isInternalStateCorrect);
        }
    }
}

std::vector<std::pair<StringData, TrackedBSONColumnBuilder::BinaryDiff>>
MeasurementMap::intermediate(int32_t& size) {
    // Remove the old compressed size.
    size -= _compressedSize;
    _compressedSize = 0;

    std::vector<std::pair<StringData, TrackedBSONColumnBuilder::BinaryDiff>> intermediates;
    for (auto& entry : _builders) {
        auto& builder = entry.second;
        auto diff = builder.intermediate();

        _compressedSize += (diff.offset() + diff.size());
        intermediates.push_back(
            {StringData(entry.first.c_str(), entry.first.size()), std::move(diff)});
    }

    size += _compressedSize;
    return intermediates;
}

void MeasurementMap::_insertNewKey(StringData key,
                                   const BSONElement& elem,
                                   TrackedBSONColumnBuilder builder) {
    builder.append(elem);
    _builders.try_emplace(make_tracked_string(_trackingContext, key.data(), key.size()),
                          std::move(builder));
}

void MeasurementMap::_fillSkipsInMissingFields(const std::set<StringData>& fieldsSeen) {
    // Fill in skips for any fields that existed in prior measurements in this bucket, but
    // weren't in this measurement.
    for (auto& entry : _builders) {
        if (fieldsSeen.contains(entry.first.c_str())) {
            continue;
        }
        entry.second.skip();
    }
}

void MeasurementMap::insertOne(std::vector<BSONElement> oneMeasurementDataFields) {
    std::set<StringData> fieldsSeen;

    for (const auto& elem : oneMeasurementDataFields) {
        StringData key = elem.fieldNameStringData();
        fieldsSeen.insert(key);

        auto builderIt = _builders.find(key);
        if (builderIt == _builders.end()) {
            TrackedBSONColumnBuilder columnBuilder{_trackingContext.get().makeAllocator<void>()};
            for (size_t i = 0; i < _measurementCount; ++i) {
                columnBuilder.skip();
            }
            _insertNewKey(key, elem, std::move(columnBuilder));
        } else {
            builderIt->second.append(elem);
        }
    }
    _measurementCount++;
    _fillSkipsInMissingFields(fieldsSeen);
}

Timestamp MeasurementMap::timeOfLastMeasurement(StringData key) const {
    auto it = _builders.find(key);
    invariant(it != _builders.end());
    return it->second.last().timestamp();
}

}  // namespace mongo::timeseries::bucket_catalog
