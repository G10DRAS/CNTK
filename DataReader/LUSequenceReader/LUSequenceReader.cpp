//
// <copyright file="LUSequenceReader.cpp" company="Microsoft">
//     Copyright (c) Microsoft Corporation.  All rights reserved.
// </copyright>
//
// LUSequenceReader.cpp : Defines the exported functions for the DLL application.
//


#include "stdafx.h"
#define DATAREADER_EXPORTS  // creating the exports here
#include "DataReader.h"
#include "LUSequenceReader.h"
#ifdef LEAKDETECT
#include <vld.h> // leak detection
#endif
#include <fstream>
#include <random>       // std::default_random_engine
#include "fileutil.h"

namespace Microsoft { namespace MSR { namespace CNTK {

// GetIdFromLabel - get an Id from a Label
// mbStartSample - the starting sample we are ensureing are good
// endOfDataCheck - check if we are at the end of the dataset (no wraparound)
// returns - true if we have more to read, false if we hit the end of the dataset
template<class ElemType>
/* return value used to be unsigned */
long LUSequenceReader<ElemType>::GetIdFromLabel(const LabelType& labelValue, LabelInfo& labelInfo)
{
    auto found = labelInfo.word4idx.find(labelValue);

    return found->second;
}

template<class ElemType>
BatchLUSequenceReader<ElemType>::~BatchLUSequenceReader()
{
    if (m_labelTemp.size() > 0)
        m_labelTemp.clear();
    if (m_featureTemp.size() > 0)
        m_featureTemp.clear();
    for (int index = labelInfoMin; index < labelInfoMax; ++index)
    {
        delete[] m_labelInfo[index].m_id2classLocal;
        delete[] m_labelInfo[index].m_classInfoLocal;
    };
}

template<class ElemType>
void BatchLUSequenceReader<ElemType>::ReadLabelInfo(const wstring & vocfile,
    map<wstring, long> & word4idx,
    bool readClass,
    map<wstring, long>& word4cls,
    map<long, wstring>& idx4word,
    map<long, long>& idx4class,
    int & mNbrCls)
{
    char strFileName[MAX_STRING];
    wstring strtmp;
    size_t sz;
    long b;

    wcstombs_s(&sz, strFileName, 2048, vocfile.c_str(), vocfile.length());

    wifstream vin; 
    vin.open(strFileName, wifstream::in);
    if (!vin.good())
        LogicError("LUSequenceReader cannot open %ls \n", strFileName);

    wstring wstr = L" ";
    b = 0;
    this->nwords = 0;
    int prevcls = -1;

    mNbrCls = 0;
    while (vin.good())
    {
        getline(vin, strtmp); 
        strtmp = wtrim(strtmp);
        if (strtmp.length() == 0)
            break; 
        if (readClass)
        {
            vector<wstring> wordandcls = wsep_string(strtmp, wstr);
#ifdef __unix__
            long cls = (long)wcstol(wordandcls[1].c_str(),nullptr,10);
#else
            long cls = (long)_wtoi(wordandcls[1].c_str());
#endif
            word4cls[wordandcls[0]] = cls;

            idx4class[b] = cls;

            if (idx4class[b] != prevcls)
            {
                if (idx4class[b] < prevcls)
                    LogicError("LUSequenceReader: the word list needs to be grouped into classes and the classes indices need to be ascending.");
                prevcls = idx4class[b];
            }

            word4idx[wordandcls[0]] = b;
            idx4word[b++] = wordandcls[0];
            if (mNbrCls < cls)
                mNbrCls = cls;
        }
        else {
            word4idx[strtmp] = b;
            idx4word[b++] = strtmp;
        }
        this->nwords++;
    }
    vin.close();

    if (readClass)
        mNbrCls++;
}

template<class ElemType>
void BatchLUSequenceReader<ElemType>::GetClassInfo(LabelInfo& lblInfo)
{
    if (lblInfo.m_clsinfoRead || lblInfo.mNbrClasses == 0) return;

    // populate local CPU matrix
    if (lblInfo.m_id2classLocal == nullptr)
        lblInfo.m_id2classLocal = new Matrix<ElemType>(CPUDEVICE);
    if (lblInfo.m_classInfoLocal == nullptr)
        lblInfo.m_classInfoLocal = new Matrix<ElemType>(CPUDEVICE);

    lblInfo.m_classInfoLocal->SwitchToMatrixType(MatrixType::DENSE, matrixFormatDense, false);
    lblInfo.m_classInfoLocal->Resize(2, lblInfo.mNbrClasses);

    //move to CPU since element-wise operation is expensive and can go wrong in GPU
    int curDevId = lblInfo.m_classInfoLocal->GetDeviceId();
    lblInfo.m_classInfoLocal->TransferFromDeviceToDevice(curDevId, CPUDEVICE, true, false, false);

    int clsidx;
    int prvcls = -1;
    for (size_t j = 0; j < this->nwords; j++)
    {
        clsidx = lblInfo.idx4class[(long)j];
        if (prvcls != clsidx)
        {
            if (prvcls >= 0)
                (*lblInfo.m_classInfoLocal)(1, prvcls) = (float)j;
            prvcls = clsidx;
            (*lblInfo.m_classInfoLocal)(0, prvcls) = (float)j;
        }
    }
    (*lblInfo.m_classInfoLocal)(1, prvcls) = (float)this->nwords;

    lblInfo.m_classInfoLocal->TransferFromDeviceToDevice(CPUDEVICE, curDevId, true, false, false);

    lblInfo.m_clsinfoRead = true;
}

// GetIdFromLabel - get an Id from a Label
// mbStartSample - the starting sample we are ensureing are good
// endOfDataCheck - check if we are at the end of the dataset (no wraparound)
// returns - true if we have more to read, false if we hit the end of the dataset
template<class ElemType>
bool LUSequenceReader<ElemType>::GetIdFromLabel(const vector<LabelIdType>& labelValue, vector<LabelIdType>& val)
{
    val.clear();

    for (size_t i = 0; i < labelValue.size(); i++)
    {
        val.push_back(labelValue[i]);
    }
    return true;
}

template<class ElemType>
int LUSequenceReader<ElemType>::GetSentenceEndIdFromOutputLabel()
{

    // now get the labels
    LabelInfo& featIn = m_labelInfo[labelInfoOut];

    auto found = featIn.word4idx.find(featIn.endSequence);

    // not yet found, add to the map
    if (found != featIn.word4idx.end())
    {
        return (int)found->second;
    }
    else return -1;
}

// GetData - Gets metadata from the specified section (into CPU memory) 
// sectionName - section name to retrieve data from
// numRecords - number of records to read
// data - pointer to data buffer, if NULL, dataBufferSize will be set to size of required buffer to accomidate request
// dataBufferSize - [in] size of the databuffer in bytes
//                  [out] size of buffer filled with data
// recordStart - record to start reading from, defaults to zero (start of data)
// returns: true if data remains to be read, false if the end of data was reached
template<class ElemType>
bool LUSequenceReader<ElemType>::GetData(const std::wstring& , size_t , void* , size_t& , size_t )
{
    return false;
}

//bool LUSequenceReader<ElemType>::CheckIdFromLabel(const typename LUSequenceParser<ElemType>::LabelType& labelValue, const LabelInfo& labelInfo, typename LUSequenceParser<ElemType>::LabelIdType & labelId)
template<class ElemType>
bool LUSequenceReader<ElemType>::CheckIdFromLabel(const LabelType& labelValue, const LabelInfo& labelInfo, unsigned & labelId)
{
    auto found = labelInfo.mapLabelToId.find(labelValue);

    // not yet found, add to the map
    if (found == labelInfo.mapLabelToId.end())
    {
        return false; 
    }
    labelId = found->second;
    return true; 
}

template<class ElemType>
void LUSequenceReader<ElemType>::WriteLabelFile()
{
    // update the label dimension if it is not big enough, need it here because m_labelIdMax get's updated in the processing loop (after a read)
    for (int index = labelInfoMin; index < labelInfoMax; ++index)
    {
        LabelInfo& labelInfo = m_labelInfo[index];

        // write out the label file if they don't have one
        if (!labelInfo.fileToWrite.empty())
        {
            if (labelInfo.mapIdToLabel.size() > 0)
            {
                File labelFile(labelInfo.fileToWrite, fileOptionsWrite | fileOptionsText);
                for (int i=0; i < labelInfo.mapIdToLabel.size(); ++i)
                {
                    labelFile << labelInfo.mapIdToLabel[i] << '\n';
                }
                labelInfo.fileToWrite.clear();
            }
            else if (!m_cachingWriter)
            {
                fprintf(stderr, "WARNING: file %ws NOT written to disk, label files only written when starting at epoch zero!", labelInfo.fileToWrite.c_str());
            }
        }
    }
}

template<class ElemType>
void LUSequenceReader<ElemType>::LoadLabelFile(const std::wstring &filePath, std::vector<LabelType>& retLabels)
{
    // initialize with file name
    std::wstring path = filePath;
    
    wchar_t stmp[MAX_STRING];
    wstring str; 
    retLabels.resize(0);
    wifstream vin;
#ifdef __unix__
    vin.open(ws2s(path).c_str(), ifstream::in);
#else
    vin.open(path.c_str(), ifstream::in);
#endif

    while (vin.good())
    {
        vin.getline(stmp, MAX_STRING);
        wstring temp = stmp;
        str = wtrim(temp);
        if (str.length() == 0)
            break; 

        // check for a comment line
        wstring::size_type pos = str.find_first_not_of(L" \t");
        if (pos != -1)
        {
            str = wtrim(str);
            retLabels.push_back((LabelType)str);
        }
    }
    vin.close();  
}

template<class ElemType>
void LUSequenceReader<ElemType>::ChangeMaping(const map<LabelType, LabelType>& maplist,
    const LabelType & unkstr,
    map<LabelType, LabelIdType> & word4idx)
{
    auto punk = word4idx.find(unkstr);
    for(auto ptr = word4idx.begin(); ptr != word4idx.end(); ptr++)
    {
        LabelType wrd = ptr->first;
        LabelIdType idx = -1; 
        if (maplist.find(wrd) != maplist.end())
        {
            LabelType mpp = maplist.find(wrd)->second; 
            idx = word4idx[mpp];
        }
        else
        {
            if (punk == word4idx.end())
            {
                RuntimeError("check unk list is missing ");
            }
            idx = punk->second;
        }

        word4idx[wrd] = idx;
    }
}


template<class ElemType>
void BatchLUSequenceReader<ElemType>::Init(const ConfigParameters& readerConfig)
{
    // See if the user wants caching
    m_cachingReader = NULL;
    m_cachingWriter = NULL;

    LoadWordMapping(readerConfig);

    std::vector<std::wstring> features;
    std::vector<std::wstring> labels;
    GetFileConfigNames(readerConfig, features, labels);
    if (features.size() > 0)
    {
        m_featuresName = features[0];
    }

    {
        wstring tInputLabel = readerConfig("inputLabel", L"");
        wstring tOutputLabel = readerConfig("outputLabel", L"");

        if (labels.size() == 2)
        {
            if (tInputLabel == L"" && tOutputLabel == L"")
            {
                for (int index = labelInfoMin; index < labelInfoMax; ++index)
                {
                    m_labelsName[index] = labels[index];
                }
            }
            else
            {
                int index = 0;
                for (int i = labelInfoMin; i < labelInfoMax; ++i)
                {
                    if (labels[i] == tInputLabel)
                        m_labelsName[index] = labels[i];
                }
                if (m_labelsName[index] == L"")
                    RuntimeError("cannot find input label");

                index = 1;
                for (int i = labelInfoMin; i < labelInfoMax; ++i)
                {
                    if (labels[i] == tOutputLabel)
                        m_labelsName[index] = labels[i];
                }
                if (m_labelsName[index] == L"")
                    RuntimeError("cannot find output label");
            }
        }
        else
            RuntimeError("two label definitions (in and out) required for Sequence Reader");

        ConfigParameters featureConfig = readerConfig(m_featuresName, "");
        ConfigParameters labelConfig[2] = { readerConfig(m_labelsName[0], ""), readerConfig(m_labelsName[1], "") };

        for (int index = labelInfoMin; index < labelInfoMax; ++index)
        {
            m_labelInfo[index].idMax = 0;
            m_labelInfo[index].beginSequence = (wstring) labelConfig[index]("beginSequence", "");
            m_labelInfo[index].endSequence = (wstring) labelConfig[index]("endSequence", "");
            m_labelInfo[index].busewordmap = labelConfig[index]("usewordmap", "false");

            m_labelInfo[index].isproposal = labelConfig[index]("isproposal", "false");

            m_labelInfo[index].m_clsinfoRead = false;

            // determine label type desired
            std::string labelType(labelConfig[index]("labelType", "Category"));
            if (labelType == "Category")
            {
                m_labelInfo[index].type = labelCategory;
            }
            else
                LogicError("LUSequence reader only supports category label");

            // if we have labels, we need a label Mapping file, it will be a file with one label per line
            if (m_labelInfo[index].type != labelNone)
            {
                string mode = labelConfig[index]("mode", "plain");//plain, class

                m_labelInfo[index].m_classInfoLocal = nullptr;
                m_labelInfo[index].m_id2classLocal = nullptr;

                if (mode == "class")
                {
                    m_labelInfo[index].readerMode = ReaderMode::Class;
                }

                std::wstring wClassFile = labelConfig[index]("token", "");
                if (wClassFile != L""){
                    ReadLabelInfo(wClassFile, m_labelInfo[index].word4idx, 
                        m_labelInfo[index].readerMode == ReaderMode::Class, 
                        m_labelInfo[index].word4cls,
                        m_labelInfo[index].idx4word, m_labelInfo[index].idx4class, m_labelInfo[index].mNbrClasses);

                    GetClassInfo(m_labelInfo[index]);
                }
                if (m_labelInfo[index].busewordmap)
                    ChangeMaping(mWordMapping, mUnkStr, m_labelInfo[index].word4idx);
                m_labelInfo[index].dim = (long)m_labelInfo[index].idx4word.size();
            }

        }
    }

    // initialize all the variables
    m_mbStartSample = m_epoch = m_totalSamples = m_epochStartSample = m_seqIndex = 0;
    m_endReached = false;
    m_readNextSampleLine = 0;
    m_readNextSample = 0;

    ConfigArray wContext = readerConfig("wordContext", "0");
    intargvector wordContext = wContext;
    m_wordContext = wordContext;

    // The input data is a combination of the label Data and extra feature dims together
//    m_featureCount = m_featureDim + m_labelInfo[labelInfoIn].dim;
    m_featureCount = 1; 

    std::wstring m_file = readerConfig("file");
    if (m_traceLevel > 0)
        fprintf(stderr, "reading sequence file %ws\n", m_file.c_str());

    const LabelInfo& labelIn = m_labelInfo[labelInfoIn];
    const LabelInfo& labelOut = m_labelInfo[labelInfoOut];
    m_parser.ParseInit(m_file.c_str(), labelIn.dim, labelOut.dim, labelIn.beginSequence, labelIn.endSequence, labelOut.beginSequence, labelOut.endSequence, mUnkStr);

    mBlgSize = readerConfig("nbruttsineachrecurrentiter", "1");

    mRandomize = false;
    if (readerConfig.Exists("randomize"))
    {
        string randomizeString = readerConfig("randomize");
        if (randomizeString == "None")
        {
            ;
        }
        else if (randomizeString == "Auto" || randomizeString == "True")
        {
            mRandomize = true;
        }
    }

    mEqualLengthOutput = readerConfig("equalLength", "true");
    mAllowMultPassData = readerConfig("dataMultiPass", "false");

    mIgnoreSentenceBeginTag = readerConfig("ignoresentencebegintag", "false");

}

template<class ElemType>
void BatchLUSequenceReader<ElemType>::Reset()
{
    mProcessed.clear();
    mToProcess.clear();
    mLastProcssedSentenceId = 0;
    mPosInSentence = 0;
    mLastPosInSentence = 0;
    mNumRead = 0;

    if (m_labelTemp.size() > 0)
        m_labelTemp.clear();
    if (m_featureTemp.size() > 0)
        m_featureTemp.clear();
    m_parser.mSentenceIndex2SentenceInfo.clear();
}

template<class ElemType>
void BatchLUSequenceReader<ElemType>::StartMinibatchLoop(size_t mbSize, size_t epoch, size_t requestedEpochSamples)
{
    if (m_featuresBuffer==NULL)
    {
        const LabelInfo& labelInfo = m_labelInfo[( m_labelInfo[labelInfoOut].type == labelNextWord)?labelInfoIn:labelInfoOut];
        m_featuresBuffer = new ElemType[mbSize*labelInfo.dim];
        memset(m_featuresBuffer,0,sizeof(ElemType)*mbSize*labelInfo.dim);
    }

    if (m_labelsBuffer==NULL)
    {
        const LabelInfo& labelInfo = m_labelInfo[( m_labelInfo[labelInfoOut].type == labelNextWord)?labelInfoIn:labelInfoOut];
        if (labelInfo.type == labelCategory)
        {
            m_labelsBuffer = new ElemType[labelInfo.dim*mbSize];
            memset(m_labelsBuffer,0,sizeof(ElemType)*labelInfo.dim*mbSize);
            m_labelsIdBuffer = new long[mbSize];
            memset(m_labelsIdBuffer,0,sizeof(long)*mbSize);
        }
        else if (labelInfo.type != labelNone)
        {
            m_labelsBuffer = new ElemType[mbSize];
            memset(m_labelsBuffer,0,sizeof(ElemType)*mbSize);
            m_labelsIdBuffer = NULL;
        }
    }

    m_mbSize = mbSize;
    m_epochSize = requestedEpochSamples;

    // we use epochSize, which might not be set yet, so use a default value for allocations if not yet set
    m_epoch = epoch;
    m_mbStartSample = epoch*m_epochSize;

    // allocate room for the data
    m_featureData.reserve(m_featureCount*m_mbSize);
    if (m_labelInfo[labelInfoOut].type == labelCategory)
        m_labelIdData.reserve(m_mbSize);
    else if (m_labelInfo[labelInfoOut].type != labelNone)
        m_labelData.reserve(m_mbSize);
    m_sequence.reserve(m_seqIndex); // clear out the sequence array

    m_clsinfoRead = false; 
    m_idx2clsRead = false; 

    mTotalSentenceSofar = 0;
    m_totalSamples = 0;

    Reset();

    m_parser.ParseReset(); /// restart from the corpus begining

}

template<class ElemType>
size_t BatchLUSequenceReader<ElemType>::FindNextSentences(size_t numRead)
{
    vector<size_t> sln ;

    if (mToProcess.size() > 0 && mProcessed.size() > 0)
    {
        bool allDone = true; 
        for (int s = 0; s < mToProcess.size(); s++)
        {
            size_t mp = mToProcess[s];
            if (mProcessed[mp] == false)
            {
                allDone = false;
                break;
            }
        }
        if (allDone)
        {
            mLastPosInSentence = 0;
            mToProcess.clear();
            /// reset sentence Begin and setnenceEnd
            mSentenceEnd = false;
            mSentenceBegin = false;
        }
    }

    if (mToProcess.size() > 0 && mProcessed.size() > 0)
    {
        size_t nbrToProcess = mToProcess.size();
        mSentenceBeginAt.resize(nbrToProcess, -1);
        mSentenceEndAt.resize(nbrToProcess, -1);
        mSentenceLength.clear();
        mMaxSentenceLength = 0;

        for (size_t i = 0; i < nbrToProcess; i++)
        {
            size_t seq = mToProcess[i];
            size_t ln = m_parser.mSentenceIndex2SentenceInfo[seq].sLen;
            mSentenceLength.push_back(ln);
            mMaxSentenceLength = max(mMaxSentenceLength, ln); 
        }
        return mToProcess.size();
    }

    mMaxSentenceLength = 0;

    if (m_parser.mSentenceIndex2SentenceInfo.size() == 0)
        return mMaxSentenceLength;

    size_t iNumber = min(numRead, mProcessed.size());
    int previousLn = -1;
    for (size_t seq = mLastProcssedSentenceId, inbrReader = 0; inbrReader < iNumber; seq++)
    {
        if (seq >= mProcessed.size())
            break;

        if (mProcessed[seq]) continue;

        if (mEqualLengthOutput)
        {
            if (mProcessed[seq] == false && mToProcess.size() < mBlgSize)
            {
                int ln = (int)m_parser.mSentenceIndex2SentenceInfo[seq].sLen;
                if (ln == previousLn || previousLn == -1)
                {
                    sln.push_back(ln);
                    mToProcess.push_back(seq);
                    mMaxSentenceLength = max((int)mMaxSentenceLength, ln);
                    if (previousLn == -1)
                        mLastProcssedSentenceId = seq + 1;  /// update index for the next retrieval
                    previousLn = ln;
                }
            }

            if (mToProcess.size() == mBlgSize) break;
            inbrReader++;
        }
        else
        {
            if (mProcessed[seq] == false && mToProcess.size() < mBlgSize)
            {
                size_t ln = m_parser.mSentenceIndex2SentenceInfo[seq].sLen;
                sln.push_back(ln);
                mToProcess.push_back(seq);
                mMaxSentenceLength = max(mMaxSentenceLength, ln);
            }

            if (mToProcess.size() == mBlgSize) break;
            inbrReader++;
        }
    }

    size_t nbrToProcess = mToProcess.size();
    mSentenceBeginAt.resize(nbrToProcess, -1);
    mSentenceEndAt.resize(nbrToProcess, -1);

    mSentenceLength = sln;

    return mToProcess.size();
}

template<class ElemType>
bool BatchLUSequenceReader<ElemType>::EnsureDataAvailable(size_t /*mbStartSample*/)
{
    bool bDataIsThere = true; 

    m_featureData.clear();
    m_labelIdData.clear();
    m_featureWordContext.clear();

    // now get the labels
    LabelInfo& featIn = m_labelInfo[labelInfoIn];
    LabelInfo& labelIn = m_labelInfo[labelInfoOut];

    // see how many we already read
    std::vector<SequencePosition> seqPos;

    if (mTotalSentenceSofar > m_epochSize)
        return false;
    else
    {
        size_t nbrSentenceRead = FindNextSentences(mBlgSize);
        if (mAllowMultPassData && nbrSentenceRead == 0 && mTotalSentenceSofar > 0 && m_totalSamples < m_epochSize)
        {
            /// restart for the next pass of the data
            mProcessed.assign(mProcessed.size(), false);
            mLastProcssedSentenceId = 0;
            nbrSentenceRead = FindNextSentences(mBlgSize);
        }

        if (nbrSentenceRead == 0)
        {
            Reset();

            mNumRead = m_parser.Parse(CACHE_BLOG_SIZE, &m_labelTemp, &m_featureTemp, &seqPos, featIn.word4idx, labelIn.word4idx, mAllowMultPassData);
            if (mNumRead == 0)
            {
                fprintf(stderr, "EnsureDataAvailable: no more data\n");
                return false;
            }
            mProcessed.assign(mNumRead, false);

#ifndef DEBUG_READER
            if (mRandomize)
            {
                unsigned seed = this->m_seed; 
                std::shuffle(m_parser.mSentenceIndex2SentenceInfo.begin(), m_parser.mSentenceIndex2SentenceInfo.end(), std::default_random_engine(seed));
                this->m_seed++;
            }
#endif

            m_readNextSampleLine += mNumRead;
            nbrSentenceRead = FindNextSentences(mBlgSize);
            if (nbrSentenceRead == 0)
                return false; /// no more data to process
        }

        mTotalSentenceSofar += (ULONG) nbrSentenceRead;

        /// add one minibatch 
        int i = (int)mLastPosInSentence; 
        int j = 0;

        if (mLastPosInSentence != 0)
            throw std::runtime_error("LUSequenceReader : only support begining sentence at zero");
        if (mSentenceBeginAt.size() != mToProcess.size())
            throw std::runtime_error("LUSequenceReader : need to preallocate mSentenceBegin");
        if (mSentenceEndAt.size() != mToProcess.size())
            throw std::runtime_error("LUSequenceReader : need to preallocate mSentenceEnd");
        if (mMaxSentenceLength > m_mbSize)
            throw std::runtime_error("LUSequenceReader : minibatch size needs to be large enough to accomodate the longest sentence");

        /// reset sentenceending index to NO_LABELS, which is negative
        mSentenceEndAt.assign(mSentenceEndAt.size(), NO_LABELS);

        /**
        mtSentenceBegin : a matrix with [Ns x T]
        the first row is 0/1 bit for wether corresponding frame has sentence beginining/no_label for any of streams
        0 : no such case
        1 : case exists
        */
        mtSentenceBegin.Resize(mToProcess.size(), mMaxSentenceLength);
        mtSentenceBegin.SetValue((ElemType) SENTENCE_MIDDLE);
        DEVICEID_TYPE sentenceSegDeviceId = mtSentenceBegin.GetDeviceId();
        mtSentenceBegin.TransferFromDeviceToDevice(sentenceSegDeviceId, CPUDEVICE, true, false, false);

        m_minibatchPackingFlag.resize(mMaxSentenceLength);
        std::fill(m_minibatchPackingFlag.begin(), m_minibatchPackingFlag.end(), MinibatchPackingFlag::None);

        for (i = (int)mLastPosInSentence; j < (int)mMaxSentenceLength; i++, j++)
        {
            for (int k = 0; k < mToProcess.size(); k++)
            {
                size_t seq = mToProcess[k];

                if (
                    i == mLastPosInSentence         /// the first time instance has sentence begining
                    )
                {
                    mSentenceBeginAt[k] = i;
                    if (mIgnoreSentenceBeginTag == false)  /// ignore sentence begin, this is used for decoder network reader, which carries activities from the encoder networks
                    {
                        mtSentenceBegin.SetValue(k, j, (ElemType)SENTENCE_BEGIN);
                        m_minibatchPackingFlag[j] |= MinibatchPackingFlag::UtteranceStart;
                    }
                }

                if (i == m_parser.mSentenceIndex2SentenceInfo[seq].sLen - 1)
                {
                    mSentenceEndAt[k] = i;
                }
                if (i < m_parser.mSentenceIndex2SentenceInfo[seq].sLen)
                {
                    size_t label = m_parser.mSentenceIndex2SentenceInfo[seq].sBegin + i;
                    std::vector<std::vector<LabelIdType>> tmpCxt;

                    for (int i_cxt = 0; i_cxt < m_wordContext.size(); i_cxt++)
                    {
                        if (featIn.type == labelCategory)
                        {
                            vector<LabelIdType> index;
                            int ilabel = (int) label + m_wordContext[i_cxt];
                            if (ilabel < m_parser.mSentenceIndex2SentenceInfo[seq].sBegin)
                            {
                                GetIdFromLabel(m_featureTemp[m_parser.mSentenceIndex2SentenceInfo[seq].sBegin], index);
                            }
                            else if (ilabel >= m_parser.mSentenceIndex2SentenceInfo[seq].sEnd)
                            {
                                GetIdFromLabel(m_featureTemp[m_parser.mSentenceIndex2SentenceInfo[seq].sEnd - 1], index);
                            }
                            else
                            {
                                GetIdFromLabel(m_featureTemp[ilabel], index);
                            }
                            if (i_cxt == 0)
                            {
                                m_featureData.push_back(index);
                            }
                            tmpCxt.push_back(index);
                        }
                        else
                        {
                            RuntimeError("Input label expected to be a category label");
                        }
                    }

                    m_featureWordContext.push_back(tmpCxt);

                    // now get the output label
                    LabelIdType id = m_labelTemp[label];
                    m_labelIdData.push_back(id);

                    m_totalSamples++;
                }
                else
                {
                    /// push null 
                    std::vector<std::vector<LabelIdType>> tmpCxt;
                    std::vector<LabelIdType> index;
                    for (int i_cxt = 0; i_cxt < m_wordContext.size(); i_cxt++)
                        index.push_back((LabelIdType)NULLLABEL);
                    tmpCxt.push_back(index);
                    m_featureWordContext.push_back(tmpCxt);

                    m_labelIdData.push_back((LabelIdType)NULLLABEL);
                    mtSentenceBegin.SetValue(k, j, (ElemType) NO_LABELS);
                    m_minibatchPackingFlag[j] |= MinibatchPackingFlag::NoLabel;
                }

            }
        }

        mLastPosInSentence = (i == mMaxSentenceLength)?0:i;

        mtSentenceBegin.TransferFromDeviceToDevice(CPUDEVICE, sentenceSegDeviceId, true, false, false);
    }

    return bDataIsThere;
}

template<class ElemType>
size_t BatchLUSequenceReader<ElemType>::NumberSlicesInEachRecurrentIter()
{
    size_t sz = (mSentenceBeginAt.size() == 0)?mBlgSize : mSentenceBeginAt.size();
    if (mSentenceBeginAt.size() == 0)
    {
        mSentenceBeginAt.assign(sz, -1);
    }
    if (mSentenceEndAt.size() == 0)
    {
        mSentenceEndAt.assign(sz, -1);
    }
    return sz;
}

template<class ElemType>
void BatchLUSequenceReader<ElemType>::SetNbrSlicesEachRecurrentIter(const size_t mz)
{
    mBlgSize = mz;
}

template<class ElemType>
bool BatchLUSequenceReader<ElemType>::GetMinibatch(std::map<std::wstring, Matrix<ElemType>*>& matrices)
{

    // get out if they didn't call StartMinibatchLoop() first
    if (m_mbSize == 0)
    {
        fprintf(stderr, "GetMiniBatch : m_mbSize = 0\n");
        return false;
    }

    bool moreData = EnsureDataAvailable(m_mbStartSample);
    if (moreData == false)
        return false;

    // actual size is the size of the next seqence
    size_t actualmbsize = 0;
    size_t lablsize = 0;

    // figure out the size of the next sequence
    actualmbsize = m_labelIdData.size();
    if (actualmbsize > m_mbSize * mToProcess.size()){
        RuntimeError("specified minibatch size %d is smaller than the actual minibatch size %d. memory can crash!", m_mbSize, actualmbsize);
    }

    // now get the labels
    const LabelInfo& featInfo = m_labelInfo[labelInfoIn];

    if (actualmbsize > 0)
    {

        //loop through all the samples
        Matrix<ElemType>& features = *matrices[m_featuresName];
        Matrix<ElemType>  locObs(CPUDEVICE);
        if (features.GetMatrixType() == DENSE)
            locObs.SwitchToMatrixType(DENSE, features.GetFormat(), false);
        else
            locObs.SwitchToMatrixType(SPARSE, matrixFormatSparseCSC, false);

        if (matrices.find(m_featuresName) == matrices.end())
        {
            RuntimeError("LUsequence reader cannot find %s", m_featuresName.c_str());
        }

        locObs.Resize(featInfo.dim * m_wordContext.size(), actualmbsize);

        size_t utt_id = 0;
        for (size_t j = 0; j < actualmbsize; ++j)
        {
            utt_id = (size_t) fmod(j, mSentenceEndAt.size());  /// get the utterance id

            size_t utt_t = (size_t) floor(j/mSentenceEndAt.size()); /// the utt-specific timing

            // vector of feature data goes into matrix column
            for (size_t jj = 0; jj < m_featureWordContext[j].size(); jj++) ///  number of sentence per time
            {
                /// this support context dependent inputs since words or vector of words are placed
                /// in different slots
                for (size_t ii = 0; ii < m_featureWordContext[j][jj].size(); ii++)  /// context
                {
                    /// this can support bag of words, since words are placed in the same slot
                    size_t idx = m_featureWordContext[j][jj][ii];

                    if (idx >= featInfo.dim)
                    {
                        if (mtSentenceBegin(utt_id, utt_t) != NO_LABELS) /// for those obs that are for no observations
                        {
                            LogicError("BatchLUSequenceReader::GetMinibatch observation is larger than its dimension but no_labels sign is not used to indicate that this observation has no labels. Possible reason is a bug in EnsureDataAvailable or a bug here. ");
                        }
                        continue;
                    }

                    assert(idx < featInfo.dim);
                    if (utt_t > mSentenceEndAt[utt_id]) 
                        locObs.SetValue(idx + jj * featInfo.dim, j, (ElemType)0);
                    else
                        locObs.SetValue(idx + jj * featInfo.dim, j, (ElemType)1);
                }
            }
        }

        features.SetValue(locObs);
        
        lablsize = GetLabelOutput(matrices, m_labelInfo[labelInfoOut], actualmbsize);

        // go to the next sequence
        m_seqIndex++;
    }
    else
    {
        fprintf(stderr, "actual minibatch size is zero\n");
        return 0;
    }

    // we read some records, so process them
    if (actualmbsize == 0)
        return false;
    else
        return true;
}

template<class ElemType>
size_t BatchLUSequenceReader<ElemType>::GetLabelOutput(std::map<std::wstring, 
    Matrix<ElemType>*>& matrices, LabelInfo& labelInfo, size_t actualmbsize)
{
    Matrix<ElemType>* labels = matrices[m_labelsName[labelInfoOut]];
    if (labels == nullptr) return 0;

    DEVICEID_TYPE device = labels->GetDeviceId();

    labels->Resize(labelInfo.dim, actualmbsize);
    labels->SetValue(0);
    labels->TransferFromDeviceToDevice(device, CPUDEVICE, true);

    size_t nbrLabl = 0;
    for (size_t j = 0; j < actualmbsize; ++j)
    {
        long wrd = m_labelIdData[j];

        size_t utt_id = (size_t) fmod(j, mSentenceBeginAt.size());
        size_t utt_t = (size_t) floor(j / mSentenceBeginAt.size());

        if (utt_t > mSentenceEndAt[utt_id]) continue;
        if (labelInfo.readerMode == ReaderMode::Plain)
            labels->SetValue(wrd, j, 1); 
        else if (labelInfo.readerMode == ReaderMode::Class && labelInfo.mNbrClasses > 0)
        {
            labels->SetValue(0, j, (ElemType)wrd);

            long clsidx = -1;
            clsidx = labelInfo.idx4class[wrd];

            labels->SetValue(1, j, (ElemType)clsidx);
            /// save the [begining ending_indx) of the class 
            ElemType lft = (*labelInfo.m_classInfoLocal)(0, clsidx);
            ElemType rgt = (*labelInfo.m_classInfoLocal)(1, clsidx);
            if (rgt <= lft)
                LogicError("LUSequenceReader : right is equal or smaller than the left, which is wrong.");
            labels->SetValue(2, j, lft); /// begining index of the class
            labels->SetValue(3, j, rgt); /// end index of the class
        }
        else
            LogicError("LUSequenceReader: reader mode is not set to Plain. Or in the case of setting it to Class, the class number is 0. ");
        nbrLabl++;
    }

    return nbrLabl;
}

template<class ElemType>
void BatchLUSequenceReader<ElemType>::SetSentenceSegBatch(Matrix<ElemType>& sentenceBegin, vector<MinibatchPackingFlag>& minibatchPackingFlag)
{
    DEVICEID_TYPE device = mtSentenceBegin.GetDeviceId();
    mtSentenceBegin.TransferFromDeviceToDevice(device, sentenceBegin.GetDeviceId(), true);
    sentenceBegin.SetValue(mtSentenceBegin); 
    mtSentenceBegin.TransferFromDeviceToDevice(sentenceBegin.GetDeviceId(), device, true);

    minibatchPackingFlag = m_minibatchPackingFlag;
}

template<class ElemType>
void BatchLUSequenceReader<ElemType>::SetSentenceEnd(int wrd, int pos, int actualMbSize)
{
    // now get the labels
    LabelInfo& labelIn = m_labelInfo[labelInfoIn];
    LabelIdType index = GetIdFromLabel(labelIn.endSequence.c_str(), labelIn);

    if (pos == actualMbSize - 1) 
    {
        if (wrd == (int)index)
            mSentenceEnd = true;
        else
            mSentenceEnd = false; 
    }
}

template<class ElemType>
void BatchLUSequenceReader<ElemType>::SetSentenceBegin(int wrd, int pos, int /*actualMbSize*/)
{
    // now get the labels
    LabelInfo& labelIn = m_labelInfo[labelInfoIn];
    LabelIdType index = GetIdFromLabel(labelIn.beginSequence.c_str(), labelIn);

    if (pos == 0) 
    {
        if (wrd == (int)index)
            mSentenceBegin = true;
        else
            mSentenceBegin = false; 
    }
}


template<class ElemType>
bool BatchLUSequenceReader<ElemType>::DataEnd(EndDataType endDataType)
{
    bool ret = false;
    switch (endDataType)
    {
    case endDataNull:
        assert(false);
        break;
    case endDataEpoch:
    case endDataSet:
        ret = !EnsureDataAvailable(m_mbStartSample);
        break;
    case endDataSentence:  // for fast reader each minibatch is considered a "sentence", so always true
        if (mSentenceEndAt.size() != mToProcess.size())
            LogicError("DataEnd: sentence ending vector size %d and the toprocess vector size %d should be the same", mSentenceEndAt.size(), mToProcess.size());
        ret = true;
        for (size_t i = 0; i < mToProcess.size(); i++)
        {
            if (mSentenceEndAt[i] == NO_LABELS)
            {
                LogicError("BatchLUSequenceReader: minibatch should be large enough to accomodate the longest sentence");
            }
            size_t k = mToProcess[i];
            mProcessed[k] = true;
        }
        break;
    }
    return ret;

}

template<class ElemType>
bool BatchLUSequenceReader<ElemType>::CanReadFor(wstring nodeName)
{
    if (this->m_featuresName == nodeName) return true;
    if (m_labelsName[labelInfoIn] == nodeName) return true;
    if (m_labelsName[labelInfoOut] == nodeName) return true;

    return false;
}

/// get a column slice corresponding to a frame of observations
template<class ElemType>
bool BatchLUSequenceReader<ElemType>::GetFrame(std::map<std::wstring, Matrix<ElemType>*>& matrices, const size_t tidx, vector<size_t>& history)
{

    // get out if they didn't call StartMinibatchLoop() first
    if (m_mbSize == 0)
        return false;

    LabelInfo& labelIn = m_labelInfo[labelInfoIn];

    if (m_labelInfo[labelInfoIn].isproposal)
    {
        const LabelInfo& featInfo = m_labelInfo[labelInfoIn];

        //loop through all the samples
        Matrix<ElemType>& features = *matrices[m_featuresName];
        Matrix<ElemType>  locObs(CPUDEVICE);
        locObs.SwitchToMatrixType(SPARSE, matrixFormatSparseCSC, false);

        if (matrices.find(m_featuresName) == matrices.end())
        {
            RuntimeError("LUSequenceReader cannot find l%s", m_featuresName.c_str());
        }
        locObs.Resize(featInfo.dim * m_wordContext.size(), mBlgSize);

        assert(mBlgSize == 1); /// currently only support one utterance a time

        size_t hlength = history.size();
        int nextProposal = -1;
        if (hlength == 0)
        {
            LabelIdType index;

            index = GetIdFromLabel(m_labelInfo[labelInfoIn].beginSequence.c_str(), labelIn);

            nextProposal = index;
            history.push_back(nextProposal);
        }

        for (size_t j = 0; j < mBlgSize; ++j)
        {
            for (size_t jj = 0; jj < m_wordContext.size(); jj++)
            {
                int cxt = m_wordContext[jj];

                /// assert that wordContext is organized as descending order
                assert((jj == m_wordContext.size() - 1) ? true : cxt > m_wordContext[jj + 1]);

                size_t hidx;
                size_t hlength = history.size();
                if (hlength + cxt > 0)
                    hidx = history[hlength + cxt - 1];
                else
                    hidx = history[0];

                if (matrices.find(m_featuresName) != matrices.end())
                {
                    locObs.SetValue(hidx + jj * featInfo.dim, j, (ElemType)1);
                }
            }
        }

        features.SetValue(locObs);
    }
    else {
        for (typename map<wstring, Matrix<ElemType>>::iterator p = mMatrices.begin(); p != mMatrices.end(); p++)
        {
            assert(mMatrices[p->first].GetNumCols() > tidx);
            if (matrices.find(p->first) != matrices.end())
                matrices[p->first]->SetValue(mMatrices[p->first].ColumnSlice(tidx, mBlgSize));
        }
    }

    // we read some records, so process them
    return true;
}

/// propose labels, return a vector with size larger than 0 if this reader allows proposal
/// otherwise, return a vector with length zero
template<class ElemType>
void BatchLUSequenceReader<ElemType>::InitProposals(map<wstring, Matrix<ElemType>*>& pMat)
{
    if (m_labelInfo[labelInfoIn].isproposal)
    {
        /// no need to save info for labelInfoIn since it is in mProposals
        if (pMat.find(m_labelsName[labelInfoOut]) != pMat.end())
            mMatrices[m_labelsName[labelInfoOut]].SetValue(*(pMat[m_labelsName[labelInfoOut]]));
    }
    else {
        if (pMat.find(m_featuresName) != pMat.end())
            mMatrices[m_featuresName].SetValue(*(pMat[m_featuresName]));
    }
}

template<class ElemType>
void BatchLUSequenceReader<ElemType>::LoadWordMapping(const ConfigParameters& readerConfig)
{
    mWordMappingFn = readerConfig("wordmap", "");
    wstring si, so;
    wstring ss;
    vector<wstring> vs;
    if (mWordMappingFn != "")
    {
        wifstream fp;
        fp.open(mWordMappingFn.c_str(), wifstream::in);

        while (fp.good())
        {
            getline(fp, ss);
            ss = wtrim(ss);
            if (ss.length() == 0)
                break; 
            vs = wsep_string(ss, L" ");
            si = vs[0]; so = vs[1];
            mWordMapping[si] = so;
        }
        fp.close();
    }
    mUnkStr = (wstring)readerConfig("unk", "<unk>");
}


template class BatchLUSequenceReader<double>;
template class BatchLUSequenceReader<float>;

template<class ElemType>
bool MultiIOBatchLUSequenceReader<ElemType>::GetMinibatch(std::map<std::wstring, Matrix<ElemType>*>& matrices)
{
    /// on first iteration, need to check if all requested data matrices are available
    std::map<std::wstring, size_t>::iterator iter;
    if (mCheckDictionaryKeys)
    {
        for (auto iter = matrices.begin(); iter != matrices.end(); iter++)
        {
            bool bFound = false;
            for (typename map<wstring, BatchLUSequenceReader<ElemType>*>::iterator p = mReader.begin(); p != mReader.end(); p++)
            {
                if ((p->second)->CanReadFor(iter->first))
                {
                    nameToReader[iter->first] = p->second;
                    bFound = true;

                    break;
                }
            }
            if (bFound == false)
                RuntimeError("GetMinibatch: cannot find a node that can feed in features for L%s", iter->first.c_str());
        }
        mCheckDictionaryKeys = false;
    }

    /// set the same random seed
    for (typename map<wstring, BatchLUSequenceReader<ElemType>*>::iterator p = mReader.begin(); p != mReader.end(); p++)
    {
        p->second->SetRandomSeed(this->m_seed);
    }
    this->m_seed++;

    /// run for each reader
    for (typename map<wstring, BatchLUSequenceReader<ElemType>*>::iterator p = mReader.begin(); p != mReader.end(); p++)
    {
        if ((p->second)->GetMinibatch(matrices) == false)
            return false;
    }

    return true;
}

/// set the same random seed
template<class ElemType>
void MultiIOBatchLUSequenceReader<ElemType>::SetRandomSeed(int us)
{
    this->m_seed = us;
    for (typename map<wstring, BatchLUSequenceReader<ElemType>*>::iterator p = mReader.begin(); p != mReader.end(); p++)
    {
        p->second->SetRandomSeed(this->m_seed);
    }
}

template<class ElemType>
void MultiIOBatchLUSequenceReader<ElemType>::Init(const ConfigParameters& readerConfig)
{
    ConfigArray ioNames = readerConfig("ioNodeNames", "");
    if (ioNames.size() > 0)
    {
        /// newer code that explicitly place multiple streams for inputs
        foreach_index(i, ioNames) // inputNames should map to node names
        {
            ConfigParameters thisIO = readerConfig(ioNames[i]);

            BatchLUSequenceReader<ElemType> *thisReader = new BatchLUSequenceReader<ElemType>();
            thisReader->Init(thisIO);

            pair<wstring, BatchLUSequenceReader<ElemType>*> pp(ioNames[i], thisReader);

            mReader.insert(pp);
        }
    }
    else{
        /// older code that assumes only one stream of feature
        BatchLUSequenceReader<ElemType> *thisReader = new BatchLUSequenceReader<ElemType>();

        thisReader->Init(readerConfig);

        pair<wstring, BatchLUSequenceReader<ElemType>*> pp(msra::strfun::wstrprintf(L"stream%d", mReader.size()), thisReader);

        mReader.insert(pp);
    }
}

template<class ElemType>
void MultiIOBatchLUSequenceReader<ElemType>::StartMinibatchLoop(size_t mbSize, size_t epoch, size_t requestedEpochSamples)
{
    /// run for each reader
    for (typename map<wstring, BatchLUSequenceReader<ElemType>*>::iterator p = mReader.begin(); p != mReader.end(); p++)
    {
        (p->second)->StartMinibatchLoop(mbSize, epoch, requestedEpochSamples);
    }
}

template<class ElemType>
void MultiIOBatchLUSequenceReader<ElemType>::SetSentenceSegBatch(Matrix<ElemType> & sentenceBegin, vector<MinibatchPackingFlag>& minibatchPackingFlag)
{
    /// run for each reader
    vector<size_t> col;
    size_t rows = 0, cols = 0;
    for (typename map<wstring, BatchLUSequenceReader<ElemType>*>::iterator p = mReader.begin(); p != mReader.end(); p++)
    {
        (p->second)->SetSentenceSegBatch(sentenceBegin, minibatchPackingFlag);
        if (rows == 0)
            rows = sentenceBegin.GetNumRows();
        else
            if (rows != sentenceBegin.GetNumRows())
                LogicError("multiple streams for LU sequence reader must have the same number of rows for sentence begining");
        size_t this_col = sentenceBegin.GetNumCols();
        col.push_back(this_col);
        cols += this_col;
    }
}

template<class ElemType>
size_t MultiIOBatchLUSequenceReader<ElemType>::NumberSlicesInEachRecurrentIter()
{
    return mReader.begin()->second->NumberSlicesInEachRecurrentIter();
}

template<class ElemType>
int MultiIOBatchLUSequenceReader<ElemType>::GetSentenceEndIdFromOutputLabel()
{
    if (mReader.size() != 1)
        LogicError("GetSentenceEndIdFromOutputLabel: support only for one reader in MultiIOBatchLUSequenceReader");
    int iret = -1;

    for (typename map<wstring, BatchLUSequenceReader<ElemType>*>::iterator p = mReader.begin(); p != mReader.end(); p++)
    {
        iret = (p->second)->GetSentenceEndIdFromOutputLabel();
    }
    return iret;
}

template<class ElemType>
bool MultiIOBatchLUSequenceReader<ElemType>::DataEnd(EndDataType endDataType)
{
    bool ret = true;
    for (typename map<wstring, BatchLUSequenceReader<ElemType>*>::iterator p = mReader.begin(); p != mReader.end(); p++)
    {
        ret |= (p->second)->DataEnd(endDataType);
    }
    return ret;
}

/// history is shared
template<class ElemType>
bool MultiIOBatchLUSequenceReader<ElemType>::GetProposalObs(std::map<std::wstring, Matrix<ElemType>*>& matrices, const size_t tidx, vector<size_t>& history)
{
    /// run for each reader
    for (typename map<wstring, BatchLUSequenceReader<ElemType>*>::iterator p = mReader.begin(); p != mReader.end(); p++)
    {
        if ((p->second)->GetFrame(matrices, tidx, history) == false)
        {
            return false;
        }
    }
    return true;
}

/// need to provide initial matrice values if there are
/// these values are from getMinibatch
template<class ElemType>
void MultiIOBatchLUSequenceReader<ElemType>::InitProposals(std::map<std::wstring, Matrix<ElemType>*>& matrices)
{
    /// run for each reader
    for (typename map<wstring, BatchLUSequenceReader<ElemType>*>::iterator p = mReader.begin(); p != mReader.end(); p++)
    {
        (p->second)->InitProposals(matrices);
    }
}

template class MultiIOBatchLUSequenceReader<double>;
template class MultiIOBatchLUSequenceReader<float>;


}}}