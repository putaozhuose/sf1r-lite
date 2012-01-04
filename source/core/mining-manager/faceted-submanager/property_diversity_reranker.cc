#include "property_diversity_reranker.h"

#include <search-manager/SearchManager.h>
#include <mining-manager/group-label-logger/GroupLabelLogger.h>
#include <util/ustring/UString.h>
#include <util/ClockTimer.h>

#include <list>
#include <algorithm>
#include <glog/logging.h>

NS_FACETED_BEGIN

bool CompareSecond(const std::pair<size_t, count_t>& r1, const std::pair<size_t, count_t>& r2)
{
    return (r1.second > r2.second);
}

PropertyDiversityReranker::PropertyDiversityReranker(
    const GroupManager* groupManager,
    const std::string& diversityProperty,
    const std::string& boostingProperty
)
    :groupManager_(groupManager)
    ,diversityProperty_(diversityProperty)
    ,boostingProperty_(boostingProperty)
    ,groupLabelLogger_(NULL)
{
}

PropertyDiversityReranker::~PropertyDiversityReranker()
{
}

void PropertyDiversityReranker::rerank(
    std::vector<unsigned int>& docIdList,
    std::vector<float>& rankScoreList,
    const std::string& query
)
{
    bool isBoostLabel = false;
    const PropValueTable* pvTable = NULL;
    PropValueTable::pvid_t labelId = 0;
    izenelib::util::ClockTimer timer;

    if(groupManager_ && !boostingProperty_.empty())
    {
        if (groupLabelLogger_)
        {
            std::vector<PropValueTable::pvid_t> pvIdVec;
            std::vector<int> freqVec;
            groupLabelLogger_->getFreqLabel(query, 1, pvIdVec, freqVec);

            if(!pvIdVec.empty())
            {
                labelId = pvIdVec[0];
            }
        }

        if (!labelId && !boostingPolicyProperty_.empty())
        {
            if (initLabelValueCounters_())
            {
                labelId = getBoostLabelIdByPolicy_(docIdList);
            }
        }

        if(labelId)
        {
            pvTable = groupManager_->getPropValueTable(boostingProperty_);
            if (pvTable)
            {
                isBoostLabel = true;

                const izenelib::util::UString& labelUStr = pvTable->propValueStr(labelId);
                std::string boostLabel;
                labelUStr.convertString(boostLabel, izenelib::util::UString::UTF_8);

                LOG(INFO) << "boosting property: " << boostingProperty_
                        << ", label: " << boostLabel
                        << ", doc num: " << docIdList.size();
            }
            else
            {
                LOG(ERROR) << "in PropertyDiversityReranker: group index file is not loaded for property " << boostingProperty_;
            }
        }
        else
        {
            LOG(INFO) << "no boosting label for property " << boostingProperty_;
        }
    }

    if (isBoostLabel)
    {
        std::size_t numDoc = docIdList.size();

        std::vector<unsigned int> boostingDocIdList;
        std::vector<float> boostingScoreList;
        boostingDocIdList.reserve(numDoc);;
        boostingScoreList.reserve(numDoc);

        std::vector<unsigned int> leftDocIdList;
        std::vector<float> leftScoreList;
        leftDocIdList.reserve(numDoc);;
        leftScoreList.reserve(numDoc);

        for(std::size_t i = 0; i < numDoc; ++i)
        {
            docid_t docId = docIdList[i];

            if (pvTable->testDoc(docId, labelId))
            {
                boostingDocIdList.push_back(docId);
                boostingScoreList.push_back(rankScoreList[i]);
            }
            else
            {
                leftDocIdList.push_back(docId);
                leftScoreList.push_back(rankScoreList[i]);
            }
        }
        if(!boostingDocIdList.empty())
        {
            rerankImpl_(boostingDocIdList, boostingScoreList);
        }
        if(!leftDocIdList.empty())
        {
            rerankImpl_(leftDocIdList, leftScoreList);
        }
        boostingDocIdList.resize(numDoc);
        boostingScoreList.resize(numDoc);
        std::copy_backward(leftDocIdList.begin(), leftDocIdList.end(), boostingDocIdList.end());
        std::copy_backward(leftScoreList.begin(), leftScoreList.end(), boostingScoreList.end());
        std::swap(docIdList, boostingDocIdList);
        std::swap(rankScoreList, boostingScoreList);
    }
    else
    {
        rerankImpl_(docIdList, rankScoreList);
    }

    LOG(INFO) << "PropertyDiversityReranker::rerank() costs " << timer.elapsed() << " seconds";
}

bool PropertyDiversityReranker::initLabelValueCounters_()
{
    if (!labelValueCounters_.empty())
        return true;

    if (groupManager_ && !boostingProperty_.empty())
    {
        const PropValueTable* pvTable = NULL;
        pvTable = groupManager_->getPropValueTable(boostingProperty_);
        if (pvTable)
        {
            const PropValueTable::ValueIdList& parentIdList = pvTable->parentIdList();
            for (PropValueTable::pvid_t pvid = 0; pvid < parentIdList.size(); pvid++)
            {
                // get first level labels
                if (parentIdList[pvid] == 0)
                {
                    LabelValueCounter counter(pvid);
                    labelValueCounters_.push_back(counter);
                }
            }

            return true;
        }
    }

    return false;
}

PropValueTable::pvid_t
PropertyDiversityReranker::getBoostLabelIdByPolicy_(
        std::vector<unsigned int>& docIdList)
{
    // labelId with maximum average value
    PropValueTable::pvid_t maxLabelId = 0;

    if (groupManager_ && !searchManager_.expired())
    {
        const PropValueTable* pvTable = NULL;
        pvTable = groupManager_->getPropValueTable(boostingProperty_);

        boost::shared_ptr<SearchManager> srcSearchManager = searchManager_.lock();
        shared_ptr<NumericPropertyTable>
        propertyData(srcSearchManager->createPropertyTable(boostingPolicyProperty_));

        if (pvTable && propertyData)
        {
            // count label value
            std::size_t numDoc = docIdList.size();
            std::size_t numLabel = labelValueCounters_.size();
            double value = 0;
            for(std::size_t i = 0; i < numDoc; ++i)
            {
                for (std::size_t j = 0; j < numLabel; ++j)
                {
                    if (pvTable->testDoc(docIdList[i], labelValueCounters_[j].labelId_))
                    {
                        if (propertyData->convertPropertyValue(docIdList[i], value))
                        {
                            labelValueCounters_[j].totalvalue_ += value;
                            labelValueCounters_[j].cnt_ ++;
                        }
                    }
                }
            }

            // get maximum average value
            double maxAvgValue = 0;
            for (std::size_t j = 0; j < numLabel; ++j)
            {
                LabelValueCounter& counter = labelValueCounters_[j];
                counter.compute();
                if (counter.avgValue_ > maxAvgValue)
                {
                    maxLabelId = counter.labelId_;
                    maxAvgValue = counter.avgValue_;
                }
            }
        }
    }

    return maxLabelId;
}

void PropertyDiversityReranker::rerankImpl_(
    std::vector<unsigned int>& docIdList,
    std::vector<float>& rankScoreList
)
{
    rerankDiversity_(docIdList, rankScoreList);
    rerankCTR_(docIdList, rankScoreList);
}

void PropertyDiversityReranker::rerankDiversity_(
    std::vector<unsigned int>& docIdList,
    std::vector<float>& rankScoreList
)
{
    typedef std::list<std::pair<unsigned int,float> > DocIdList;
    typedef std::map<PropValueTable::pvid_t, DocIdList> DocIdMap;

    const PropValueTable* pvTable = NULL;
    if (groupManager_ && !diversityProperty_.empty())
    {
        pvTable = groupManager_->getPropValueTable(diversityProperty_);
        if (!pvTable)
        {
            LOG(ERROR) << "in PropertyDiversityReranker: group index file is not loaded for group property " << diversityProperty_;
            return;
        }
    }
    else
    {
        return;
    }

    const PropValueTable::ValueIdTable& idTable = pvTable->valueIdTable();
    std::size_t numDoc = docIdList.size();
    std::vector<unsigned int> newDocIdList;
    std::vector<float> newScoreList;
    newDocIdList.reserve(numDoc);
    newScoreList.reserve(numDoc);

    DocIdMap docIdMap;
    DocIdList missDocs;
    for (std::size_t i = 0; i < numDoc; ++i)
    {
        docid_t docId = docIdList[i];

        if (docId < idTable.size() && idTable[docId].empty() == false)
        {
            // use 1st group value
            PropValueTable::pvid_t pvId = idTable[docId][0];
            docIdMap[pvId].push_back(std::make_pair(docId, rankScoreList[i]));
        }
        else
        {
            missDocs.push_back(std::make_pair(docId, rankScoreList[i]));
        }
    }

    LOG(INFO) << "diversity property: " << diversityProperty_
              << ", group num: " << docIdMap.size()
              << ", doc num: " << docIdList.size();

    // single property or empty
    if(docIdMap.size() <= 1)
    {
        return;
    }

    while (!docIdMap.empty())
    {
        DocIdMap::iterator mapIt = docIdMap.begin();
        while(mapIt != docIdMap.end())
        {
            if(mapIt->second.empty())
            {
                docIdMap.erase(mapIt++);
            }
            else
            {
                std::pair<unsigned int, float> element = mapIt->second.front();
                mapIt->second.pop_front();
                newDocIdList.push_back(element.first);
                newScoreList.push_back(element.second);
                ++mapIt;
            }
        }
    }

    for(DocIdList::iterator missIt = missDocs.begin(); missIt != missDocs.end(); ++missIt)
    {
        newDocIdList.push_back(missIt->first);
        newScoreList.push_back(missIt->second);
    }

    std::swap(docIdList, newDocIdList);
    std::swap(rankScoreList, newScoreList);
}

void PropertyDiversityReranker::rerankCTR_(
    std::vector<unsigned int>& docIdList,
    std::vector<float>& rankScoreList
)
{
    std::vector<std::pair<size_t, count_t> > posClickCountList;
    bool hasClickCount = false;
    if (ctrManager_)
    {
        hasClickCount = ctrManager_->getClickCountListByDocIdList(docIdList, posClickCountList);
    }

    if (!hasClickCount)
        return;

    LOG(INFO) << "CTR rerank, doc num: " << docIdList.size();

    // sort by click-count
    std::stable_sort(posClickCountList.begin(), posClickCountList.end(), CompareSecond);

    std::size_t numDoc = docIdList.size();
    std::vector<unsigned int> newDocIdList(numDoc);
    std::vector<float> newScoreList(numDoc);

    size_t i = 0;
    for (std::vector<std::pair<size_t, count_t> >::iterator it = posClickCountList.begin();
        it != posClickCountList.end(); ++it)
    {
        size_t pos = it->first;
        newDocIdList[i] = docIdList[pos];
        newScoreList[i] = rankScoreList[pos];
        ++i;
    }

    std::swap(docIdList, newDocIdList);
    std::swap(rankScoreList, newScoreList);
}

NS_FACETED_END
