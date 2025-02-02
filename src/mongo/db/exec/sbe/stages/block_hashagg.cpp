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

#include "mongo/db/exec/sbe/stages/block_hashagg.h"

#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/stages/hashagg_base.h"
#include "mongo/db/exec/sbe/values/block_interface.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"


namespace mongo {
namespace sbe {
namespace {
bool allFalse(std::pair<value::TypeTags, value::Value> bitset) {
    invariant(bitset.first == value::TypeTags::valueBlock);
    // TODO SERVER-85739 use special cases for different types of blocks.
    const auto& deblocked = value::bitcastTo<value::ValueBlock*>(bitset.second)->extract();
    for (size_t i = 0; i < deblocked.count(); i++) {
        invariant(deblocked[i].first == value::TypeTags::Boolean);
        if (value::bitcastTo<bool>(deblocked[i].second)) {
            return false;
        }
    }
    return true;
}

// Given a vector of partition IDs, and partition ID, create a bitset indicating whether each
// element in the vector matches the given partition ID.
std::unique_ptr<value::ValueBlock> computeBitmapForPartition(
    const std::vector<size_t>& partitionMap, size_t partition) {
    std::vector<bool> bitmap;
    bitmap.resize(partitionMap.size());
    for (size_t i = 0; i < partitionMap.size(); i++) {
        bitmap[i] = (partitionMap[i] == partition);
    }
    return std::make_unique<value::BoolBlock>(std::move(bitmap));
}

// Takes two bitsets of equal size, one as a ValueBlock and one as a tag/val, and returns a bitset
// of the same size with elements pairwise ANDed together.
std::unique_ptr<value::ValueBlock> bitAnd(value::ValueBlock* bitset1, value::ValueBlock* bitset2) {
    // TODO SERVER-85738 Implement efficient bitAnd operation on blocks.
    auto vals1 = bitset1->extract();
    auto vals2 = bitset2->extract();
    invariant(vals1.count() == vals2.count());

    std::vector<bool> vecResult;
    vecResult.resize(vals1.count());
    for (size_t i = 0; i < vals1.count(); i++) {
        invariant(vals1[i].first == value::TypeTags::Boolean &&
                  vals2[i].first == value::TypeTags::Boolean);
        vecResult[i] =
            value::bitcastTo<bool>(vals1[i].second) && value::bitcastTo<bool>(vals2[i].second);
    }
    return std::make_unique<value::BoolBlock>(std::move(vecResult));
}

/*
 * Block that holds a view of a single value. It does not take ownership of the given value. This is
 * used because the block accumulators expect block inputs, but in some cases we may need to provide
 * is scalars that we do not own.
 * Used only for BlockHashAgg.
 */
class SingletonViewBlock final : public value::ValueBlock {
public:
    SingletonViewBlock() {}

    SingletonViewBlock(value::TypeTags tag, value::Value val) : _tag(tag), _val(val) {}

    void setTagVal(std::pair<value::TypeTags, value::Value> tagVal) {
        _tag = tagVal.first;
        _val = tagVal.second;
    }

    std::unique_ptr<ValueBlock> clone() const override {
        return std::make_unique<SingletonViewBlock>(_tag, _val);
    }

    boost::optional<size_t> tryCount() const override {
        return 1;
    }

    value::DeblockedTagVals deblock(
        boost::optional<value::DeblockedTagValStorage>& storage) override {
        return {1, &_tag, &_val};
    }

private:
    value::TypeTags _tag;
    value::Value _val;
};

struct SpanHasher {
    SpanHasher() {}

    size_t operator()(const std::span<size_t>& idxs) const {
        return boost::hash_range(idxs.begin(), idxs.end());
    }
};

struct SpanEq {
    SpanEq() {}

    bool operator()(const std::span<size_t>& lhs, const std::span<size_t>& rhs) const {
        invariant(lhs.size() == rhs.size());

        for (size_t i = 0; i < lhs.size(); ++i) {
            if (lhs[i] != rhs[i]) {
                return false;
            }
        }

        return true;
    }
};

using KeyTableType = stdx::
    unordered_map<std::span<size_t>, std::pair<size_t, value::MaterializedRow>, SpanHasher, SpanEq>;
}  // namespace

BlockHashAggStage::BlockHashAggStage(std::unique_ptr<PlanStage> input,
                                     value::SlotVector groupSlotIds,
                                     value::SlotId blockBitsetInSlotId,
                                     value::SlotVector blockDataInSlotIds,
                                     value::SlotVector accumulatorDataSlotIds,
                                     value::SlotId accumulatorBitsetSlotId,
                                     BlockAndRowAggs aggs,
                                     bool allowDiskUse,
                                     SlotExprPairVector mergingExprs,
                                     PlanYieldPolicy* yieldPolicy,
                                     PlanNodeId planNodeId,
                                     bool participateInTrialRunTracking,
                                     bool forceIncreasedSpilling)
    : HashAggBaseStage("block_group"_sd,
                       yieldPolicy,
                       planNodeId,
                       nullptr,
                       participateInTrialRunTracking,
                       allowDiskUse,
                       forceIncreasedSpilling),
      _groupSlots(groupSlotIds),
      _blockBitsetInSlotId(blockBitsetInSlotId),
      _blockDataInSlotIds(std::move(blockDataInSlotIds)),
      _accumulatorBitsetSlotId(accumulatorBitsetSlotId),
      _accumulatorDataSlotIds(std::move(accumulatorDataSlotIds)),
      _blockRowAggs(std::move(aggs)),
      _mergingExprs(std::move(mergingExprs)) {
    tassert(8780600,
            "Expected 'blockDataInSlotIds' and 'accumulatorDataSlotIds' to have the same size",
            _blockDataInSlotIds.size() == _accumulatorDataSlotIds.size());

    _children.emplace_back(std::move(input));

    _outIdBlocks.resize(_groupSlots.size());
    _outAggBlocks.resize(_blockRowAggs.size());
    _blockDataInAccessors.resize(_blockDataInSlotIds.size());
    _accumulatorDataAccessors.resize(_accumulatorDataSlotIds.size());
    _gbBlocks.resize(_groupSlots.size());
    _dataBlocks.resize(_blockDataInSlotIds.size());
    _deblockedTokens.resize(_groupSlots.size());

    _tokenInfos.reserve(_groupSlots.size());

    if (_allowDiskUse) {
        tassert(8780601,
                "Disk use enabled for HashAggStage but incorrect number of merging expresssions",
                _blockRowAggs.size() == _mergingExprs.size());
    }
}

BlockHashAggStage::~BlockHashAggStage() {
    groupCounters.incrementGroupCounters(_specificStats.spills,
                                         _specificStats.spilledDataStorageSize,
                                         _specificStats.spilledRecords);
}

std::unique_ptr<PlanStage> BlockHashAggStage::clone() const {
    BlockAndRowAggs blockRowAggs;
    for (const auto& [slot, aggs] : _blockRowAggs) {
        blockRowAggs.emplace_back(
            slot, BlockRowAccumulators{aggs.blockAgg->clone(), aggs.rowAgg->clone()});
    }

    SlotExprPairVector mergingExprs;
    mergingExprs.reserve(_mergingExprs.size());

    for (auto&& [k, v] : _mergingExprs) {
        mergingExprs.push_back({k, v->clone()});
    }

    return std::make_unique<BlockHashAggStage>(_children[0]->clone(),
                                               _groupSlots,
                                               _blockBitsetInSlotId,
                                               _blockDataInSlotIds,
                                               _accumulatorDataSlotIds,
                                               _accumulatorBitsetSlotId,
                                               std::move(blockRowAggs),
                                               _allowDiskUse,
                                               std::move(mergingExprs),
                                               _yieldPolicy,
                                               _commonStats.nodeId,
                                               participateInTrialRunTracking(),
                                               _forceIncreasedSpilling);
}

void BlockHashAggStage::prepare(CompileCtx& ctx) {
    _children[0]->prepare(ctx);

    value::SlotSet dupCheck;
    auto throwIfDupSlot = [&dupCheck](boost::optional<value::SlotId> slot) {
        if (slot) {
            auto [_, inserted] = dupCheck.emplace(*slot);
            tassert(7953400, "duplicate slot id", inserted);
        }
    };

    _blockBitsetInAccessor = _children[0]->getAccessor(ctx, _blockBitsetInSlotId);
    invariant(_blockBitsetInAccessor);

    for (size_t i = 0; i < _blockDataInSlotIds.size(); i++) {
        _blockDataInAccessors[i] = _children[0]->getAccessor(ctx, _blockDataInSlotIds[i]);
        invariant(_blockDataInAccessors[i]);
    }
    throwIfDupSlot(_blockBitsetInSlotId);

    _outAccessorsMap.reserve(_groupSlots.size() + _blockRowAggs.size());
    _outIdBlockAccessors.resize(_groupSlots.size());
    _outAggBlockAccessors.resize(_blockRowAggs.size());

    if (_allowDiskUse) {
        _spilledAccessors.resize(_blockRowAggs.size());
    }

    for (size_t i = 0; i < _groupSlots.size(); ++i) {
        auto& slot = _groupSlots[i];
        throwIfDupSlot(slot);

        _idInAccessors.emplace_back(_children[0]->getAccessor(ctx, slot));

        // Construct accessor for obtaining the key values from the hash table.
        _idHtAccessors.emplace_back(std::make_unique<HashKeyAccessor>(_htIt, i));

        _outAccessorsMap[slot] = &_outIdBlockAccessors[i];
    }

    // Change the agg slot accessors to point to the blocks.
    for (size_t i = 0; i < _outAggBlocks.size(); ++i) {
        auto& outBlock = _outAggBlocks[i];
        _outAggBlockAccessors[i].reset(
            false, value::TypeTags::valueBlock, value::bitcastFrom<value::ValueBlock*>(&outBlock));
    }

    for (size_t i = 0; i < _blockRowAggs.size(); ++i) {
        auto& [slot, _] = _blockRowAggs[i];
        throwIfDupSlot(slot);

        _rowAggHtAccessors.emplace_back(std::make_unique<HashAggAccessor>(_htIt, i));
        _rowAggRSAccessors.emplace_back(std::make_unique<value::OwnedValueAccessor>());
        _rowAggAccessors.emplace_back(
            std::make_unique<value::SwitchAccessor>(std::vector<value::SlotAccessor*>{
                _rowAggHtAccessors.back().get(), _rowAggRSAccessors.back().get()}));
        _outAccessorsMap[slot] = &_outAggBlockAccessors[i];

        if (_allowDiskUse) {
            value::SlotId spillSlot = _mergingExprs[i].first;
            throwIfDupSlot(spillSlot);

            _spilledAccessorMap[spillSlot] = &_spilledAccessors[i];
        }
    }

    // Initialize '_accumulatorDataAccessorMap'.
    for (size_t i = 0; i < _accumulatorDataSlotIds.size(); ++i) {
        auto slot = _accumulatorDataSlotIds[i];
        throwIfDupSlot(slot);

        _accumulatorDataAccessorMap[slot] = &_accumulatorDataAccessors[i];
    }

    // Compile 'blockAggs', 'rowAggs', and 'mergingExprs'.
    for (size_t i = 0; i < _blockRowAggs.size(); ++i) {
        auto& [_, aggs] = _blockRowAggs[i];

        ctx.root = this;
        ctx.aggExpression = true;
        ctx.accumulator = _rowAggAccessors[i].get();

        _blockLevelAggCodes.emplace_back(aggs.blockAgg->compile(ctx));
        _aggCodes.emplace_back(aggs.rowAgg->compile(ctx));

        if (_allowDiskUse) {
            std::unique_ptr<EExpression>& mergingExpr = _mergingExprs[i].second;
            _mergingExprCodes.emplace_back(mergingExpr->compile(ctx));
        }

        ctx.aggExpression = false;
    }

    _compiled = true;
}

value::SlotAccessor* BlockHashAggStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (slot == _blockBitsetInSlotId) {
        // Re-map the bitset slot to our output bitset accessor.
        return &_blockBitsetOutAccessor;
    }

    if (_compiled) {
        if (_outAccessorsMap.count(slot)) {
            return _outAccessorsMap[slot];
        }
    } else {
        if (_accumulatorBitsetSlotId == slot) {
            return &_accumulatorBitsetAccessor;
        }
        if (auto it = _accumulatorDataAccessorMap.find(slot);
            it != _accumulatorDataAccessorMap.end()) {
            return it->second;
        }
        if (auto it = _spilledAccessorMap.find(slot); it != _spilledAccessorMap.end()) {
            return it->second;
        }
    }

    return _children[0]->getAccessor(ctx, slot);
}

void BlockHashAggStage::executeBlockLevelAccumulatorCode(const value::MaterializedRow& key) {
    // If all bits are false, there's no work to do. We don't want to make an erroneous
    // entry in our hash map.
    if (allFalse(_accumulatorBitsetAccessor.getViewOfValue())) {
        return;
    }

    _htIt = _ht->find(key);
    if (_htIt == _ht->end()) {
        // New key we haven't seen before.
        value::MaterializedRow ownedKey = key;
        ownedKey.makeOwned();

        auto [it, _] = _ht->emplace(std::move(ownedKey), value::MaterializedRow{0});
        // Initialize accumulators.
        it->second.resize(_rowAggAccessors.size());
        _htIt = it;
    }

    // Run the block level accumulators.
    for (size_t i = 0; i < _blockLevelAggCodes.size(); ++i) {
        auto [owned, tag, val] = _bytecode.run(_blockLevelAggCodes[i].get());
        _rowAggHtAccessors[i]->reset(owned, tag, val);
    }
}

void BlockHashAggStage::executeRowLevelAccumulatorCode(
    const value::DeblockedTagVals& extractedBitmap,
    const std::vector<value::DeblockedTagVals>& extractedGbs,
    const std::vector<value::DeblockedTagVals>& extractedData) {

    boost::optional<value::MaterializedRow> key;

    for (size_t blockIndex = 0; blockIndex < _currentBlockSize; ++blockIndex) {
        auto [bitTag, bitVal] = extractedBitmap[blockIndex];
        invariant(bitTag == value::TypeTags::Boolean);

        if (!value::bitcastTo<bool>(bitVal)) {
            continue;
        }

        if (!key) {
            key.emplace(extractedGbs.size());
        }

        for (size_t i = 0; i < extractedGbs.size(); ++i) {
            auto [idTag, idVal] = extractedGbs[i][blockIndex];
            key->reset(i, false, idTag, idVal);
        }

        // Set '_htIt' to point to the entry for 'key' in '_ht'.
        _htIt = _ht->find(*key);

        if (_htIt == _ht->end()) {
            // New key we haven't seen before.
            key->makeOwned();
            auto [it, _] = _ht->emplace(std::move(*key), value::MaterializedRow{0});
            key = boost::none;

            // Initialize accumulators.
            it->second.resize(_rowAggAccessors.size());
            _htIt = it;
        }

        // Set '_accumulatorDataAccessors' to the input values for the current 'blockIndex'.
        for (size_t i = 0; i < extractedData.size(); ++i) {
            auto [tag, val] = extractedData[i][blockIndex];
            _accumulatorDataAccessors[i].reset(tag, val);
        }

        // Run each row level accumulator.
        for (size_t i = 0; i < _aggCodes.size(); ++i) {
            auto [rowOwned, rowTag, rowVal] = _bytecode.run(_aggCodes[i].get());
            _rowAggHtAccessors[i]->reset(rowOwned, rowTag, rowVal);
        }
    }
}

void BlockHashAggStage::runAccumulatorsTokenized(const TokenizedKeys& tokenizedKeys) {
    invariant(_blockBitsetInAccessor);
    auto [bitmapInTag, bitmapInVal] = _blockBitsetInAccessor->getViewOfValue();
    invariant(bitmapInTag == value::TypeTags::valueBlock);

    // Set '_accumulatorDataAccessors' to the input value blocks.
    for (size_t i = 0; i < _dataBlocks.size(); ++i) {
        _accumulatorDataAccessors[i].reset(value::TypeTags::valueBlock,
                                           value::bitcastFrom<value::ValueBlock*>(_dataBlocks[i]));
    }

    // Process the accumulators for each partition rather than one element at a time.
    for (size_t partition = 0; partition < tokenizedKeys.keys.size(); ++partition) {
        // The accumulators use `_accumulatorBitsetAccessor` to determine which values to
        // accumulate. If we have multiple partitions, we need some additional logic to
        // indicate which partition we're processing.
        // TODO SERVER-85739 we can avoid allocating a new bitset for every input. We
        // can potentially reuse the same bitset. It also might not be worth the
        // additional code complexity.
        if (tokenizedKeys.keys.size() > 1) {
            // Combine the partition bitmap and input bitmap using bitAnd().
            auto partitionBitset = computeBitmapForPartition(tokenizedKeys.idxs, partition);
            auto accBitset = bitAnd(partitionBitset.get(), _bitmapBlock);

            _accumulatorBitsetAccessor.reset(
                true,
                value::TypeTags::valueBlock,
                value::bitcastFrom<value::ValueBlock*>(accBitset.release()));
        } else {
            // The partition bitmap would be all 1s if we computed it, so we can just use
            // the input bitmap in this case.
            _accumulatorBitsetAccessor.reset(false,
                                             value::TypeTags::valueBlock,
                                             value::bitcastFrom<value::ValueBlock*>(_bitmapBlock));
        }

        executeBlockLevelAccumulatorCode(tokenizedKeys.keys[partition]);
    }
}

void BlockHashAggStage::runAccumulatorsElementWise() {
    // Extract the bitmap.
    value::DeblockedTagVals extractedBitmap = _bitmapBlock->extract();

    // Extract the group bys.
    std::vector<value::DeblockedTagVals> extractedGbs;
    extractedGbs.resize(_gbBlocks.size());
    for (size_t i = 0; i < _gbBlocks.size(); ++i) {
        extractedGbs[i] = _gbBlocks[i]->extract();
    }

    // Extract each data block into this array for when we process them element-wise.
    std::vector<value::DeblockedTagVals> extractedData;
    extractedData.resize(_dataBlocks.size());
    for (size_t i = 0; i < _dataBlocks.size(); i++) {
        extractedData[i] = _dataBlocks[i]->extract();
    }

    // Call executeRowLevelAccumulatorCode() to run the row accumulators.
    executeRowLevelAccumulatorCode(extractedBitmap, extractedGbs, extractedData);

    _accumulatorBitsetAccessor.reset(false, value::TypeTags::Nothing, 0);
}

boost::optional<BlockHashAggStage::TokenizedKeys> BlockHashAggStage::tokenizeTokenInfos(
    const std::vector<value::TokenizedBlock>& tokenInfos,
    const std::vector<value::DeblockedTagVals>& deblockedTokens) {
    invariant(!tokenInfos.empty());

    // compoundKeys is an N x M vector, where N is the number of elements in the input blocks,
    // and M is the number of input blocks.
    std::vector<size_t> compoundKeys(tokenInfos[0].idxs.size() * tokenInfos.size());

    // All input blocks must be the same size, enforced by an invariant in open().
    size_t ckIdx = 0;
    for (size_t blockIdx = 0; blockIdx < tokenInfos[0].idxs.size(); ++blockIdx) {
        for (size_t keyIdx = 0; keyIdx < tokenInfos.size(); ++keyIdx) {
            compoundKeys[ckIdx++] = tokenInfos[keyIdx].idxs[blockIdx];
        }
    }

    auto keyMap = KeyTableType();
    size_t uniqueCount = 0;
    std::vector<value::MaterializedRow> keys;
    std::vector<size_t> idxs(tokenInfos[0].idxs.size(), 0);
    for (size_t blockIdx = 0; blockIdx < tokenInfos[0].idxs.size(); ++blockIdx) {
        // Create an empty key that we will populate with the corresponding tokens for each element
        // in the key.
        value::MaterializedRow key{tokenInfos.size()};
        std::span<size_t> htKey{&compoundKeys[blockIdx * tokenInfos.size()], tokenInfos.size()};
        auto [it, inserted] = keyMap.emplace(htKey, std::pair(uniqueCount, key));
        if (inserted) {
            uniqueCount++;
            if (uniqueCount > kMaxNumPartitionsForTokenizedPath) {
                // We've seen more "partitions" for this block than we are willing to process in
                // the Tokenized path, so we will exit early and run the accumulators element
                // wise.
                return boost::none;
            }

            for (size_t keyIdx = 0; keyIdx < tokenInfos.size(); ++keyIdx) {
                size_t idx = (blockIdx * tokenInfos.size()) /* rowIdx */ + keyIdx /* colIdx */;
                auto [tag, val] = deblockedTokens[keyIdx][compoundKeys[idx]];
                // Update the key element at keyIdx with the corresponding token.
                it->second.second.reset(keyIdx, false, tag, val);
            }
            // Now that the full key is materialized, insert it into the vector of keys.
            keys.push_back(it->second.second);
        }
        idxs[blockIdx] = it->second.first;
    }

    return BlockHashAggStage::TokenizedKeys{std::move(keys), std::move(idxs)};
}

boost::optional<BlockHashAggStage::TokenizedKeys> BlockHashAggStage::tryTokenizeGbs() {
    // Populate '_tokenInfos' and '_deblockedTokens'.
    _tokenInfos.clear();

    for (size_t i = 0; i < _gbBlocks.size(); ++i) {
        _tokenInfos.push_back(_gbBlocks[i]->tokenize());

        tassert(8608600,
                "All input blocks must be the same size",
                _tokenInfos[i].idxs.size() == _currentBlockSize);

        _deblockedTokens[i] = _tokenInfos[i].tokens->extract();
    }

    // Combine the TokenizedBlocks for each input key, combine them into compound keys, tokenize
    // these compound keys, and then return the result.
    return tokenizeTokenInfos(_tokenInfos, _deblockedTokens);
}

void BlockHashAggStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));
    _children[0]->open(reOpen);
    _commonStats.opens++;

    _ht.emplace();

    for (auto& aggAccessor : _rowAggAccessors) {
        aggAccessor->setIndex(0);
    }
    if (_recordStore) {
        _recordStore->resetCursor(_opCtx, _rsCursor);
    }

    if (reOpen) {
        _done = false;
    }

    invariant(_blockBitsetInAccessor);

    MemoryCheckData memoryCheckData;

    while (PlanState::ADVANCED == _children[0]->getNext()) {
        // Update '_bitmapBlock' and '_currentBlockSize'.
        auto [bitmapInTag, bitmapInVal] = _blockBitsetInAccessor->getViewOfValue();
        invariant(bitmapInTag == value::TypeTags::valueBlock);

        _bitmapBlock = value::bitcastTo<value::ValueBlock*>(bitmapInVal);
        _currentBlockSize = _bitmapBlock->count();

        // Update '_gbBlocks' and '_dataBlocks'.
        for (size_t i = 0; i < _idInAccessors.size(); ++i) {
            auto [tag, val] = _idInAccessors[i]->getViewOfValue();
            _gbBlocks[i] = tag == value::TypeTags::valueBlock ? value::getValueBlock(val)
                                                              : makeMonoBlock(tag, val);
        }

        for (size_t i = 0; i < _blockDataInAccessors.size(); i++) {
            auto [tag, val] = _blockDataInAccessors[i]->getViewOfValue();
            _dataBlocks[i] = tag == value::TypeTags::valueBlock ? value::getValueBlock(val)
                                                                : makeMonoBlock(tag, val);
        }

        // Try to generate tokenized group-by keys.
        auto tokenizedKeys = tryTokenizeGbs();

        if (tokenizedKeys) {
            // If we generated tokenized group-by keys successfully, call runAccumulatorsTokenized()
            // to run the block-level accumulators.
            runAccumulatorsTokenized(*tokenizedKeys);
        } else {
            // If tryTokenizeGbs() returned boost::none, call runAccumulatorsElementWise() to
            // deblock everything and run the row-level accumulators.
            runAccumulatorsElementWise();
        }

        if (!_ht->empty()) {
            if (_forceIncreasedSpilling) {
                // Spill for every row that appears in the hash table.
                spill(memoryCheckData);
            } else {
                // Estimates how much memory is being used. If we estimate that the hash table
                // exceeds the allotted memory budget, its contents are spilled to the
                // '_recordStore' and '_ht' is cleared.
                checkMemoryUsageAndSpillIfNecessary(memoryCheckData, false);
            }
        }

        trackResult();

        // We no longer need any of the TokenizedBlocks or MonoBlocks that we created during this
        // iteration, so we can discard them now.
        _tokenInfos.clear();
        _monoBlocks.clear();
    }

    // If we spilled at any point while consuming the input, then do one final spill to write
    // any leftover contents of '_ht' to the record store. That way, when recovering the input
    // from the record store and merging partial aggregates we don't have to worry about the
    // possibility of some of the data being in the hash table and some being in the record
    // store.
    if (_recordStore) {
        if (!_ht->empty()) {
            spill(memoryCheckData);
        }

        _specificStats.spilledDataStorageSize = _recordStore->rs()->storageSize(_opCtx);

        // Establish a cursor, positioned at the beginning of the record store.
        _rsCursor = _recordStore->getCursor(_opCtx);
    }

    _accumulatorBitsetAccessor.reset(false, value::TypeTags::Nothing, 0);
    _htIt = _ht->end();

    for (auto&& aggAccessor : _rowAggAccessors) {
        if (_recordStore) {
            aggAccessor->setIndex(1);
        } else {
            aggAccessor->setIndex(0);
        }
    }
}

boost::optional<value::MaterializedRow> BlockHashAggStage::getNextSpilledHelper() {
    auto recoverSpilledRecord = [&](const Record& record) {
        return deserializeSpilledRecord(record, _groupSlots.size(), _stashedBuffer);
    };

    for (size_t idx = 0; idx < _aggCodes.size(); ++idx) {
        _rowAggRSAccessors[idx]->reset(false, value::TypeTags::Nothing, 0);
    }

    // Take a spilled row and merge it with the current accumulated value.
    auto processRow = [&](const value::MaterializedRow& spilledAggRow) {
        invariant(spilledAggRow.size() == _outAggBlocks.size());

        for (size_t idx = 0; idx < _aggCodes.size(); ++idx) {
            auto [spilledTag, spilledVal] = spilledAggRow.getViewOfValue(idx);
            _spilledAccessors[idx].reset(spilledTag, spilledVal);

            auto [rowOwned, rowTag, rowVal] = _bytecode.run(_mergingExprCodes[idx].get());
            _rowAggRSAccessors[idx]->reset(rowOwned, rowTag, rowVal);
        }
    };

    value::MaterializedRow firstKey{0};
    // If we have a stashed row from last time, use that first. Otherwise ask the record store for
    // the next value and process all the data for that key.
    if (_stashedNextRow) {
        firstKey = std::move(_stashedNextRow->first);
        processRow(_stashedNextRow->second);
        _stashedNextRow = boost::none;
    } else {
        auto nextRecord = _rsCursor->next();
        if (!nextRecord) {
            return boost::none;
        }
        // We are just starting the process of merging the spilled file segments.
        auto firstRecoveredRow = recoverSpilledRecord(*nextRecord);
        firstKey = std::move(firstRecoveredRow.first);
        processRow(firstRecoveredRow.second);
    }

    // Find additional partial aggregates for the same key and merge them in order to compute the
    // final output.
    _currentBuffer = std::move(_stashedBuffer);
    for (auto nextRecord = _rsCursor->next(); nextRecord; nextRecord = _rsCursor->next()) {
        auto recoveredRow = recoverSpilledRecord(*nextRecord);
        // If we found a different key, then we're done accumulating the current key. Since there's
        // no peek API, we have to stash `recoveredRow` for next time.
        if (!value::MaterializedRowEq()(recoveredRow.first, firstKey)) {
            _stashedNextRow = std::move(recoveredRow);
            return {std::move(firstKey)};
        }

        // Merge in the new partial aggregate values.
        processRow(recoveredRow.second);
    }

    return {std::move(firstKey)};
}

PlanState BlockHashAggStage::getNextSpilled() {

    size_t resultIdx = 0;
    for (; resultIdx < kBlockOutSize; resultIdx++) {
        auto nextKey = getNextSpilledHelper();
        // If we have a key, add the value to our result. If not, break because we won't get anymore
        // values from the record store.
        if (nextKey) {
            invariant(nextKey->size() == _outIdBlocks.size());
            for (size_t i = 0; i < nextKey->size(); i++) {
                auto [keyComponentTag, keyComponentVal] = nextKey->getViewOfValue(i);
                _outIdBlocks[i].push_back(value::copyValue(keyComponentTag, keyComponentVal));
            }
            for (size_t i = 0; i < _outAggBlocks.size(); ++i) {
                auto [accTag, accVal] = _rowAggRSAccessors[i]->getViewOfValue();
                _outAggBlocks[i].push_back(value::copyValue(accTag, accVal));
            }
        } else {
            break;
        }
    }

    // If we didn't put any new values in the blocks, we must have no more spilled values.
    if (resultIdx == 0) {
        return trackPlanState(PlanState::IS_EOF);
    }
    populateBitmapSlot(resultIdx);
    return trackPlanState(PlanState::ADVANCED);
}

PlanState BlockHashAggStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));
    checkForInterruptAndYield(_opCtx);

    size_t idx = 0;
    for (auto& b : _outIdBlocks) {
        b.clear();
        b.reserve(kBlockOutSize);
        _outIdBlockAccessors[idx++].reset(
            false, value::TypeTags::valueBlock, value::bitcastFrom<value::ValueBlock*>(&b));
    }

    for (auto& b : _outAggBlocks) {
        b.clear();
        b.reserve(kBlockOutSize);
    }

    // If we've spilled, then we need to produce the output by merging the spilled segments from the
    // spill file.
    if (_recordStore) {
        return getNextSpilled();
    }

    // When we return, populate our bitmap slot with an block of all 1s, with size equal to the
    // number of rows in the block we produce.
    size_t numRows = 0;
    ON_BLOCK_EXIT([&]() { populateBitmapSlot(numRows); });

    while (numRows < kBlockOutSize) {
        if (_htIt == _ht->end()) {
            _htIt = _ht->begin();
        } else {
            ++_htIt;
        }

        if (_done) {
            return trackPlanState(PlanState::IS_EOF);
        }

        if (_htIt == _ht->end()) {
            _done = true;
            if (numRows == 0) {
                return trackPlanState(PlanState::IS_EOF);
            } else {
                return trackPlanState(PlanState::ADVANCED);
            }
        }

        invariant(_outAggBlocks.size() == _outAggBlockAccessors.size());
        invariant(_outAggBlocks.size() == _rowAggAccessors.size());

        // Copy the key from the current element in the HT into the out blocks.
        idx = 0;
        for (auto& acc : _idHtAccessors) {
            auto [t, v] = acc->copyOrMoveValue();
            _outIdBlocks[idx++].push_back(t, v);
        }

        // Copy the values from the current element in the HT into the out block.
        idx = 0;
        for (auto& acc : _rowAggHtAccessors) {
            auto [t, v] = acc->copyOrMoveValue();
            _outAggBlocks[idx++].push_back(t, v);
        }

        ++numRows;
    }

    return trackPlanState(PlanState::ADVANCED);
}

std::unique_ptr<PlanStageStats> BlockHashAggStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    ret->specific = std::make_unique<HashAggStats>(_specificStats);

    if (includeDebugInfo) {
        DebugPrinter printer;
        BSONObjBuilder bob;

        bob.append("groupBySlots", _groupSlots.begin(), _groupSlots.end());

        {
            BSONObjBuilder blockExprBob(bob.subobjStart("blockExpressions"));
            for (auto&& [slot, aggs] : _blockRowAggs) {
                blockExprBob.append(str::stream() << slot,
                                    printer.print(aggs.blockAgg->debugPrint()));
            }
        }

        {
            BSONObjBuilder rowExprBob(bob.subobjStart("rowExpressions"));
            for (auto&& [slot, aggs] : _blockRowAggs) {
                rowExprBob.append(str::stream() << slot, printer.print(aggs.rowAgg->debugPrint()));
            }
        }

        bob.append(
            "accumulatorDataSlots", _accumulatorDataSlotIds.begin(), _accumulatorDataSlotIds.end());

        if (!_mergingExprs.empty()) {
            BSONObjBuilder nestedBuilder{bob.subobjStart("mergingExprs")};
            for (auto&& [slot, expr] : _mergingExprs) {
                nestedBuilder.append(str::stream() << slot, printer.print(expr->debugPrint()));
            }
        }

        ret->debugInfo = bob.obj();
    }

    ret->children.emplace_back(_children[0]->getStats(includeDebugInfo));
    return ret;
}

const SpecificStats* BlockHashAggStage::getSpecificStats() const {
    return &_specificStats;
}

HashAggStats* BlockHashAggStage::getHashAggStats() {
    return &_specificStats;
}

void BlockHashAggStage::close() {
    auto optTimer(getOptTimer(_opCtx));

    trackClose();
    _children[0]->close();

    _ht = boost::none;
    if (_recordStore && _opCtx) {
        _recordStore->resetCursor(_opCtx, _rsCursor);
    }
    _rsCursor.reset();
    _recordStore.reset();
    _stashedNextRow = boost::none;

    _currentBlockSize = 0;
    _bitmapBlock = nullptr;
    _tokenInfos.clear();
    _monoBlocks.clear();

    _children[0]->close();
}

std::vector<DebugPrinter::Block> BlockHashAggStage::debugPrint() const {
    auto ret = PlanStage::debugPrint();

    ret.emplace_back(DebugPrinter::Block("bitset ="));
    DebugPrinter::addIdentifier(ret, _blockBitsetInSlotId);

    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _groupSlots.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, _groupSlots[idx]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    // Print the row-level and block-level accessors.
    for (bool blockExprPrint : {true, false}) {
        ret.emplace_back(DebugPrinter::Block("[`"));
        bool first = true;
        for (auto&& [slot, aggs] : _blockRowAggs) {
            if (!first) {
                ret.emplace_back(DebugPrinter::Block("`,"));
            }

            DebugPrinter::addIdentifier(ret, slot);
            ret.emplace_back("=");
            const auto& aggExpr = blockExprPrint ? aggs.blockAgg : aggs.rowAgg;
            DebugPrinter::addBlocks(ret, aggExpr->debugPrint());
            first = false;
        }
        ret.emplace_back("`]");
    }

    {
        bool first = true;
        ret.emplace_back(DebugPrinter::Block("[`"));
        for (auto slot : _accumulatorDataSlotIds) {
            if (!first) {
                ret.emplace_back(DebugPrinter::Block("`,"));
            }

            DebugPrinter::addIdentifier(ret, slot);
            first = false;
        }
        ret.emplace_back(DebugPrinter::Block("`]"));
    }

    if (!_mergingExprs.empty()) {
        ret.emplace_back("spillSlots[`");
        for (size_t idx = 0; idx < _mergingExprs.size(); ++idx) {
            if (idx) {
                ret.emplace_back("`,");
            }

            DebugPrinter::addIdentifier(ret, _mergingExprs[idx].first);
        }
        ret.emplace_back("`]");

        ret.emplace_back("mergingExprs[`");
        for (size_t idx = 0; idx < _mergingExprs.size(); ++idx) {
            if (idx) {
                ret.emplace_back("`,");
            }

            DebugPrinter::addBlocks(ret, _mergingExprs[idx].second->debugPrint());
        }
        ret.emplace_back("`]");
    }

    DebugPrinter::addNewLine(ret);
    DebugPrinter::addBlocks(ret, _children[0]->debugPrint());

    return ret;
}

size_t BlockHashAggStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_children);
    size += size_estimator::estimate(_blockRowAggs);
    size += size_estimator::estimate(_outAccessorsMap);
    size += size_estimator::estimate(_mergingExprs);
    return size;
}

void BlockHashAggStage::populateBitmapSlot(size_t n) {
    _blockBitsetOutAccessor.reset(
        true,
        value::TypeTags::valueBlock,
        value::bitcastFrom<value::ValueBlock*>(
            std::make_unique<value::MonoBlock>(
                n, value::TypeTags::Boolean, value::bitcastFrom<bool>(true))
                .release()));
}

value::ValueBlock* BlockHashAggStage::makeMonoBlock(value::TypeTags tag, value::Value val) {
    // Add another element to the end of the '_monoBlocks' deque and get a reference to it
    // ('monoBlockOpt'), emplace a MonoBlock with the specified value into 'monoBlockOpt',
    // and then return a pointer to the MonoBlock.
    _monoBlocks.emplace_back();
    boost::optional<value::MonoBlock>& monoBlockOpt = _monoBlocks.back();

    monoBlockOpt.emplace(_currentBlockSize, tag, val);

    return &*monoBlockOpt;
}
}  // namespace sbe
}  // namespace mongo
