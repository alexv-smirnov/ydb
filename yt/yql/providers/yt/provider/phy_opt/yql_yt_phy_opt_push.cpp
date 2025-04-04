#include "yql_yt_phy_opt.h"

#include <yt/yql/providers/yt/provider/yql_yt_helpers.h>
#include <yql/essentials/providers/common/provider/yql_provider.h>

#include <yql/essentials/core/yql_opt_utils.h>

namespace NYql {

using namespace NNodes;

TMaybeNode<TExprBase> TYtPhysicalOptProposalTransformer::PushMergeLimitToInput(TExprBase node, TExprContext& ctx) const {
    if (node.Ref().HasResult() && node.Ref().GetResult().Type() != TExprNode::World) {
        return node;
    }

    auto op = node.Cast<TYtMerge>();

    auto settings = op.Settings();
    auto limitSetting = NYql::GetSetting(settings.Ref(), EYtSettingType::Limit);
    if (!limitSetting) {
        return node;
    }

    auto section = op.Input().Item(0);
    if (NYql::HasAnySetting(section.Settings().Ref(), EYtSettingType::Skip | EYtSettingType::Sample)) {
        return node;
    }
    if (NYql::HasNonEmptyKeyFilter(section)) {
        return node;
    }

    if (AnyOf(section.Paths(), [](const TYtPath& path) { return !path.Ranges().Maybe<TCoVoid>().IsValid(); })) {
        return node;
    }

    for (auto path: section.Paths()) {
        TYtPathInfo pathInfo(path);
        // Dynamic tables don't support range selectors
        if (pathInfo.Table->Meta->IsDynamic) {
            return node;
        }
    }

    TExprNode::TPtr effectiveLimit = GetLimitExpr(limitSetting, ctx);
    if (!effectiveLimit) {
        return node;
    }

    auto sectionSettings = section.Settings().Ptr();
    auto sectionLimitSetting = NYql::GetSetting(*sectionSettings, EYtSettingType::Take);
    if (sectionLimitSetting) {
        effectiveLimit = ctx.NewCallable(node.Pos(), "Min", { effectiveLimit, sectionLimitSetting->ChildPtr(1) });
        sectionSettings = NYql::RemoveSetting(*sectionSettings, EYtSettingType::Take, ctx);
    }

    sectionSettings = NYql::AddSetting(*sectionSettings, EYtSettingType::Take, effectiveLimit, ctx);

    // Keep empty "limit" setting to prevent repeated Limits optimization
    auto updatedSettings = NYql::RemoveSetting(settings.Ref(), EYtSettingType::Limit, ctx);
    updatedSettings = NYql::AddSetting(*updatedSettings, EYtSettingType::Limit, ctx.NewList(node.Pos(), {}), ctx);

    return Build<TYtMerge>(ctx, op.Pos())
        .InitFrom(op)
        .Input()
            .Add()
                .InitFrom(section)
                .Settings(sectionSettings)
            .Build()
        .Build()
        .Settings(updatedSettings)
        .Done();
}

TExprBase TYtPhysicalOptProposalTransformer::RebuildKeyFilterAfterPushDown(TExprBase filter, size_t usedKeysCount, TExprContext& ctx) const {
    auto origBoundTupleType = filter.Ref().GetTypeAnn()->Cast<TListExprType>()->GetItemType()->Cast<TTupleExprType>()->GetItems()[0]->Cast<TTupleExprType>();
    auto origBoundTupleKeyCount = origBoundTupleType->GetSize() - 1;

    auto origBoundTupleArg = ctx.NewArgument(filter.Pos(), "boundTuple");
    TExprNode::TListType newBoundTupleItems;
    for (size_t i = 0; i < usedKeysCount; i++) {
        newBoundTupleItems.push_back(
            Build<TCoNth>(ctx, filter.Pos())
                .Tuple(origBoundTupleArg)
                .Index(ctx.NewAtom(filter.Pos(), i))
            .Done()
            .Ptr()
        );
    }
    newBoundTupleItems.push_back(
        Build<TCoNth>(ctx, filter.Pos())
            .Tuple(origBoundTupleArg)
            .Index(ctx.NewAtom(filter.Pos(), origBoundTupleKeyCount))
        .Done()
        .Ptr()
    );

    auto handleBoundTuple = Build<TCoLambda>(ctx, filter.Pos())
        .Args({origBoundTupleArg})
        .Body<TExprList>()
            .Add(std::move(newBoundTupleItems))
        .Build()
        .Done();

    return Build<TCoMap>(ctx, filter.Pos())
        .Input(filter)
        .Lambda<TCoLambda>()
            .Args({"boundTuple"})
            .Body<TExprList>()
                .Add<TExprApplier>()
                    .Apply(handleBoundTuple)
                    .With<TCoNth>(0)
                        .Tuple("boundTuple")
                        .Index(ctx.NewAtom(filter.Pos(), 0))
                    .Build()
                .Build()
                .Add<TExprApplier>()
                    .Apply(handleBoundTuple)
                    .With<TCoNth>(0)
                        .Tuple("boundTuple")
                        .Index(ctx.NewAtom(filter.Pos(), 1))
                    .Build()
                .Build()
            .Build()
        .Build()
        .Done();
}

TMaybeNode<TExprBase> TYtPhysicalOptProposalTransformer::PushDownKeyExtract(TExprBase node, TExprContext& ctx) const {
    if (node.Ref().HasResult() && node.Ref().GetResult().Type() != TExprNode::World) {
        return node;
    }

    auto op = node.Cast<TYtTransientOpBase>();

    auto getInnerOpForUpdate = [] (const TYtPath& path, const TVector<TStringBuf>& usedKeyFilterColumns) -> TMaybeNode<TYtTransientOpBase> {
        auto maybeOp = path.Table().Maybe<TYtOutput>().Operation().Maybe<TYtTransientOpBase>();
        if (!maybeOp) {
            return {};
        }
        auto innerOp = maybeOp.Cast();
        if (innerOp.Ref().StartsExecution() || innerOp.Ref().HasResult()) {
            return {};
        }

        if (!innerOp.Maybe<TYtMerge>() && !innerOp.Maybe<TYtMap>()) {
            return {};
        }

        if (innerOp.Input().Size() != 1 || innerOp.Output().Size() != 1) {
            return {};
        }

        if (NYql::HasSetting(innerOp.Settings().Ref(), EYtSettingType::Limit)) {
            return {};
        }
        const auto outSorted = innerOp.Output().Item(0).Ref().GetConstraint<TSortedConstraintNode>();
        if (!outSorted) {
            return {};
        }
        for (auto path: innerOp.Input().Item(0).Paths()) {
            const auto inputSorted = path.Ref().GetConstraint<TSortedConstraintNode>();
            if (!inputSorted || !inputSorted->Includes(*outSorted)) {
                return {};
            }
        }

        auto innerSection = innerOp.Input().Item(0);
        if (NYql::HasSettingsExcept(innerSection.Settings().Ref(), EYtSettingType::SysColumns)) {
            return {};
        }

        if (auto maybeMap = innerOp.Maybe<TYtMap>()) {
            // lambda must be passthrough for columns used in key filter
            // TODO: use passthrough constraints here
            TCoLambda lambda = maybeMap.Cast().Mapper();
            TMaybe<THashSet<TStringBuf>> passthroughColumns;
            bool analyzeJustMember = true;
            if (&lambda.Args().Arg(0).Ref() != &lambda.Body().Ref()) {
                auto maybeInnerFlatMap = GetFlatMapOverInputStream(lambda);
                if (!maybeInnerFlatMap) {
                    return {};
                }

                if (!IsPassthroughFlatMap(maybeInnerFlatMap.Cast(), &passthroughColumns, analyzeJustMember)) {
                    return {};
                }
            }

            if (passthroughColumns &&
                !AllOf(usedKeyFilterColumns, [&](const TStringBuf& col) { return passthroughColumns->contains(col); }))
            {
                return {};
            }
        }

        return maybeOp;
    };

    bool hasUpdates = false;
    TVector<TExprBase> updatedSections;
    for (auto section: op.Input()) {
        bool hasPathUpdates = false;
        TVector<TYtPath> updatedPaths;
        auto settings = section.Settings().Ptr();
        const EYtSettingType kfType = NYql::HasSetting(*settings, EYtSettingType::KeyFilter2) ?
            EYtSettingType::KeyFilter2 : EYtSettingType::KeyFilter;
        const auto keyFilters = NYql::GetAllSettingValues(*settings, kfType);
        // Non empty filters and without table index
        const bool haveNonEmptyKeyFiltersWithoutIndex =
            AnyOf(keyFilters, [](const TExprNode::TPtr& f) { return f->ChildrenSize() > 0; }) &&
            AllOf(keyFilters, [&](const TExprNode::TPtr& f) { return f->ChildrenSize() < GetMinChildrenForIndexedKeyFilter(kfType); });

        bool allPathUpdated = true;
        if (haveNonEmptyKeyFiltersWithoutIndex) {

            TSyncMap syncList;
            for (auto filter: keyFilters) {
                if (!IsYtCompleteIsolatedLambda(*filter, syncList, false)) {
                    return node;
                }
            }

            // TODO: should actually be true for both kf1/kf2 - enforce in ValidateSettings()
            YQL_ENSURE(kfType == EYtSettingType::KeyFilter || keyFilters.size() == 1);
            const auto kfColumns = GetKeyFilterColumns(section, kfType);
            YQL_ENSURE(!kfColumns.empty());
            for (auto path: section.Paths()) {
                auto pathRowSpec = TYtTableBaseInfo::GetRowSpec(path.Table());

                if (auto maybeOp = getInnerOpForUpdate(path, kfColumns)) {
                    auto innerOp = maybeOp.Cast();
                    if (kfType == EYtSettingType::KeyFilter2) {
                        // check input/output keyFilter columns are of same type
                        const TStructExprType* inputType =
                            innerOp.Input().Item(0).Ref().GetTypeAnn()->Cast<TListExprType>()->GetItemType()->Cast<TStructExprType>();
                        const TStructExprType* outputType =
                            innerOp.Output().Item(0).Ref().GetTypeAnn()->Cast<TListExprType>()->GetItemType()->Cast<TStructExprType>();
                        bool sameTypes = true;
                        for (auto& keyColumn : kfColumns) {
                            auto inPos = inputType->FindItem(keyColumn);
                            auto outPos = outputType->FindItem(keyColumn);
                            YQL_ENSURE(inPos);
                            YQL_ENSURE(outPos);
                            const TTypeAnnotationNode* inColumnType = inputType->GetItems()[*inPos]->GetItemType();
                            const TTypeAnnotationNode* outColumnType = outputType->GetItems()[*outPos]->GetItemType();
                            if (!IsSameAnnotation(*inColumnType, *outColumnType)) {
                                sameTypes = false;
                                break;
                            }
                        }

                        if (!sameTypes) {
                            // TODO: improve
                            updatedPaths.push_back(path);
                            allPathUpdated = false;
                            continue;
                        }
                    }

                    auto innerOpSection = innerOp.Input().Item(0);
                    TExprNode::TPtr updatedSection;
                    if (kfType == EYtSettingType::KeyFilter2 && State_->Configuration->DropUnusedKeysFromKeyFilter.Get().GetOrElse(DEFAULT_DROP_UNUSED_KEYS_FROM_KEY_FILTER)) {
                        for (auto innerOpPath: innerOpSection.Paths()) {
                            auto innerOpPathRowSpec = TYtTableBaseInfo::GetRowSpec(innerOpPath.Table());

                            YQL_ENSURE(kfColumns.size() <= innerOpPathRowSpec->SortedBy.size());
                            for (size_t i = 0; i < kfColumns.size(); i++) {
                                YQL_ENSURE(innerOpPathRowSpec->SortedBy[i] == pathRowSpec->SortedBy[i]);
                            }
                        }

                        TExprNode::TListType rebuiltKeyFilters;
                        for (auto filter : keyFilters) {
                            YQL_ENSURE(filter->ChildrenSize() == 2);
                            auto rebuiltFilter = RebuildKeyFilterAfterPushDown(TExprBase(filter->HeadPtr()), kfColumns.size(), ctx);
                            rebuiltKeyFilters.push_back(Build<TCoNameValueTuple>(ctx, innerOpSection.Settings().Pos())
                                .Name().Build("keyFilter2")
                                .Value<TExprList>()
                                    .Add(rebuiltFilter)
                                    .Add(filter->Child(1))
                                .Build()
                                .Done()
                                .Ptr()
                            );
                        }

                        updatedSection = Build<TYtSection>(ctx, innerOpSection.Pos())
                            .InitFrom(innerOpSection)
                            .Settings(NYql::MergeSettings(innerOpSection.Settings().Ref(), *ctx.NewList(innerOpSection.Settings().Pos(), std::move(rebuiltKeyFilters)), ctx))
                            .Done()
                            .Ptr();

                    } else {
                        updatedSection = Build<TYtSection>(ctx, innerOpSection.Pos())
                            .InitFrom(innerOpSection)
                            .Settings(NYql::MergeSettings(innerOpSection.Settings().Ref(), *NYql::KeepOnlySettings(section.Settings().Ref(), EYtSettingType::KeyFilter | EYtSettingType::KeyFilter2, ctx), ctx))
                            .Done()
                            .Ptr();
                    }

                    auto updatedSectionList = Build<TYtSectionList>(ctx, innerOp.Input().Pos()).Add(updatedSection).Done();
                    auto updatedInnerOp = ctx.ChangeChild(innerOp.Ref(), TYtTransientOpBase::idx_Input, updatedSectionList.Ptr());
                    if (!syncList.empty()) {
                        updatedInnerOp = ctx.ChangeChild(*updatedInnerOp, TYtTransientOpBase::idx_World, ApplySyncListToWorld(innerOp.World().Ptr(), syncList, ctx));
                    }

                    updatedPaths.push_back(
                        Build<TYtPath>(ctx, path.Pos())
                            .InitFrom(path)
                            .Table<TYtOutput>()
                                .InitFrom(path.Table().Cast<TYtOutput>())
                                .Operation(updatedInnerOp)
                            .Build()
                            .Done());

                    hasPathUpdates = true;
                } else {
                    updatedPaths.push_back(path);
                    allPathUpdated = false;
                }
            }
        }
        if (hasPathUpdates) {
            hasUpdates = true;
            if (allPathUpdated) {
                settings = NYql::RemoveSettings(*settings, EYtSettingType::KeyFilter | EYtSettingType::KeyFilter2, ctx);
                settings = NYql::AddSetting(*settings, kfType, ctx.NewList(section.Pos(), {}), ctx);
            }
            updatedSections.push_back(
                Build<TYtSection>(ctx, section.Pos())
                    .InitFrom(section)
                    .Paths()
                        .Add(updatedPaths)
                    .Build()
                    .Settings(settings)
                    .Done());
        } else {
            updatedSections.push_back(section);
        }
    }

    if (!hasUpdates) {
        return node;
    }

    auto sectionList = Build<TYtSectionList>(ctx, op.Input().Pos())
        .Add(updatedSections)
        .Done();

    return TExprBase(ctx.ChangeChild(node.Ref(), TYtTransientOpBase::idx_Input, sectionList.Ptr()));
}

TMaybeNode<TExprBase> TYtPhysicalOptProposalTransformer::PushDownYtMapOverSortedMerge(TExprBase node, TExprContext& ctx, const TGetParents& getParents) const {
    auto map = node.Cast<TYtMap>();

    if (map.Ref().HasResult()) {
        return node;
    }

    if (map.Input().Size() > 1 || map.Output().Size() > 1) {
        return node;
    }

    if (NYql::HasAnySetting(map.Settings().Ref(), EYtSettingType::Sharded | EYtSettingType::JobCount)) {
        return node;
    }

    if (!NYql::HasSetting(map.Settings().Ref(), EYtSettingType::Ordered)) {
        return node;
    }

    auto section = map.Input().Item(0);
    if (section.Paths().Size() > 1) {
        return node;
    }
    if (NYql::HasSettingsExcept(section.Settings().Ref(), EYtSettingType::KeyFilter | EYtSettingType::KeyFilter2)) {
        return node;
    }
    if (NYql::HasNonEmptyKeyFilter(section)) {
        return node;
    }
    auto path = section.Paths().Item(0);
    if (!path.Columns().Maybe<TCoVoid>() || !path.Ranges().Maybe<TCoVoid>()) {
        return node;
    }
    auto maybeMerge = path.Table().Maybe<TYtOutput>().Operation().Maybe<TYtMerge>();
    if (!maybeMerge) {
        return node;
    }
    auto merge = maybeMerge.Cast();
    if (merge.Ref().StartsExecution() || merge.Ref().HasResult()) {
        return node;
    }
    const auto rowSpec = TYqlRowSpecInfo(merge.Output().Item(0).RowSpec());
    if (!rowSpec.IsSorted()) {
        return node;
    }
    TMaybeNode<TExprBase> columns;
    if (rowSpec.HasAuxColumns()) {
        TSet<TStringBuf> members;
        for (auto item: rowSpec.GetType()->GetItems()) {
            members.insert(item->GetName());
        }
        columns = TExprBase(ToAtomList(members, merge.Pos(), ctx));
    }

    auto mergeSection = merge.Input().Item(0);
    if (NYql::HasSettingsExcept(mergeSection.Settings().Ref(), EYtSettingType::KeyFilter | EYtSettingType::KeyFilter2)) {
        return node;
    }
    if (NYql::HasNonEmptyKeyFilter(mergeSection)) {
        return node;
    }
    if (merge.Settings().Size() > 0) {
        return node;
    }

    const TParentsMap* parentsMap = getParents();
    if (IsOutputUsedMultipleTimes(merge.Ref(), *parentsMap)) {
        // Merge output is used more than once
        return node;
    }

    auto world = map.World().Ptr();
    if (!merge.World().Ref().IsWorld()) {
        world = Build<TCoSync>(ctx, map.Pos()).Add(world).Add(merge.World()).Done().Ptr();
    }
    TVector<TYtPath> paths;
    for (auto path: mergeSection.Paths()) {
        auto newPath = Build<TYtPath>(ctx, map.Pos())
            .Table<TYtOutput>()
                .Operation<TYtMap>()
                    .InitFrom(map)
                    .World(world)
                    .Input()
                        .Add()
                            .Paths()
                                .Add<TYtPath>()
                                    .InitFrom(path)
                                    .Columns(columns.IsValid() ? columns.Cast() : path.Columns())
                                .Build()
                            .Build()
                            .Settings(section.Settings())
                        .Build()
                    .Build()
                .Build()
                .OutIndex().Value("0").Build()
            .Build()
            .Columns<TCoVoid>().Build()
            .Ranges<TCoVoid>().Build()
            .Stat<TCoVoid>().Build()
            .Done();
        paths.push_back(std::move(newPath));
    }

    return Build<TYtMerge>(ctx, node.Pos())
        .World<TCoWorld>().Build()
        .DataSink(merge.DataSink())
        .Output(map.Output()) // Rewrite output type from YtMap
        .Input()
            .Add()
                .Paths()
                    .Add(paths)
                .Build()
                .Settings(mergeSection.Settings())
            .Build()
        .Build()
        .Settings()
        .Build()
        .Done();
}

}  // namespace NYql
