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

#include "mongo/db/query/classic_runtime_planner_for_sbe/planner_interface.h"

#include "mongo/db/exec/trial_period_utils.h"
#include "mongo/db/query/bind_input_params.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/sbe_trial_runtime_executor.h"
#include "mongo/db/query/stage_builder_util.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::classic_runtime_planner_for_sbe {

namespace {
class ValidCandidatePlanner : public PlannerBase {
public:
    ValidCandidatePlanner(PlannerDataForSBE plannerData, sbe::plan_ranker::CandidatePlan candidate)
        : PlannerBase(std::move(plannerData)), _candidate(std::move(candidate)) {}

    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeExecutor(
        std::unique_ptr<CanonicalQuery> canonicalQuery) override {
        auto nss = cq()->nss();
        auto remoteCursors = cq()->getExpCtx()->explain
            ? nullptr
            : search_helpers::getSearchRemoteCursors(cq()->cqPipeline());
        auto remoteExplains = cq()->getExpCtx()->explain
            ? search_helpers::getSearchRemoteExplains(cq()->getExpCtxRaw(), cq()->cqPipeline())
            : nullptr;
        return uassertStatusOK(
            plan_executor_factory::make(opCtx(),
                                        std::move(canonicalQuery),
                                        {makeVector(std::move(_candidate)), 0 /*winnerIdx*/},
                                        collections(),
                                        plannerOptions(),
                                        std::move(nss),
                                        extractSbeYieldPolicy(),
                                        std::move(remoteCursors),
                                        std::move(remoteExplains)));
    }

private:
    sbe::plan_ranker::CandidatePlan _candidate;
};

/**
 * Recover $where expression JS function predicate from the SBE runtime environemnt, if
 * necessary, so we could successfully replan the query. The primary match expression was
 * modified during the input parameters bind-in process while we were collecting execution
 * stats above.
 */
void recoverWhereExpression(CanonicalQuery* canonicalQuery,
                            sbe::plan_ranker::CandidatePlan&& candidate) {
    if (canonicalQuery->getExpCtxRaw()->hasWhereClause) {
        input_params::recoverWhereExprPredicate(canonicalQuery->getPrimaryMatchExpression(),
                                                candidate.data.stageData);
    }
}

/**
 * Executes the "trial" portion of a single plan until it
 *   - reaches EOF,
 *   - reaches the 'maxNumResults' limit,
 *   - early exits via the TrialRunTracker, or
 *   - returns a failure Status.
 *
 * All documents returned by the plan are enqueued into the 'CandidatePlan->results' queue.
 */
sbe::plan_ranker::CandidatePlan collectExecutionStatsForCachedPlan(
    const PlannerDataForSBE& plannerData,
    std::unique_ptr<sbe::PlanStage> root,
    stage_builder::PlanStageData data,
    size_t maxTrialPeriodNumReads) {
    const auto maxNumResults{trial_period::getTrialPeriodNumToReturn(*plannerData.cq)};

    sbe::plan_ranker::CandidatePlan candidate{nullptr /*solution*/,
                                              std::move(root),
                                              sbe::plan_ranker::CandidatePlanData{std::move(data)},
                                              false /* exitedEarly*/,
                                              Status::OK(),
                                              true /*isCachedPlan*/};
    ON_BLOCK_EXIT([rootPtr = candidate.root.get()] { rootPtr->detachFromTrialRunTracker(); });

    // Callback for the tracker when it exceeds any of the tracked metrics. If the tracker exceeds
    // the number of reads before returning 'maxNumResults' number of documents, it means that the
    // cached plan isn't performing as well as it used to and we'll need to replan, so we let the
    // tracker terminate the trial. Otherwise, the cached plan is terminated when the number of the
    // results reach 'maxNumResults'.
    auto onMetricReached = [&candidate](TrialRunTracker::TrialRunMetric metric) {
        switch (metric) {
            case TrialRunTracker::kNumReads:
                return true;  // terminate the trial run
            default:
                MONGO_UNREACHABLE;
        }
    };
    candidate.data.tracker =
        std::make_unique<TrialRunTracker>(std::move(onMetricReached),
                                          size_t{0} /*kNumResults*/,
                                          maxTrialPeriodNumReads /*kNumReads*/);
    candidate.root->attachToTrialRunTracker(candidate.data.tracker.get());

    sbe::TrialRuntimeExecutor{plannerData.opCtx,
                              plannerData.collections,
                              *plannerData.cq,
                              plannerData.sbeYieldPolicy.get(),
                              AllIndicesRequiredChecker{plannerData.collections}}
        .executeCachedCandidateTrial(&candidate, maxNumResults);

    return candidate;
}

// TODO SERVER-87466 Trigger replanning by throwing an exception, instead of creating another
// planner.
std::unique_ptr<PlannerInterface> replan(PlannerDataForSBE plannerData,
                                         std::string replanReason,
                                         bool shouldCache) {
    // The plan drawn from the cache is being discarded, and should no longer be
    // registered with the yield policy.
    plannerData.sbeYieldPolicy->clearRegisteredPlans();

    // Use the query planning module to plan the whole query.
    auto solutions =
        uassertStatusOK(QueryPlanner::plan(*plannerData.cq, plannerData.plannerParams));

    // There's a single solution, there's a special planner for just this case.
    if (solutions.size() == 1) {
        LOGV2_DEBUG(8523804,
                    1,
                    "Replanning of query resulted in a single query solution",
                    "query"_attr = redact(plannerData.cq->toStringShort()),
                    "shouldCache"_attr = (shouldCache ? "yes" : "no"));
        return std::make_unique<SingleSolutionPassthroughPlanner>(
            std::move(plannerData), std::move(solutions[0]), std::move(replanReason));
    }

    // Multiple solutions. Resort to multiplanning.
    LOGV2_DEBUG(8523805,
                1,
                "Query plan after replanning and its cache status",
                "query"_attr = redact(plannerData.cq->toStringShort()),
                "shouldCache"_attr = (shouldCache ? "yes" : "no"));
    const auto cachingMode =
        shouldCache ? PlanCachingMode::AlwaysCache : PlanCachingMode::NeverCache;
    return std::make_unique<MultiPlanner>(
        std::move(plannerData), std::move(solutions), cachingMode, std::move(replanReason));
}
}  // namespace

std::unique_ptr<PlannerInterface> makePlannerForCacheEntry(
    PlannerDataForSBE plannerData, std::unique_ptr<sbe::CachedPlanHolder> cachedPlanHolder) {
    const auto& decisionReads = cachedPlanHolder->decisionWorks;
    auto sbePlan = std::move(cachedPlanHolder->cachedPlan->root);
    auto planStageData = std::move(cachedPlanHolder->cachedPlan->planStageData);
    planStageData.debugInfo = cachedPlanHolder->debugInfo;

    LOGV2_DEBUG(
        8523404, 5, "Recovering SBE plan from the cache", "decisionReads"_attr = decisionReads);

    const bool isPinnedCacheEntry = !decisionReads.has_value();
    if (isPinnedCacheEntry) {
        auto sbePlanAndData = std::make_pair(std::move(sbePlan), std::move(planStageData));
        return std::make_unique<SingleSolutionPassthroughPlanner>(std::move(plannerData),
                                                                  std::move(sbePlanAndData));
    }

    const size_t maxReadsBeforeReplan = internalQueryCacheEvictionRatio * *decisionReads;
    auto candidate = collectExecutionStatsForCachedPlan(
        plannerData, std::move(sbePlan), std::move(planStageData), maxReadsBeforeReplan);

    tassert(8523801, "'debugInfo' should be initialized", candidate.data.stageData.debugInfo);
    auto explainer = plan_explainer_factory::make(candidate.root.get(),
                                                  &candidate.data.stageData,
                                                  candidate.solution.get(),
                                                  {},    /* optimizedData */
                                                  {},    /* rejectedCandidates */
                                                  false, /* isMultiPlan */
                                                  true /* isFromPlanCache */,
                                                  true /*matchesCachedPlan*/,
                                                  candidate.data.stageData.debugInfo);
    if (!candidate.status.isOK()) {
        // On failure, fall back to replanning the whole query. We neither evict the existing cache
        // entry, nor cache the result of replanning.
        LOGV2_DEBUG(8523802,
                    1,
                    "Execution of cached plan failed, falling back to replan",
                    "query"_attr = redact(plannerData.cq->toStringShort()),
                    "planSummary"_attr = explainer->getPlanSummary(),
                    "error"_attr = candidate.status.toString());
        std::string replanReason = str::stream() << "cached plan returned: " << candidate.status;
        recoverWhereExpression(plannerData.cq, std::move(candidate));
        return replan(std::move(plannerData), std::move(replanReason), /* shouldCache */ false);
    }

    if (candidate.exitedEarly) {
        // The trial period took more than 'maxReadsBeforeReplan' physical reads. This plan may not
        // be efficient any longer, so we replan from scratch.
        auto numReads =
            candidate.data.tracker->getMetric<TrialRunTracker::TrialRunMetric::kNumReads>();
        LOGV2_DEBUG(
            8523803,
            1,
            "Evicting cache entry for a query and replanning it since the number of required reads "
            "mismatch the number of cached reads",
            "maxReadsBeforeReplan"_attr = maxReadsBeforeReplan,
            "decisionReads"_attr = decisionReads,
            "numReads"_attr = numReads,
            "query"_attr = redact(plannerData.cq->toStringShort()),
            "planSummary"_attr = explainer->getPlanSummary());

        // Deactivate the current cache entry.
        auto& sbePlanCache = sbe::getPlanCache(plannerData.opCtx);
        sbePlanCache.deactivate(
            plan_cache_key_factory::make(*plannerData.cq,
                                         plannerData.collections,
                                         canonical_query_encoder::Optimizer::kSbeStageBuilders));

        std::string replanReason = str::stream()
            << "cached plan was less efficient than expected: expected trial execution to take "
            << decisionReads << " reads but it took at least " << numReads << " reads";
        recoverWhereExpression(plannerData.cq, std::move(candidate));
        return replan(std::move(plannerData), std::move(replanReason), /* shouldCache */ true);
    }

    // If the trial run did not exit early, it means no replanning is necessary and can return this
    // candidate to the executor. All results generated during the trial are stored with the
    // candidate so that the executor will be able to reuse them.
    return std::make_unique<ValidCandidatePlanner>(std::move(plannerData), std::move(candidate));
}
}  // namespace mongo::classic_runtime_planner_for_sbe
